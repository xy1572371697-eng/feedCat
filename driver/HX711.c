#include <linux/module.h>
#include <linux/fs.h>
#include <linux/gpio.h>
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
#include <linux/types.h>
#include <linux/debugfs.h>
#include <linux/atomic.h>

/************************ 全局配置 ************************/
#define WEIGHT_DEV_NAME      "weight_sensor"
#define WEIGHT_DEV_MINOR     1
#define WEIGHT_DEV_COUNT     1
#define HX711_CHANNEL        1
#define HX711_DELAY_US       10

/************************ 重量检测私有数据 ************************/
typedef struct {
    int                gpio_dt;
    int                gpio_sck;
    int                calibration;    /* 校准系数 */
    int                offset;         /* 零点偏移 */
    int                low_threshold;
    int                raw_data;       /* 最近原始值(24位) */
    int                current_weight;
    struct mutex       mutex;
    dev_t              devid;
    struct cdev        cdev;
    struct class       *class;
    struct device      *device;
    struct device_node *nd;
    struct platform_device *pdev;
    /* Debugfs: 运行时诊断接口 */
    struct dentry      *debugfs_dir;
    atomic_t           sample_count;   /* 累计采样次数 */
    atomic_t           error_count;    /* 累计采样错误次数 */
    u32                last_raw_u32;   /* debugfs 只读原始值 */
} weight_platform_dev_t;

static weight_platform_dev_t *weight_dev;

/************************ HX711底层驱动接口 ************************/
static int hx711_read_raw(weight_platform_dev_t *dev)
{
    int i, j;
    int value = 0;
    unsigned char data[3] = {0};

    if (!dev) return -1;

    mutex_lock(&dev->mutex);
    if (gpio_get_value(dev->gpio_dt) != 0) {
        mutex_unlock(&dev->mutex);
        pr_err("[weight] HX711 data not ready\n");
        return -1;
    }

    for (j = 0; j < 3; j++) {
        for (i = 7; i >= 0; i--) {
            gpio_set_value(dev->gpio_sck, 1);
            udelay(HX711_DELAY_US);
            data[j] |= (gpio_get_value(dev->gpio_dt) << i);
            gpio_set_value(dev->gpio_sck, 0);
            udelay(HX711_DELAY_US);
        }
    }

    for (i = 0; i < HX711_CHANNEL; i++) {
        gpio_set_value(dev->gpio_sck, 1);
        udelay(HX711_DELAY_US);
        gpio_set_value(dev->gpio_sck, 0);
        udelay(HX711_DELAY_US);
    }
    mutex_unlock(&dev->mutex);

    value = (data[0] << 16) | (data[1] << 8) | data[2];
    if (value & 0x800000)
        value |= 0xFF000000;

    dev->raw_data    = value;
    dev->last_raw_u32 = (u32)value;
    atomic_inc(&dev->sample_count);
    return value;
}

int weight_get(void)
{
    int raw;
    int weight;

    if (!weight_dev) return -1;

    raw = hx711_read_raw(weight_dev);
    if (raw < 0) {
        atomic_inc(&weight_dev->error_count);
        return -1;
    }

    weight = (raw - weight_dev->offset) / weight_dev->calibration;
    weight = weight < 0 ? 0 : weight;
    weight_dev->current_weight = weight;

    if (weight < weight_dev->low_threshold)
        pr_warn("[weight] 缺粮警告！当前重量：%d克（阈值：%d克）\n",
                weight, weight_dev->low_threshold);

    pr_info("[weight] 实时重量：%d克（原始值：%d）\n", weight, raw);
    return weight;
}
EXPORT_SYMBOL_GPL(weight_get);

static void weight_calib_zero(weight_platform_dev_t *dev)
{
    int i;
    int sum = 0;
    int count = 10;

    if (!dev) return;

    mutex_lock(&dev->mutex);
    for (i = 0; i < count; i++) {
        sum += hx711_read_raw(dev);
        msleep(10);
    }
    dev->offset = sum / count;
    mutex_unlock(&dev->mutex);
    pr_info("[weight] 零点校准完成，偏移值：%d\n", dev->offset);
}

static void weight_set_calibration(weight_platform_dev_t *dev, int cali)
{
    if (!dev || cali <= 0) return;
    dev->calibration = cali;
    pr_info("[weight] 校准系数已设置：%d\n", cali);
}

/************************ Debugfs 接口 ************************/
/*
 * 通过 Debugfs 向用户态暴露驱动内部状态，无需修改业务代码即可运行时诊断。
 * 查看方式：
 *   cat /sys/kernel/debug/hx711_weight/raw_adc        # 最近一次 ADC 原始值
 *   cat /sys/kernel/debug/hx711_weight/calibration    # 当前校准系数
 *   cat /sys/kernel/debug/hx711_weight/offset         # 零点偏移
 *   cat /sys/kernel/debug/hx711_weight/sample_count   # 累计采样次数
 *   cat /sys/kernel/debug/hx711_weight/error_count    # 采样失败次数
 */
static void weight_debugfs_init(weight_platform_dev_t *dev)
{
    dev->debugfs_dir = debugfs_create_dir("hx711_weight", NULL);
    if (IS_ERR_OR_NULL(dev->debugfs_dir)) {
        pr_warn("[weight] debugfs init failed\n");
        dev->debugfs_dir = NULL;
        return;
    }
    debugfs_create_u32("raw_adc",        0444, dev->debugfs_dir, &dev->last_raw_u32);
    debugfs_create_u32("calibration",    0444, dev->debugfs_dir, (u32 *)&dev->calibration);
    debugfs_create_u32("offset",         0444, dev->debugfs_dir, (u32 *)&dev->offset);
    debugfs_create_u32("low_threshold",  0444, dev->debugfs_dir, (u32 *)&dev->low_threshold);
    debugfs_create_u32("current_weight", 0444, dev->debugfs_dir, (u32 *)&dev->current_weight);
    debugfs_create_atomic_t("sample_count", 0444, dev->debugfs_dir, &dev->sample_count);
    debugfs_create_atomic_t("error_count",  0444, dev->debugfs_dir, &dev->error_count);
    pr_info("[weight] debugfs ready: /sys/kernel/debug/hx711_weight/\n");
}

static void weight_debugfs_remove(weight_platform_dev_t *dev)
{
    if (dev->debugfs_dir)
        debugfs_remove_recursive(dev->debugfs_dir);
}

/************************ 字符设备操作集 ************************/
static int weight_open(struct inode *inode, struct file *filp)
{
    weight_platform_dev_t *dev = container_of(inode->i_cdev, weight_platform_dev_t, cdev);
    filp->private_data = dev;
    pr_info("[weight] device open\n");
    return 0;
}

static ssize_t weight_read(struct file *filp, char __user *buf, size_t count, loff_t *ppos)
{
    weight_platform_dev_t *dev = filp->private_data;
    char weight_buf[32] = {0};
    int weight;

    if (!dev) return -EIO;

    weight = weight_get();
    if (weight < 0) return -EIO;

    snprintf(weight_buf, sizeof(weight_buf), "%d\n", weight);
    if (copy_to_user(buf, weight_buf, strlen(weight_buf)))
        return -EFAULT;

    return strlen(weight_buf);
}

static ssize_t weight_write(struct file *filp, const char __user *buf, size_t count, loff_t *ppos)
{
    weight_platform_dev_t *dev = filp->private_data;
    char cmd_buf[64] = {0};
    int val;

    if (!dev) return -EIO;

    if (copy_from_user(cmd_buf, buf, min(count, sizeof(cmd_buf)-1)))
        return -EFAULT;

    if (strcmp(cmd_buf, "ZERO") == 0) {
        weight_calib_zero(dev);
    } else if (sscanf(cmd_buf, "CALI:%d", &val) == 1) {
        weight_set_calibration(dev, val);
    } else if (sscanf(cmd_buf, "THRESH:%d", &val) == 1) {
        dev->low_threshold = val > 0 ? val : 50;
        pr_info("[weight] 缺粮阈值已设置：%d克\n", dev->low_threshold);
    } else {
        pr_err("[weight] 无效命令：%s\n", cmd_buf);
        return -EINVAL;
    }

    return count;
}

static int weight_release(struct inode *inode, struct file *filp)
{
    pr_info("[weight] device release\n");
    return 0;
}

static const struct file_operations weight_fops = {
    .owner      = THIS_MODULE,
    .open       = weight_open,
    .read       = weight_read,
    .write      = weight_write,
    .release    = weight_release,
};

/************************ 设备树解析 ************************/
static int weight_parse_dts(weight_platform_dev_t *dev)
{
    int ret;
    u32 val;

    dev->nd = dev->pdev->dev.of_node;
    if (!dev->nd) {
        pr_err("[weight] no device tree node\n");
        return -EINVAL;
    }

    dev->gpio_dt  = of_get_named_gpio(dev->nd, "weight-dt-gpios", 0);
    dev->gpio_sck = of_get_named_gpio(dev->nd, "weight-sck-gpios", 0);
    if (!gpio_is_valid(dev->gpio_dt) || !gpio_is_valid(dev->gpio_sck)) {
        pr_err("[weight] get gpio failed | dt:%d, sck:%d\n", dev->gpio_dt, dev->gpio_sck);
        return -EINVAL;
    }

    ret = gpio_request(dev->gpio_dt, "weight-dt");
    if (ret < 0) { pr_err("[weight] request dt gpio failed\n"); return ret; }
    ret = gpio_request(dev->gpio_sck, "weight-sck");
    if (ret < 0) { gpio_free(dev->gpio_dt); return ret; }

    gpio_direction_input(dev->gpio_dt);
    gpio_direction_output(dev->gpio_sck, 0);

    ret = of_property_read_u32(dev->nd, "weight-calibration", &val);
    dev->calibration = ret < 0 ? 430 : val;

    ret = of_property_read_u32(dev->nd, "weight-offset", &val);
    dev->offset = ret < 0 ? 0 : val;

    ret = of_property_read_u32(dev->nd, "weight-low-threshold", &val);
    dev->low_threshold = ret < 0 ? 50 : val;

    pr_info("[weight] params: cali=%d, offset=%d, threshold=%d\n",
            dev->calibration, dev->offset, dev->low_threshold);
    return 0;
}

/************************ Platform驱动核心接口 ************************/
static int weight_platform_probe(struct platform_device *pdev)
{
    int ret;
    weight_platform_dev_t *dev;

    pr_info("[weight] platform probe start\n");

    dev = kzalloc(sizeof(weight_platform_dev_t), GFP_KERNEL);
    if (!dev) return -ENOMEM;

    dev->pdev = pdev;
    mutex_init(&dev->mutex);
    atomic_set(&dev->sample_count, 0);
    atomic_set(&dev->error_count, 0);
    weight_dev = dev;

    ret = weight_parse_dts(dev);
    if (ret < 0) { kfree(dev); return ret; }

    ret = alloc_chrdev_region(&dev->devid, WEIGHT_DEV_MINOR, WEIGHT_DEV_COUNT, WEIGHT_DEV_NAME);
    if (ret < 0) { pr_err("[weight] alloc devid failed\n"); goto err_alloc; }

    cdev_init(&dev->cdev, &weight_fops);
    dev->cdev.owner = THIS_MODULE;
    ret = cdev_add(&dev->cdev, dev->devid, WEIGHT_DEV_COUNT);
    if (ret < 0) { pr_err("[weight] cdev_add failed\n"); goto err_cdev; }

    dev->class = class_create(THIS_MODULE, "weight_platform_class");
    if (IS_ERR(dev->class)) { ret = PTR_ERR(dev->class); goto err_class; }

    dev->device = device_create(dev->class, &pdev->dev, dev->devid, NULL, WEIGHT_DEV_NAME);
    if (IS_ERR(dev->device)) { ret = PTR_ERR(dev->device); goto err_device; }

    platform_set_drvdata(pdev, dev);
    weight_get();

    /* 注册 Debugfs 接口，导出 ADC 原始值、校准参数、采样统计供运行时诊断 */
    weight_debugfs_init(dev);

    pr_info("[weight] platform probe success | dev: /dev/%s\n", WEIGHT_DEV_NAME);
    return 0;

err_device:
    class_destroy(dev->class);
err_class:
    cdev_del(&dev->cdev);
err_cdev:
    unregister_chrdev_region(dev->devid, WEIGHT_DEV_COUNT);
err_alloc:
    gpio_free(dev->gpio_sck);
    gpio_free(dev->gpio_dt);
    kfree(dev);
    weight_dev = NULL;
    return ret;
}

static int weight_platform_remove(struct platform_device *pdev)
{
    weight_platform_dev_t *dev = platform_get_drvdata(pdev);
    pr_info("[weight] platform remove start\n");

    if (dev) {
        weight_debugfs_remove(dev);
        device_destroy(dev->class, dev->devid);
        class_destroy(dev->class);
        cdev_del(&dev->cdev);
        unregister_chrdev_region(dev->devid, WEIGHT_DEV_COUNT);
        gpio_free(dev->gpio_sck);
        gpio_free(dev->gpio_dt);
        kfree(dev);
    }
    weight_dev = NULL;
    pr_info("[weight] platform remove success\n");
    return 0;
}

/************************ Platform驱动匹配表 ************************/
static const struct of_device_id weight_of_match[] = {
    { .compatible = "alientek,weight-platform" },
    { /* Sentinel */ }
};
MODULE_DEVICE_TABLE(of, weight_of_match);

static struct platform_driver weight_platform_driver = {
    .driver = {
        .name = "weight-platform-driver",
        .of_match_table = weight_of_match,
        .owner = THIS_MODULE,
    },
    .probe  = weight_platform_probe,
    .remove = weight_platform_remove,
};

static int __init weight_drv_init(void)
{
    return platform_driver_register(&weight_platform_driver);
}

static void __exit weight_drv_exit(void)
{
    platform_driver_unregister(&weight_platform_driver);
}

module_init(weight_drv_init);
module_exit(weight_drv_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("IMX6U HX711 Weight Sensor Driver with Debugfs");
MODULE_AUTHOR("Developer");
MODULE_VERSION("V3.0");
