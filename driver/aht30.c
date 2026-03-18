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
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/init.h>

#define AHT30_NAME "aht30"
#define AHT30_NUMBER 1
#define AHT30_I2C_ADDR 0x38  // AHT30默认I2C地址

// CRC相关参数
enum crc_para {
    CRC_INIT = 0xFF,
    CRC_POLY = 0x31,
};

// AHT30设备结构体
struct aht30_dev {
    struct i2c_client *client;
    dev_t devid;
    int major;
    int minor;
    struct cdev cdev;
    struct class *class;
    struct device *device;
    struct mutex lock;  // 互斥锁，防止并发访问
    
    u8 status;
    u32 hum_raw;        // 湿度原始值
    u32 tem_raw;        // 温度原始值
    int hum;            // 湿度值(0.1%精度)
    int tem;            // 温度值(0.1℃精度)
    u8 crc_receive;     // 接收到的CRC值
    u8 crc_calc;        // 计算出的CRC值
};

static struct aht30_dev *aht30_devp;

// 向AHT30写入寄存器数据
static s32 aht30_write_regs(struct aht30_dev *dev, u8 *buf, u8 len)
{
    int ret;
    struct i2c_msg msg;
    
    if (!dev || !buf || len == 0)
        return -EINVAL;

    msg.addr = dev->client->addr;
    msg.buf = buf;
    msg.flags = 0;  // 写操作
    msg.len = len;

    ret = i2c_transfer(dev->client->adapter, &msg, 1);
    if (ret != 1) {
        pr_err("AHT30 I2C write failed, ret=%d\n", ret);
        return -EREMOTEIO;
    }
    
    return 0;
}

// 从AHT30读取寄存器数据
static int aht30_read_regs(struct aht30_dev *dev, void *val, int len)
{
    int ret;
    struct i2c_msg msg[2];
    
    if (!dev || !val || len <= 0)
        return -EINVAL;

    // 第一步：发送读取命令
    msg[0].addr = dev->client->addr;
    msg[0].buf = (u8[]){0x71};  // 读取状态命令
    msg[0].flags = 0;           // 写操作
    msg[0].len = 1;

    // 第二步：接收数据
    msg[1].addr = dev->client->addr;
    msg[1].buf = val;
    msg[1].flags = I2C_M_RD;    // 读操作
    msg[1].len = len;

    ret = i2c_transfer(dev->client->adapter, msg, 2);
    if (ret == 2) {  // 修复：== 而非 =
        ret = 0;
    } else {
        pr_err("AHT30 I2C read failed, ret=%d\n", ret);
        ret = -EREMOTEIO;
    }
    
    return ret;
}

// AHT30初始化
static int aht30_init_sensor(struct aht30_dev *dev)
{
    u8 init_cmd[] = {0xBE, 0x08, 0x00};  // AHT30初始化命令
    int ret;
    
    mutex_lock(&dev->lock);
    
    // 发送初始化命令
    ret = aht30_write_regs(dev, init_cmd, sizeof(init_cmd));
    if (ret < 0) {
        mutex_unlock(&dev->lock);
        return ret;
    }
    
    mdelay(40);  // 初始化等待时间
    mutex_unlock(&dev->lock);
    
    return 0;
}

// 触发AHT30测量
static int aht30_trigger_measure(struct aht30_dev *dev)
{
    u8 measure_cmd[] = {0xAC, 0x33, 0x00};  // 修复：正确的测量命令
    int ret;
    
    mutex_lock(&dev->lock);
    
    // 发送测量命令
    ret = aht30_write_regs(dev, measure_cmd, sizeof(measure_cmd));
    if (ret < 0) {
        mutex_unlock(&dev->lock);
        return ret;
    }
    
    mdelay(80);  // 测量等待时间
    mutex_unlock(&dev->lock);
    
    return 0;
}

// 计算CRC8校验值
static u8 aht30_calc_crc8(u8 *data, u8 len)
{
    u8 crc = CRC_INIT;
    u8 i, j;
    
    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (j = 0; j < 8; j++) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ CRC_POLY;
            } else {
                crc <<= 1;
            }
            crc &= 0xFF;  // 确保只保留8位
        }
    }
    
    return crc;
}

// 接收AHT30测量数据
static int aht30_receive_data(struct aht30_dev *dev)
{
    u8 buf[7];
    int ret;
    
    if (!dev)
        return -EINVAL;

    mutex_lock(&dev->lock);
    
    // 读取7字节数据
    ret = aht30_read_regs(dev, buf, 7);
    if (ret < 0) {
        mutex_unlock(&dev->lock);
        return ret;
    }

    // 解析状态字节
    dev->status = buf[0];
    
    // 解析湿度原始值 (20位)
    dev->hum_raw = ((u32)buf[1] << 12) | ((u32)buf[2] << 4) | ((buf[3] & 0xF0) >> 4);
    
    // 解析温度原始值 (20位)
    dev->tem_raw = ((u32)(buf[3] & 0x0F) << 16) | ((u32)buf[4] << 8) | buf[5];
    
    // 获取接收到的CRC值
    dev->crc_receive = buf[6];
    
    // 计算CRC值
    dev->crc_calc = aht30_calc_crc8(buf, 6);
    
    mutex_unlock(&dev->lock);
    
    // 校验CRC
    if (dev->crc_calc != dev->crc_receive) {
        pr_err("AHT30 CRC check failed: calc=0x%02x, receive=0x%02x\n", 
               dev->crc_calc, dev->crc_receive);
        return -EIO;
    }
    
    return 0;
}

// 计算温湿度值 (保留1位小数)
static void aht30_calc_data(struct aht30_dev *dev)
{
    if (!dev)
        return;

    mutex_lock(&dev->lock);
    
    // 湿度计算: 湿度(0.1%) = (原始值 / 2^20) * 1000
    dev->hum = (dev->hum_raw * 1000) / 0x100000;
    
    // 温度计算: 温度(0.1℃) = (原始值 / 2^20) * 2000 - 500
    dev->tem = (dev->tem_raw * 2000) / 0x100000 - 500;
    
    mutex_unlock(&dev->lock);
}

// 设备文件打开函数
static int aht30_open(struct inode *inode, struct file *filp)
{
    filp->private_data = aht30_devp;
    return 0;
}

// 设备文件读取函数
static ssize_t aht30_read(struct file *filp, char __user *buf, 
                          size_t cnt, loff_t *off)
{
    int ret;
    int data[2];  // [0]温度(0.1℃), [1]湿度(0.1%)
    struct aht30_dev *dev = filp->private_data;
    
    if (!dev || !buf)
        return -EINVAL;

    // 触发测量
    ret = aht30_trigger_measure(dev);
    if (ret < 0)
        return ret;

    // 读取测量数据
    ret = aht30_receive_data(dev);
    if (ret < 0)
        return ret;

    // 计算温湿度值
    aht30_calc_data(dev);

    // 准备返回数据
    data[0] = dev->tem;
    data[1] = dev->hum;

    // 拷贝数据到用户空间
    ret = copy_to_user(buf, data, sizeof(data));
    if (ret != 0) {
        pr_err("AHT30 copy to user failed, ret=%d\n", ret);
        return -EFAULT;
    }

    // 修复：返回实际读取的字节数
    return sizeof(data);
}

// 设备文件释放函数
static int aht30_release(struct inode *inode, struct file *filp)
{
    return 0;
}

// 文件操作集合
static const struct file_operations aht30_fops = {
    .owner = THIS_MODULE,
    .open = aht30_open,
    .read = aht30_read,
    .release = aht30_release,
};

// I2C设备匹配表
static const struct of_device_id aht30_of_match[] = {
    {.compatible = "alientek,aht30"},
    {},
};
MODULE_DEVICE_TABLE(of, aht30_of_match);

// I2C设备ID表
static const struct i2c_device_id aht30_id_table[] = {
    {AHT30_NAME, 0},
    {},
};
MODULE_DEVICE_TABLE(i2c, aht30_id_table);

// I2C probe函数 (设备匹配成功时调用)
static int aht30_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    int ret;
    struct aht30_dev *dev;

    // 分配设备结构体内存
    dev = devm_kzalloc(&client->dev, sizeof(struct aht30_dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;

    // 初始化互斥锁
    mutex_init(&dev->lock);
    
    // 保存I2C客户端指针
    dev->client = client;
    i2c_set_clientdata(client, dev);
    aht30_devp = dev;

    // 1. 注册字符设备号
    if (dev->major) {
        dev->devid = MKDEV(dev->major, 0);
        ret = register_chrdev_region(dev->devid, AHT30_NUMBER, AHT30_NAME);
    } else {
        ret = alloc_chrdev_region(&dev->devid, 0, AHT30_NUMBER, AHT30_NAME);
        dev->major = MAJOR(dev->devid);
        dev->minor = MINOR(dev->devid);
    }
    
    if (ret < 0) {
        pr_err("AHT30 register chrdev failed, ret=%d\n", ret);
        goto fail_devid;
    }

    // 2. 初始化cdev
    dev->cdev.owner = THIS_MODULE;
    cdev_init(&dev->cdev, &aht30_fops);
    
    // 3. 添加cdev
    ret = cdev_add(&dev->cdev, dev->devid, AHT30_NUMBER);
    if (ret < 0) {
        pr_err("AHT30 add cdev failed, ret=%d\n", ret);
        goto fail_cdev;
    }

    // 4. 创建类
    dev->class = class_create(THIS_MODULE, AHT30_NAME);
    if (IS_ERR(dev->class)) {
        ret = PTR_ERR(dev->class);
        pr_err("AHT30 create class failed, ret=%d\n", ret);
        goto fail_class;
    }

    // 5. 创建设备节点
    dev->device = device_create(dev->class, &client->dev, dev->devid, NULL, AHT30_NAME);
    if (IS_ERR(dev->device)) {
        ret = PTR_ERR(dev->device);
        pr_err("AHT30 create device failed, ret=%d\n", ret);
        goto fail_device;
    }

    // 6. 初始化AHT30传感器
    ret = aht30_init_sensor(dev);
    if (ret < 0) {
        pr_err("AHT30 sensor init failed, ret=%d\n", ret);
        goto fail_sensor;
    }

    pr_info("AHT30 probe success, major=%d\n", dev->major);
    return 0;

fail_sensor:
    device_destroy(dev->class, dev->devid);
fail_device:
    class_destroy(dev->class);
fail_class:
    cdev_del(&dev->cdev);
fail_cdev:
    unregister_chrdev_region(dev->devid, AHT30_NUMBER);
fail_devid:
    return ret;
}

// I2C remove函数 (设备移除时调用)
static int aht30_remove(struct i2c_client *client)
{
    struct aht30_dev *dev = i2c_get_clientdata(client);

    // 销毁设备节点
    device_destroy(dev->class, dev->devid);
    
    // 销毁类
    class_destroy(dev->class);
    
    // 删除字符设备
    cdev_del(&dev->cdev);
    
    // 注销设备号
    unregister_chrdev_region(dev->devid, AHT30_NUMBER);
    
    pr_info("AHT30 remove success\n");
    return 0;
}

// I2C驱动结构体
static struct i2c_driver aht30_driver = {
    .probe = aht30_probe,
    .remove = aht30_remove,  // 修复：指向正确的remove函数
    .driver = {
        .owner = THIS_MODULE,
        .name = AHT30_NAME,
        .of_match_table = aht30_of_match,
    },
    .id_table = aht30_id_table,
};

// 模块初始化
static int __init aht30_init(void)
{
    int ret;
    
    ret = i2c_add_driver(&aht30_driver);
    if (ret < 0) {
        pr_err("AHT30 add driver failed, ret=%d\n", ret);
        return ret;
    }
    
    pr_info("AHT30 driver init success\n");
    return 0;
}

// 模块退出
static void __exit aht30_exit(void)
{
    i2c_del_driver(&aht30_driver);
    pr_info("AHT30 driver exit success\n");
}

module_init(aht30_init);
module_exit(aht30_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("xiangyang");
MODULE_DESCRIPTION("AHT30 Temperature and Humidity Sensor I2C Driver");
MODULE_ALIAS("i2c:aht30");