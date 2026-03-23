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
#include <linux/debugfs.h>
#include <linux/atomic.h>

#define IR_DEV_NAME            "ir_obstacle"
#define IR_DEV_MINOR           2
#define IR_DEV_COUNT           1
#define DEFAULT_CHECK_INTERVAL 50
#define DEFAULT_DEBOUNCE_TIME  20

typedef struct {
    int                gpio_ir_out;
    int                ir_reverse;
    int                check_interval;
    int                debounce_time;
    int                obstacle_state;
    unsigned long      last_check_time;
    struct mutex       mutex;
    dev_t              devid;
    struct cdev        cdev;
    struct class       *class;
    struct device      *device;
    struct device_node *nd;
    struct platform_device *pdev;
    /* Debugfs: 运行时诊断接口 */
    struct dentry      *debugfs_dir;
    atomic_t           detect_count;   /* 累计检测次数 */
    atomic_t           obstacle_count; /* 累计遮挡次数 */
    atomic_t           debounce_hit;   /* 防抖命中次数（信号抖动被过滤） */
} ir_obstacle_dev_t;

static ir_obstacle_dev_t *ir_dev;

int ir_obstacle_check(void)
{
    static int last_state = -1;
    int val;
    unsigned long current_time;

    if (!ir_dev) {
        pr_err("[ir] 红外驱动未初始化\n");
        return -1;
    }

    mutex_lock(&ir_dev->mutex);
    current_time = jiffies;

    if (jiffies_to_msecs(current_time - ir_dev->last_check_time) < ir_dev->check_interval) {
        mutex_unlock(&ir_dev->mutex);
        return ir_dev->obstacle_state;
    }

    val = gpio_get_value(ir_dev->gpio_ir_out);

    /* 防抖：再读一次，两次不一致则视为抖动，计入防抖命中统计 */
    msleep(ir_dev->debounce_time);
    if (val != gpio_get_value(ir_dev->gpio_ir_out)) {
        atomic_inc(&ir_dev->debounce_hit);
        mutex_unlock(&ir_dev->mutex);
        return ir_dev->obstacle_state;
    }

    if (ir_dev->ir_reverse)
        ir_dev->obstacle_state = (val == 0) ? 1 : 0;
    else
        ir_dev->obstacle_state = (val == 1) ? 1 : 0;

    ir_dev->last_check_time = current_time;

    /* 更新检测计数和遮挡计数 */
    atomic_inc(&ir_dev->detect_count);
    if (ir_dev->obstacle_state)
        atomic_inc(&ir_dev->obstacle_count);

    if (ir_dev->obstacle_state != last_state) {
        if (ir_dev->obstacle_state)
            pr_warn("[ir] 检测到遮挡！\n");
        else
            pr_info("[ir] 遮挡解除\n");
        last_state = ir_dev->obstacle_state;
    }

    mutex_unlock(&ir_dev->mutex);
    return ir_dev->obstacle_state;
}
EXPORT_SYMBOL_GPL(ir_obstacle_check);

/************************ Debugfs 接口 ************************/
/*
 * 查看方式：
 *   cat /sys/kernel/debug/tcrt5000_ir/detect_count    # 累计检测次数
 *   cat /sys/kernel/debug/tcrt5000_ir/obstacle_count  # 累计遮挡次数
 *   cat /sys/kernel/debug/tcrt5000_ir/debounce_hit    # 防抖命中次数
 *   cat /sys/kernel/debug/tcrt5000_ir/obstacle_state  # 当前遮挡状态(0/1)
 */
static void ir_debugfs_init(ir_obstacle_dev_t *dev)
{
    dev->debugfs_dir = debugfs_create_dir("tcrt5000_ir", NULL);
    if (IS_ERR_OR_NULL(dev->debugfs_dir)) {
        pr_warn("[ir] debugfs init failed\n");
        dev->debugfs_dir = NULL;
        return;
    }
    debugfs_create_u32("obstacle_state",  0444, dev->debugfs_dir, (u32 *)&dev->obstacle_state);
    debugfs_create_u32("check_interval",  0444, dev->debugfs_dir, (u32 *)&dev->check_interval);
    debugfs_create_u32("debounce_time",   0444, dev->debugfs_dir, (u32 *)&dev->debounce_time);
    debugfs_create_u32("ir_reverse",      0444, dev->debugfs_dir, (u32 *)&dev->ir_reverse);
    debugfs_create_atomic_t("detect_count",   0444, dev->debugfs_dir, &dev->detect_count);
    debugfs_create_atomic_t("obstacle_count", 0444, dev->debugfs_dir, &dev->obstacle_count);
    debugfs_create_atomic_t("debounce_hit",   0444, dev->debugfs_dir, &dev->debounce_hit);
    pr_info("[ir] debugfs ready: /sys/kernel/debug/tcrt5000_ir/\n");
}

static void ir_debugfs_remove(ir_obstacle_dev_t *dev)
{
    if (dev->debugfs_dir)
        debugfs_remove_recursive(dev->debugfs_dir);
}

static int ir_open(struct inode *inode, struct file *filp)
{
    filp->private_data = container_of(inode->i_cdev, ir_obstacle_dev_t, cdev);
    pr_info("[ir] device open\n");
    return 0;
}

static ssize_t ir_read(struct file *filp, char __user *buf, size_t count, loff_t *ppos)
{
    ir_obstacle_dev_t *dev = filp->private_data;
    char state_buf[8] = {0};
    int len = snprintf(state_buf, sizeof(state_buf), "%d\n", dev->obstacle_state);
    if (copy_to_user(buf, state_buf, len)) return -EFAULT;
    return len;
}

static ssize_t ir_write(struct file *filp, const char __user *buf, size_t count, loff_t *ppos)
{
    ir_obstacle_dev_t *dev = filp->private_data;
    char cmd_buf[64] = {0};
    int val;
    if (copy_from_user(cmd_buf, buf, min(count, sizeof(cmd_buf)-1))) return -EFAULT;
    if (sscanf(cmd_buf, "REVERSE:%d", &val) == 1)
        dev->ir_reverse = (val == 1) ? 1 : 0;
    else if (sscanf(cmd_buf, "INTERVAL:%d", &val) == 1)
        dev->check_interval = val > 10 ? val : DEFAULT_CHECK_INTERVAL;
    else if (sscanf(cmd_buf, "DEBOUNCE:%d", &val) == 1)
        dev->debounce_time = val > 5 ? val : DEFAULT_DEBOUNCE_TIME;
    else if (strcmp(cmd_buf, "CHECK") == 0)
        ir_obstacle_check();
    else
        return -EINVAL;
    return count;
}

static int ir_release(struct inode *inode, struct file *filp)
{
    pr_info("[ir] device release\n");
    return 0;
}

static const struct file_operations ir_fops = {
    .owner   = THIS_MODULE,
    .open    = ir_open,
    .read    = ir_read,
    .write   = ir_write,
    .release = ir_release,
};

static int ir_parse_dts(ir_obstacle_dev_t *dev)
{
    int ret;
    u32 val;
    dev->nd = dev->pdev->dev.of_node;
    if (!dev->nd) { pr_err("[ir] no dts node\n"); return -EINVAL; }

    dev->gpio_ir_out = of_get_named_gpio(dev->nd, "ir-out-gpios", 0);
    if (!gpio_is_valid(dev->gpio_ir_out)) return -EINVAL;
    ret = gpio_request(dev->gpio_ir_out, "ir-out");
    if (ret < 0) return ret;
    gpio_direction_input(dev->gpio_ir_out);

    ret = of_property_read_u32(dev->nd, "ir-reverse", &val);
    dev->ir_reverse = ret < 0 ? 0 : val;
    ret = of_property_read_u32(dev->nd, "ir-check-interval", &val);
    dev->check_interval = (ret < 0 || val < 10) ? DEFAULT_CHECK_INTERVAL : val;
    ret = of_property_read_u32(dev->nd, "ir-debounce-time", &val);
    dev->debounce_time = (ret < 0 || val < 5) ? DEFAULT_DEBOUNCE_TIME : val;

    dev->obstacle_state   = 0;
    dev->last_check_time  = jiffies;
    pr_info("[ir] params: reverse=%d, interval=%dms, debounce=%dms\n",
            dev->ir_reverse, dev->check_interval, dev->debounce_time);
    return 0;
}

static int ir_platform_probe(struct platform_device *pdev)
{
    int ret;
    ir_obstacle_dev_t *dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev) return -ENOMEM;

    dev->pdev = pdev;
    mutex_init(&dev->mutex);
    atomic_set(&dev->detect_count, 0);
    atomic_set(&dev->obstacle_count, 0);
    atomic_set(&dev->debounce_hit, 0);
    ir_dev = dev;

    ret = ir_parse_dts(dev);
    if (ret < 0) { kfree(dev); return ret; }

    ret = alloc_chrdev_region(&dev->devid, IR_DEV_MINOR, IR_DEV_COUNT, IR_DEV_NAME);
    if (ret < 0) goto err_alloc;
    cdev_init(&dev->cdev, &ir_fops);
    dev->cdev.owner = THIS_MODULE;
    ret = cdev_add(&dev->cdev, dev->devid, IR_DEV_COUNT);
    if (ret < 0) goto err_cdev;
    dev->class = class_create(THIS_MODULE, "ir_obstacle_class");
    if (IS_ERR(dev->class)) { ret = PTR_ERR(dev->class); goto err_class; }
    dev->device = device_create(dev->class, &pdev->dev, dev->devid, NULL, IR_DEV_NAME);
    if (IS_ERR(dev->device)) { ret = PTR_ERR(dev->device); goto err_device; }

    platform_set_drvdata(pdev, dev);
    ir_obstacle_check();

    /* 注册 Debugfs 接口，导出检测次数、遮挡次数、防抖命中率供运行时诊断 */
    ir_debugfs_init(dev);

    pr_info("[ir] platform probe success | dev: /dev/%s\n", IR_DEV_NAME);
    return 0;

err_device: class_destroy(dev->class);
err_class:  cdev_del(&dev->cdev);
err_cdev:   unregister_chrdev_region(dev->devid, IR_DEV_COUNT);
err_alloc:  gpio_free(dev->gpio_ir_out); kfree(dev); ir_dev = NULL;
    return ret;
}

static int ir_platform_remove(struct platform_device *pdev)
{
    ir_obstacle_dev_t *dev = platform_get_drvdata(pdev);
    if (dev) {
        ir_debugfs_remove(dev);
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

static const struct of_device_id ir_of_match[] = {
    { .compatible = "alientek,ir-obstacle-platform" },
    { /* Sentinel */ }
};
MODULE_DEVICE_TABLE(of, ir_of_match);

static struct platform_driver ir_platform_driver = {
    .driver = {
        .name = "ir-obstacle-platform-driver",
        .of_match_table = ir_of_match,
        .owner = THIS_MODULE,
    },
    .probe  = ir_platform_probe,
    .remove = ir_platform_remove,
};

static int __init ir_drv_init(void)
{
    return platform_driver_register(&ir_platform_driver);
}

static void __exit ir_drv_exit(void)
{
    platform_driver_unregister(&ir_platform_driver);
}

module_init(ir_drv_init);
module_exit(ir_drv_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("IMX6U TCRT5000 IR Obstacle Driver with Debugfs");
MODULE_AUTHOR("Developer");
MODULE_VERSION("V2.0");
