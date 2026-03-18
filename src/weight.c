#include "weight.h"

#define WEIGHT_DEVICE_PATH "/dev/weight_sensor"
#define MAX_CMD_SIZE 64

struct weight_device {
    int fd;
    int threshold;      // 缺粮阈值（克）
    char error[256];
};

weight_device_t* weight_open(void)
{
    weight_device_t *dev = calloc(1, sizeof(weight_device_t));
    if (!dev) return NULL;
    
    dev->fd = open(WEIGHT_DEVICE_PATH, O_RDWR);
    if (dev->fd < 0) {
        snprintf(dev->error, sizeof(dev->error),
                 "open %s failed: %s", WEIGHT_DEVICE_PATH, strerror(errno));
        free(dev);
        return NULL;
    }
    
    // 读取一次获取初始阈值（驱动返回格式只有重量数字）
    // 阈值需要通过命令读取或使用默认值
    dev->threshold = 50;  // 默认50克
    
    printf("重量传感器打开成功: %s\n", WEIGHT_DEVICE_PATH);
    return dev;
}

void weight_close(weight_device_t *dev)
{
    if (dev) {
        if (dev->fd >= 0) close(dev->fd);
        free(dev);
    }
}

int weight_read_gram(weight_device_t *dev)
{
    if (!dev || dev->fd < 0) return -1;
    
    char buf[32] = {0};
    
    // 移动到文件开头
    lseek(dev->fd, 0, SEEK_SET);
    
    // 读取重量（驱动只返回 "xxx\n"）
    ssize_t ret = read(dev->fd, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        snprintf(dev->error, sizeof(dev->error),
                 "read weight failed: %s", strerror(errno));
        return -1;
    }
    
    return atoi(buf);
}

bool weight_calibrate_zero(weight_device_t *dev)
{
    if (!dev || dev->fd < 0) return false;
    
    ssize_t ret = write(dev->fd, "ZERO", 4);
    if (ret != 4) {
        snprintf(dev->error, sizeof(dev->error),
                 "calibrate zero failed: %s", strerror(errno));
        return false;
    }
    
    printf("重量传感器零点校准完成\n");
    return true;
}

bool weight_calibrate_with_weight(weight_device_t *dev, int known_weight)
{
    if (!dev || dev->fd < 0 || known_weight <= 0) return false;
    
    char cmd[MAX_CMD_SIZE];
    snprintf(cmd, sizeof(cmd), "CALI:%d", known_weight);
    
    ssize_t ret = write(dev->fd, cmd, strlen(cmd));
    if (ret != (ssize_t)strlen(cmd)) {
        snprintf(dev->error, sizeof(dev->error),
                 "calibrate with weight failed: %s", strerror(errno));
        return false;
    }
    
    printf("重量传感器校准完成，使用 %d克 砝码\n", known_weight);
    return true;
}

bool weight_set_threshold(weight_device_t *dev, int threshold)
{
    if (!dev || dev->fd < 0 || threshold <= 0) return false;
    
    char cmd[MAX_CMD_SIZE];
    snprintf(cmd, sizeof(cmd), "THRESH:%d", threshold);
    
    ssize_t ret = write(dev->fd, cmd, strlen(cmd));
    if (ret != (ssize_t)strlen(cmd)) {
        snprintf(dev->error, sizeof(dev->error),
                 "set threshold failed: %s", strerror(errno));
        return false;
    }
    
    dev->threshold = threshold;
    printf("缺粮阈值已设置为: %d克\n", threshold);
    return true;
}

bool weight_is_low(weight_device_t *dev)
{
    if (!dev) return false;
    
    int weight = weight_read_gram(dev);
    if (weight < 0) return false;
    
    return weight < dev->threshold;
}

const char* weight_get_error(weight_device_t *dev)
{
    return dev ? dev->error : "NULL";
}