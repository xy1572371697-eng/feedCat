#include <linux/module.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/pwm.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/platform_device.h>
#include <linux/jiffies.h>
#include <linux/list.h>

/************************ 全局配置 ************************/
#define DEV_NAME          "motor_platform"
#define DEV_MINOR         0
#define DEV_COUNT         1
#define FEED_TOLERANCE    2       // 喂食误差容忍值(克)
#define BLOCK_DETECT_MS   5000    // 堵转检测超时(ms)
#define WEIGHT_CHECK_INTERVAL 100 // 重量检测间隔(ms)

/************************ 喂食记录结构体 ************************/
struct feed_record {
    unsigned long time;
    int target_gram;
    int actual_gram;
    struct list_head list;
};

/************************ Platform设备私有数据 ************************/
typedef struct {
    // 硬件资源 - 简化：只保留一个控制引脚
    struct pwm_device  *pwm_dev;    // PWM控制速度
    int                gpio_en;      // 使能引脚 (原IN1)
    // 注：IN2固定接地，不再需要GPIO控制
    
    // 运行状态
    int                running;
    int                pwm_freq;
    int                pwm_duty;
    int                max_feed_gram;
    int                feed_count;
    
    // 联动相关
    int                weight_check_en;
    int                block_detected;
    
    // 驱动框架
    struct mutex       mutex;
    dev_t              devid;
    struct cdev        cdev;
    struct class       *class;
    struct device      *device;
    struct device_node *nd;
    struct platform_device *pdev;
    
    // 喂食记录
    struct list_head   feed_list;
    int                record_count;
} motor_platform_dev_t;

static motor_platform_dev_t *motor_dev;

// 声明HX711重量读取接口
extern int weight_get(void);

/************************ 喂食记录管理 ************************/
static void feed_record_add(motor_platform_dev_t *dev, int target, int actual)
{
    struct feed_record *rec = kzalloc(sizeof(*rec), GFP_KERNEL);
    if (!rec) return;

    rec->time = jiffies;
    rec->target_gram = target;
    rec->actual_gram = actual;

    list_add(&rec->list, &dev->feed_list);
    dev->record_count++;
    dev->feed_count++;

    if (dev->record_count > 20) {
        struct feed_record *old = list_last_entry(&dev->feed_list, struct feed_record, list);
        list_del(&old->list);
        kfree(old);
        dev->record_count--;
    }
}

/************************ 底层控制接口 - 简化版 ************************/
 static void motor_set_pwm(motor_platform_dev_t *dev)
{
     u64 period;
     u64 duty_ns;
     if (!dev || !dev->pwm_dev) return;
    
     period = 1000000000ULL / dev->pwm_freq;
     duty_ns = period * dev->pwm_duty / 100;
     pwm_config(dev->pwm_dev, duty_ns, period);
 }

static void motor_start(motor_platform_dev_t *dev)
{
    if (!dev) return;
    
    mutex_lock(&dev->mutex);
    if (dev->running) {
        mutex_unlock(&dev->mutex);
        return;
    }
    
    // 使能控制引脚
    gpio_set_value(dev->gpio_en, 1);
    // 使能PWM
    pwm_enable(dev->pwm_dev);
    
    dev->running = 1;
    dev->block_detected = 0;
    mutex_unlock(&dev->mutex);
    
    pr_info("[motor] 电机启动 | 占空比: %d%%\n", dev->pwm_duty);
}

static void motor_stop(motor_platform_dev_t *dev)
{
    if (!dev) return;
    
    mutex_lock(&dev->mutex);
    if (!dev->running) {
        mutex_unlock(&dev->mutex);
        return;
    }
    
    // 禁用PWM
    pwm_disable(dev->pwm_dev);
    // 关闭控制引脚
    gpio_set_value(dev->gpio_en, 0);
    
    dev->running = 0;
    mutex_unlock(&dev->mutex);
    
    pr_info("[motor] 电机停止\n");
}

/************************ 核心功能：定速旋转+重量变化停止 ************************/
void motor_feed(int g)
{
    int real_feed;
    int weight_before, weight_after, weight_change = 0;
    unsigned long start_jiffies, current_jiffies;
    int check_count = 0;

    if (!motor_dev || g <= 0) {
        pr_err("[motor] feed err: invalid dev or gram %d\n", g);
        return;
    }

    weight_before = weight_get();
    if (weight_before < 0) {
        pr_err("[motor] feed err: weight sensor read fail\n");
        return;
    }
    
    pr_info("[motor] 喂食启动 | 目标：%d克 | 初始重量：%d克\n", g, weight_before);

    // 启动电机
    motor_start(motor_dev);
    start_jiffies = jiffies;

    // 循环检测重量变化
    while (1) {
        // 堵转超时保护
        current_jiffies = jiffies;
        if (jiffies_to_msecs(current_jiffies - start_jiffies) > BLOCK_DETECT_MS) {
            motor_stop(motor_dev);
            motor_dev->block_detected = 1;
            pr_err("[motor] 电机超时停止（%dms）\n", BLOCK_DETECT_MS);
            return;
        }

        msleep(WEIGHT_CHECK_INTERVAL);
        weight_after = weight_get();
        if (weight_after < 0) continue;

        weight_change = weight_before - weight_after;
        weight_change = weight_change < 0 ? 0 : weight_change;
        check_count++;

        pr_debug("[motor] 重量变化：%d克 / 目标%d克\n", weight_change, g);

        if (weight_change >= (g - FEED_TOLERANCE)) {
            break;
        }
    }

    motor_stop(motor_dev);

    weight_after = weight_get();
    real_feed = weight_before - weight_after;
    real_feed = real_feed < 0 ? 0 : real_feed;
    
    pr_info("[motor] 喂食完成 | 目标：%d克 | 实际：%d克\n", g, real_feed);
    
    // 记录喂食
    feed_record_add(motor_dev, g, real_feed);
}
EXPORT_SYMBOL_GPL(motor_feed);

/************************ 字符设备操作集 ************************/
static int motor_open(struct inode *inode, struct file *filp)
{
    motor_platform_dev_t *dev = container_of(inode->i_cdev, motor_platform_dev_t, cdev);
    filp->private_data = dev;
    return 0;
}

static ssize_t motor_read(struct file *filp, char __user *buf, size_t count, loff_t *ppos)
{
    motor_platform_dev_t *dev = filp->private_data;
    char info_buf[256] = {0};
    int len;

    len = snprintf(info_buf, sizeof(info_buf), 
        "FeedCount:%d|MaxFeed:%d|PWMDuty:%d|BlockDetected:%d\n",
        dev->feed_count, dev->max_feed_gram, dev->pwm_duty,
        dev->block_detected);

    if (copy_to_user(buf, info_buf, len)) {
        return -EFAULT;
    }
    return len;
}

static ssize_t motor_write(struct file *filp, const char __user *buf, size_t count, loff_t *ppos)
{
    motor_platform_dev_t *dev = filp->private_data;
    char cmd_buf[64] = {0};
    int g;

    if (copy_from_user(cmd_buf, buf, min(count, sizeof(cmd_buf)-1))) {
        return -EFAULT;
    }

    if (sscanf(cmd_buf, "FEED:%d", &g) == 1) {
        motor_feed(g);
    } else if (strcmp(cmd_buf, "STOP") == 0) {
        motor_stop(dev);
    }

    return count;
}

static int motor_release(struct inode *inode, struct file *filp)
{
    motor_platform_dev_t *dev = filp->private_data;
    motor_stop(dev);
    return 0;
}

static const struct file_operations motor_fops = {
    .owner      = THIS_MODULE,
    .open       = motor_open,
    .read       = motor_read,
    .write      = motor_write,
    .release    = motor_release,
};

/************************ 设备树解析 - 简化版 ************************/
static int motor_parse_dts(motor_platform_dev_t *dev)
{
    int ret;
    u32 val;

    dev->nd = dev->pdev->dev.of_node;
    if (!dev->nd) {
        pr_err("[motor] no device tree node\n");
        return -EINVAL;
    }

    // 1. 只读取一个使能引脚 (原IN1)
    dev->gpio_en = of_get_named_gpio(dev->nd, "motor-en-gpios", 0);
    if (!gpio_is_valid(dev->gpio_en)) {
        pr_err("[motor] get motor enable gpio failed: %d\n", dev->gpio_en);
        return -EINVAL;
    }
    pr_info("[motor] get enable gpio: %d\n", dev->gpio_en);

    // 2. 申请GPIO
    ret = gpio_request(dev->gpio_en, "motor-en");
    if (ret < 0) {
        pr_err("[motor] request enable gpio failed: %d\n", ret);
        return ret;
    }
    gpio_direction_output(dev->gpio_en, 0);

    // 3. 获取PWM设备
    dev->pwm_dev = of_pwm_get(dev->nd, "motor-pwm");
    if (IS_ERR(dev->pwm_dev)) {
        ret = PTR_ERR(dev->pwm_dev);
        pr_err("[motor] get pwm failed: %d\n", ret);
        gpio_free(dev->gpio_en);
        return ret;
    }
    pr_info("[motor] get pwm success\n");

    // 4. 读取参数
    ret = of_property_read_u32(dev->nd, "motor-pwm-freq", &val);
    dev->pwm_freq = ret < 0 ? 1000 : val;

    ret = of_property_read_u32(dev->nd, "motor-pwm-duty", &val);
    dev->pwm_duty = ret < 0 ? 50 : val;

    ret = of_property_read_u32(dev->nd, "motor-max-feed-gram", &val);
    dev->max_feed_gram = ret < 0 ? 50 : val;

    dev->weight_check_en = 1;
    dev->feed_count = 0;
    dev->record_count = 0;
    INIT_LIST_HEAD(&dev->feed_list);

    pr_info("[motor] 参数: freq=%d, duty=%d%%, max=%d\n",
            dev->pwm_freq, dev->pwm_duty, dev->max_feed_gram);

    return 0;
}

/************************ Platform驱动核心接口 ************************/
static int motor_platform_probe(struct platform_device *pdev)
{
    int ret;
    motor_platform_dev_t *dev;

    dev = kzalloc(sizeof(motor_platform_dev_t), GFP_KERNEL);
    if (!dev) {
        pr_err("[motor] alloc dev data failed\n");
        return -ENOMEM;
    }
    
    dev->pdev = pdev;
    mutex_init(&dev->mutex);
    motor_dev = dev;

    ret = motor_parse_dts(dev);
    if (ret < 0) {
        kfree(dev);
        return ret;
    }

    ret = alloc_chrdev_region(&dev->devid, DEV_MINOR, DEV_COUNT, DEV_NAME);
    if (ret < 0) {
        pr_err("[motor] alloc devid failed: %d\n", ret);
        goto err_alloc;
    }

    cdev_init(&dev->cdev, &motor_fops);
    dev->cdev.owner = THIS_MODULE;
    ret = cdev_add(&dev->cdev, dev->devid, DEV_COUNT);
    if (ret < 0) {
        pr_err("[motor] add cdev failed: %d\n", ret);
        goto err_cdev;
    }

    dev->class = class_create(THIS_MODULE, "motor_platform_class");
    if (IS_ERR(dev->class)) {
        ret = PTR_ERR(dev->class);
        pr_err("[motor] create class failed: %d\n", ret);
        goto err_class;
    }

    dev->device = device_create(dev->class, &pdev->dev, dev->devid, NULL, DEV_NAME);
    if (IS_ERR(dev->device)) {
        ret = PTR_ERR(dev->device);
        pr_err("[motor] create device failed: %d\n", ret);
        goto err_device;
    }

    platform_set_drvdata(pdev, dev);
    pr_info("[motor] probe success | dev: /dev/%s\n", DEV_NAME);
    return 0;

err_device:
    class_destroy(dev->class);
err_class:
    cdev_del(&dev->cdev);
err_cdev:
    unregister_chrdev_region(dev->devid, DEV_COUNT);
err_alloc:
    pwm_free(dev->pwm_dev);
    gpio_free(dev->gpio_en);
    kfree(dev);
    motor_dev = NULL;
    return ret;
}

static int motor_platform_remove(struct platform_device *pdev)
{
    motor_platform_dev_t *dev = platform_get_drvdata(pdev);
    struct feed_record *rec, *tmp;

    if (dev) {
        motor_stop(dev);

        list_for_each_entry_safe(rec, tmp, &dev->feed_list, list) {
            list_del(&rec->list);
            kfree(rec);
        }

        device_destroy(dev->class, dev->devid);
        class_destroy(dev->class);
        cdev_del(&dev->cdev);
        unregister_chrdev_region(dev->devid, DEV_COUNT);
        pwm_free(dev->pwm_dev);
        gpio_free(dev->gpio_en);
        kfree(dev);
    }
    motor_dev = NULL;

    return 0;
}

/************************ Platform驱动匹配表 ************************/
static const struct of_device_id motor_of_match[] = {
    { .compatible = "alientek,motor-platform" },
    { /* Sentinel */ }
};
MODULE_DEVICE_TABLE(of, motor_of_match);

static struct platform_driver motor_platform_driver = {
    .driver = {
        .name = "motor-platform-driver",
        .of_match_table = motor_of_match,
        .owner = THIS_MODULE,
    },
    .probe = motor_platform_probe,
    .remove = motor_platform_remove,
};
/************************ 驱动入口/出口 ************************/
static int __init motor_drv_init(void)
{
    return platform_driver_register(&motor_platform_driver);
}

static void __exit motor_drv_exit(void)
{
    platform_driver_unregister(&motor_platform_driver);
}
module_init(motor_drv_init);
module_exit(motor_drv_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("IMX6U Motor Driver (Single Pin Control)");
MODULE_AUTHOR("Developer");
MODULE_VERSION("V8.0");