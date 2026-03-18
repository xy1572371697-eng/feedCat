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

/************************ 全局配置 ************************/
#define IR_DEV_NAME       "ir_obstacle"
#define IR_DEV_MINOR      2       // 次设备号（与电机0、重量1区分，避免冲突）
#define IR_DEV_COUNT      1
#define DEFAULT_CHECK_INTERVAL 50  // 默认检测间隔(ms)
#define DEFAULT_DEBOUNCE_TIME 20   // 默认防抖时间(ms)

/************************ 红外避障私有数据 ************************/
typedef struct {
    // 硬件资源
    int                gpio_ir_out;      // 红外传感器OUT引脚
    // 配置参数
    int                ir_reverse;       // 检测逻辑反向（0=默认，1=反向）
    int                check_interval;   // 检测间隔(ms)
    int                debounce_time;    // 防抖时间(ms)
    // 运行状态
    int                obstacle_state;   // 遮挡状态（0=无遮挡，1=有遮挡）
    unsigned long      last_check_time;  // 上次检测时间(jiffies)
    // 驱动框架
    struct mutex       mutex;
    dev_t              devid;
    struct cdev        cdev;
    struct class       *class;
    struct device      *device;
    struct device_node *nd;
    struct platform_device *pdev;
} ir_obstacle_dev_t;

static ir_obstacle_dev_t *ir_dev;

/************************ 核心检测接口（导出供电机驱动调用） ************************/
/**
 * @brief  检测红外避障状态（供电机驱动联动调用）
 * @return 0=无遮挡，1=有遮挡，负数=检测失败
 */
int ir_obstacle_check(void)
{
    static int last_state;
    int val;
    unsigned long current_time;

    if (!ir_dev) {
        pr_err("[ir] 红外驱动未初始化，检测失败\n");
        return -1;
    }

    mutex_lock(&ir_dev->mutex);
    current_time = jiffies;

    // 防抖：检测间隔内不重复检测，避免频繁读取GPIO
    if (jiffies_to_msecs(current_time - ir_dev->last_check_time) < ir_dev->check_interval) {
        mutex_unlock(&ir_dev->mutex);
        return ir_dev->obstacle_state;
    }

    // 读取GPIO电平（红外传感器输出）
    val = gpio_get_value(ir_dev->gpio_ir_out);

    // 防抖处理：连续读取2次，间隔防抖时间，确保状态稳定
    msleep(ir_dev->debounce_time);
    if (val != gpio_get_value(ir_dev->gpio_ir_out)) {
        mutex_unlock(&ir_dev->mutex);
        return ir_dev->obstacle_state; // 状态不稳定，返回上次状态
    }

    // 处理检测逻辑反向
    if (ir_dev->ir_reverse) {
        ir_dev->obstacle_state = (val == 0) ? 1 : 0; // 反向：低电平=有遮挡
    } else {
        ir_dev->obstacle_state = (val == 1) ? 1 : 0; // 默认：高电平=有遮挡（可根据传感器调整）
    }

    // 更新上次检测时间
    ir_dev->last_check_time = current_time;

    // 遮挡日志提醒（仅状态变化时输出）
    last_state = -1;
    if (ir_dev->obstacle_state != last_state) {
        if (ir_dev->obstacle_state) {
            pr_warn("[ir] 检测到遮挡！\n");
        } else {
            pr_info("[ir] 遮挡解除，恢复正常\n");
        }
        last_state = ir_dev->obstacle_state;
    }

    mutex_unlock(&ir_dev->mutex);
    return ir_dev->obstacle_state;
}
EXPORT_SYMBOL_GPL(ir_obstacle_check); // 导出接口，供电机驱动联动

/************************ 字符设备操作集 ************************/
static int ir_open(struct inode *inode, struct file *filp)
{
    ir_obstacle_dev_t *dev = container_of(inode->i_cdev, ir_obstacle_dev_t, cdev);
    filp->private_data = dev;
    pr_info("[ir] device open\n");
    return 0;
}

static ssize_t ir_read(struct file *filp, char __user *buf, size_t count, loff_t *ppos)
{
    ir_obstacle_dev_t *dev = filp->private_data;
    char state_buf[4] = {0};  // 足够存放 "1\n" 或 "0\n"
    int len;

    // 只返回遮挡状态（0或1），加换行符方便应用层读取
    len = snprintf(state_buf, sizeof(state_buf), "%d\n", dev->obstacle_state);

    if (copy_to_user(buf, state_buf, len)) {
        return -EFAULT;
    }
    return len;
}

static ssize_t ir_write(struct file *filp, const char __user *buf, size_t count, loff_t *ppos)
{
    ir_obstacle_dev_t *dev = filp->private_data;
    char cmd_buf[64] = {0};
    int val;

    if (copy_from_user(cmd_buf, buf, min(count, sizeof(cmd_buf)-1))) {
        return -EFAULT;
    }

    // 支持的命令：校准参数、手动检测
    if (sscanf(cmd_buf, "REVERSE:%d", &val) == 1) {
        dev->ir_reverse = (val == 1) ? 1 : 0;
        pr_info("[ir] 检测逻辑反向设置：%d（0=默认，1=反向）\n", dev->ir_reverse);
    } else if (sscanf(cmd_buf, "INTERVAL:%d", &val) == 1) {
        dev->check_interval = val > 10 ? val : DEFAULT_CHECK_INTERVAL; // 最小10ms
        pr_info("[ir] 检测间隔设置：%d ms\n", dev->check_interval);
    } else if (sscanf(cmd_buf, "DEBOUNCE:%d", &val) == 1) {
        dev->debounce_time = val > 5 ? val : DEFAULT_DEBOUNCE_TIME; // 最小5ms
        pr_info("[ir] 防抖时间设置：%d ms\n", dev->debounce_time);
    } else if (strcmp(cmd_buf, "CHECK") == 0) {
        // 手动触发一次检测
        int state = ir_obstacle_check();
        pr_info("[ir] 手动检测结果：%d（0=无遮挡，1=有遮挡）\n", state);
    } else {
        pr_err("[ir] 无效命令：%s\n", cmd_buf);
        return -EINVAL;
    }

    return count;
}

static int ir_release(struct inode *inode, struct file *filp)
{
    pr_info("[ir] device release\n");
    return 0;
}

static const struct file_operations ir_fops = {
    .owner      = THIS_MODULE,
    .open       = ir_open,
    .read       = ir_read,
    .write      = ir_write,
    .release    = ir_release,
};

/************************ 设备树解析 ************************/
static int ir_parse_dts(ir_obstacle_dev_t *dev)
{
    int ret;
    u32 val;

    dev->nd = dev->pdev->dev.of_node;
    if (!dev->nd) {
        pr_err("[ir] no device tree node\n");
        return -EINVAL;
    }

    // 1. 读取红外传感器OUT引脚
    dev->gpio_ir_out = of_get_named_gpio(dev->nd, "ir-out-gpios", 0);
    if (!gpio_is_valid(dev->gpio_ir_out)) {
        pr_err("[ir] get ir out gpio failed: %d\n", dev->gpio_ir_out);
        return -EINVAL;
    }
    pr_info("[ir] get ir out gpio: %d\n", dev->gpio_ir_out);

    // 2. 申请GPIO（配置为输入，读取传感器输出电平）
    ret = gpio_request(dev->gpio_ir_out, "ir-out");
    if (ret < 0) {
        pr_err("[ir] request ir out gpio failed: %d\n", ret);
        return ret;
    }
    gpio_direction_input(dev->gpio_ir_out); // OUT引脚为输入（MCU读取传感器状态）

    // 3. 读取设备树配置参数（无配置则用默认值）
    ret = of_property_read_u32(dev->nd, "ir-reverse", &val);
    dev->ir_reverse = ret < 0 ? 0 : val;

    ret = of_property_read_u32(dev->nd, "ir-check-interval", &val);
    dev->check_interval = ret < 0 ? DEFAULT_CHECK_INTERVAL : val;
    dev->check_interval = dev->check_interval < 10 ? DEFAULT_CHECK_INTERVAL : dev->check_interval;

    ret = of_property_read_u32(dev->nd, "ir-debounce-time", &val);
    dev->debounce_time = ret < 0 ? DEFAULT_DEBOUNCE_TIME : val;
    dev->debounce_time = dev->debounce_time < 5 ? DEFAULT_DEBOUNCE_TIME : dev->debounce_time;

    // 初始化状态
    dev->obstacle_state = 0;
    dev->last_check_time = jiffies;

    pr_info("[ir] params: reverse=%d, interval=%dms, debounce=%dms\n",
            dev->ir_reverse, dev->check_interval, dev->debounce_time);

    return 0;
}

/************************ Platform驱动核心接口 ************************/
static int ir_platform_probe(struct platform_device *pdev)
{
    int ret;
    ir_obstacle_dev_t *dev;

    pr_info("[ir] platform probe start\n");

    // 1. 分配私有数据内存
    dev = kzalloc(sizeof(ir_obstacle_dev_t), GFP_KERNEL);
    if (!dev) {
        pr_err("[ir] alloc dev data failed\n");
        return -ENOMEM;
    }
    dev->pdev = pdev;
    mutex_init(&dev->mutex);
    ir_dev = dev;

    // 2. 解析设备树
    ret = ir_parse_dts(dev);
    if (ret < 0) {
        pr_err("[ir] parse dts failed: %d\n", ret);
        kfree(dev);
        return ret;
    }

    // 3. 动态分配设备号（与电机、重量驱动区分，避免冲突）
    ret = alloc_chrdev_region(&dev->devid, IR_DEV_MINOR, IR_DEV_COUNT, IR_DEV_NAME);
    if (ret < 0) {
        pr_err("[ir] alloc devid failed: %d\n", ret);
        goto err_alloc;
    }
    pr_info("[ir] devid: major=%d, minor=%d\n", MAJOR(dev->devid), MINOR(dev->devid));

    // 4. 初始化cdev
    cdev_init(&dev->cdev, &ir_fops);
    dev->cdev.owner = THIS_MODULE;
    ret = cdev_add(&dev->cdev, dev->devid, IR_DEV_COUNT);
    if (ret < 0) {
        pr_err("[ir] add cdev failed: %d\n", ret);
        goto err_cdev;
    }

    // 5. 创建设备类和节点
    dev->class = class_create(THIS_MODULE, "ir_obstacle_class");
    if (IS_ERR(dev->class)) {
        ret = PTR_ERR(dev->class);
        pr_err("[ir] create class failed: %d\n", ret);
        goto err_class;
    }

    dev->device = device_create(dev->class, &pdev->dev, dev->devid, NULL, IR_DEV_NAME);
    if (IS_ERR(dev->device)) {
        ret = PTR_ERR(dev->device);
        pr_err("[ir] create device failed: %d\n", ret);
        goto err_device;
    }

    // 6. 绑定私有数据到Platform设备
    platform_set_drvdata(pdev, dev);

    // 初始化检测一次状态
    ir_obstacle_check();

    pr_info("[ir] platform probe success | dev: /dev/%s\n", IR_DEV_NAME);
    return 0;

err_device:
    class_destroy(dev->class);
err_class:
    cdev_del(&dev->cdev);
err_cdev:
    unregister_chrdev_region(dev->devid, IR_DEV_COUNT);
err_alloc:
    gpio_free(dev->gpio_ir_out);
    kfree(dev);
    ir_dev = NULL;
    return ret;
}

static int ir_platform_remove(struct platform_device *pdev)
{
    ir_obstacle_dev_t *dev = platform_get_drvdata(pdev);
    pr_info("[ir] platform remove start\n");

    if (dev) {
        device_destroy(dev->class, dev->devid);
        class_destroy(dev->class);
        cdev_del(&dev->cdev);
        unregister_chrdev_region(dev->devid, IR_DEV_COUNT);
        gpio_free(dev->gpio_ir_out);
        kfree(dev);
    }
    ir_dev = NULL;

    pr_info("[ir] platform remove success\n");
    return 0;
}

/************************ Platform驱动匹配表 ************************/
static const struct of_device_id ir_of_match[] = {
    { .compatible = "alientek,ir-obstacle-platform" }, // 与设备树compatible匹配
    { /* Sentinel */ }
};
MODULE_DEVICE_TABLE(of, ir_of_match);

static struct platform_driver ir_platform_driver = {
    .driver = {
        .name = "ir-obstacle-platform-driver",
        .of_match_table = ir_of_match,
        .owner = THIS_MODULE,
    },
    .probe = ir_platform_probe,
    .remove = ir_platform_remove,
};

/************************ 驱动入口/出口 ************************/
static int __init ir_drv_init(void)
{
    // 注册Platform驱动
    return platform_driver_register(&ir_platform_driver);
}

static void __exit ir_drv_exit(void)
{
    // 注销Platform驱动
    platform_driver_unregister(&ir_platform_driver);
}

module_init(ir_drv_init);
module_exit(ir_drv_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("IMX6U IR Obstacle Driver (Platform + DTS) Linux 4.15");
MODULE_AUTHOR("Developer");
MODULE_VERSION("V1.0");