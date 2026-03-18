#include "motor.h"

#define MOTOR_DEVICE_PATH "/dev/motor_platform"
#define MAX_CMD_SIZE 64

struct motor_device {
    int fd;
    int feed_count;
    int max_feed;
    int pwm_duty;
    int block_detected;
    char error[256];
};

static void parse_status(motor_device_t *dev, const char *buf)
{
    // 解析格式: "FeedCount:%d|MaxFeed:%d|PWMDuty:%d|BlockDetected:%d\n"
    const char *p;
    
    p = strstr(buf, "FeedCount:");
    if (p) dev->feed_count = atoi(p + 10);
    
    p = strstr(buf, "MaxFeed:");
    if (p) dev->max_feed = atoi(p + 8);
    
    p = strstr(buf, "PWMDuty:");
    if (p) dev->pwm_duty = atoi(p + 8);
    
    p = strstr(buf, "BlockDetected:");
    if (p) dev->block_detected = atoi(p + 15);
}

motor_device_t* motor_open(void)
{
    motor_device_t *dev = calloc(1, sizeof(motor_device_t));
    if (!dev) return NULL;
    
    dev->fd = open(MOTOR_DEVICE_PATH, O_RDWR);
    if (dev->fd < 0) {
        snprintf(dev->error, sizeof(dev->error),
                 "open %s failed: %s", MOTOR_DEVICE_PATH, strerror(errno));
        free(dev);
        return NULL;
    }
    
    // 读取初始状态
    char buf[256] = {0};
    lseek(dev->fd, 0, SEEK_SET);
    if (read(dev->fd, buf, sizeof(buf) - 1) > 0) {
        parse_status(dev, buf);
    }
    
    printf("电机驱动打开成功: %s\n", MOTOR_DEVICE_PATH);
    return dev;
}

void motor_close(motor_device_t *dev)
{
    if (dev) {
        if (dev->fd >= 0) {
            motor_emergency_stop(dev);
            close(dev->fd);
        }
        free(dev);
    }
}

bool motor_feed_gram(motor_device_t *dev, int gram)
{
    if (!dev || dev->fd < 0 || gram <= 0) {
        return false;
    }
    
    char cmd[MAX_CMD_SIZE];
    snprintf(cmd, sizeof(cmd), "FEED:%d", gram);
    
    printf("电机喂食: %d克\n", gram);
    ssize_t ret = write(dev->fd, cmd, strlen(cmd));
    
    if (ret == (ssize_t)strlen(cmd)) {
        // 读取更新后的状态
        char buf[256] = {0};
        lseek(dev->fd, 0, SEEK_SET);
        if (read(dev->fd, buf, sizeof(buf) - 1) > 0) {
            parse_status(dev, buf);
        }
        return true;
    }
    
    snprintf(dev->error, sizeof(dev->error),
             "feed failed: %s", strerror(errno));
    return false;
}

bool motor_emergency_stop(motor_device_t *dev)
{
    if (!dev || dev->fd < 0) return false;
    
    ssize_t ret = write(dev->fd, "STOP", 4);
    if (ret != 4) {
        snprintf(dev->error, sizeof(dev->error),
                 "stop failed: %s", strerror(errno));
        return false;
    }
    
    printf("电机紧急停止\n");
    return true;
}

int motor_get_feed_count(motor_device_t *dev)
{
    if (!dev) return -1;
    
    // 读取最新状态
    char buf[256] = {0};
    lseek(dev->fd, 0, SEEK_SET);
    if (read(dev->fd, buf, sizeof(buf) - 1) > 0) {
        parse_status(dev, buf);
    }
    
    return dev->feed_count;
}

int motor_get_max_feed(motor_device_t *dev)
{
    if (!dev) return -1;
    return dev->max_feed;
}

int motor_get_pwm_duty(motor_device_t *dev)
{
    if (!dev) return -1;
    return dev->pwm_duty;
}

bool motor_is_blocked(motor_device_t *dev)
{
    if (!dev) return false;
    
    // 读取最新状态
    char buf[256] = {0};
    lseek(dev->fd, 0, SEEK_SET);
    if (read(dev->fd, buf, sizeof(buf) - 1) > 0) {
        parse_status(dev, buf);
    }
    
    return dev->block_detected != 0;
}

const char* motor_get_error(motor_device_t *dev)
{
    return dev ? dev->error : "NULL";
}