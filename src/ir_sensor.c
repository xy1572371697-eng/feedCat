#include "ir_sensor.h"

#define IR_DEVICE_PATH "/dev/ir_obstacle"
#define MAX_CMD_SIZE 64

struct ir_sensor {
    int fd;
    int reverse;
    int interval;
    int debounce;
};

// 发送命令到驱动
static int send_cmd(ir_sensor_t *dev, const char *cmd)
{
    if (!dev || dev->fd < 0) return -1;
    
    ssize_t ret = write(dev->fd, cmd, strlen(cmd));
    return (ret == (ssize_t)strlen(cmd)) ? 0 : -1;
}

// 从驱动读取状态
static int read_state(ir_sensor_t *dev)
{
    if (!dev || dev->fd < 0) return -1;
    
    char buf[4] = {0};
    
    // 移动到文件开头
    lseek(dev->fd, 0, SEEK_SET);
    
    // 读取状态（驱动只返回 "0\n" 或 "1\n"）
    if (read(dev->fd, buf, sizeof(buf) - 1) <= 0) {
        return -1;
    }
    
    return atoi(buf);
}

ir_sensor_t* ir_sensor_open(void)
{
    ir_sensor_t *dev = calloc(1, sizeof(ir_sensor_t));
    if (!dev) return NULL;
    
    dev->fd = open(IR_DEVICE_PATH, O_RDWR);
    if (dev->fd < 0) {
        free(dev);
        return NULL;
    }
    
    // 初始化默认值（与驱动默认值保持一致）
    dev->reverse = 0;
    dev->interval = 50;
    dev->debounce = 20;
    
    return dev;
}

void ir_sensor_close(ir_sensor_t *dev)
{
    if (dev) {
        if (dev->fd >= 0) close(dev->fd);
        free(dev);
    }
}

int ir_sensor_get_state(ir_sensor_t *dev)
{
    if (!dev || dev->fd < 0) return -1;
    return read_state(dev);
}

int ir_sensor_check(ir_sensor_t *dev)
{
    if (!dev || dev->fd < 0) return -1;
    
    // 发送CHECK命令触发驱动检测
    if (send_cmd(dev, "CHECK") < 0) {
        return -1;
    }
    
    // 等待驱动更新状态（驱动内部有debounce_time延时）
    usleep(dev->debounce * 1000);
    
    return read_state(dev);
}

bool ir_sensor_set_reverse(ir_sensor_t *dev, int reverse)
{
    if (!dev) return false;
    
    char cmd[MAX_CMD_SIZE];
    snprintf(cmd, sizeof(cmd), "REVERSE:%d", reverse ? 1 : 0);
    
    if (send_cmd(dev, cmd) == 0) {
        dev->reverse = reverse;
        return true;
    }
    return false;
}

bool ir_sensor_set_interval(ir_sensor_t *dev, int interval_ms)
{
    if (!dev) return false;
    if (interval_ms < 10) interval_ms = 10;  // 驱动要求最小10ms
    
    char cmd[MAX_CMD_SIZE];
    snprintf(cmd, sizeof(cmd), "INTERVAL:%d", interval_ms);
    
    if (send_cmd(dev, cmd) == 0) {
        dev->interval = interval_ms;
        return true;
    }
    return false;
}

bool ir_sensor_set_debounce(ir_sensor_t *dev, int debounce_ms)
{
    if (!dev) return false;
    if (debounce_ms < 5) debounce_ms = 5;  // 驱动要求最小5ms
    
    char cmd[MAX_CMD_SIZE];
    snprintf(cmd, sizeof(cmd), "DEBOUNCE:%d", debounce_ms);
    
    if (send_cmd(dev, cmd) == 0) {
        dev->debounce = debounce_ms;
        return true;
    }
    return false;
}

bool ir_sensor_wait_obstacle(ir_sensor_t *dev, int timeout_ms)
{
    if (!dev) return false;
    
    int elapsed = 0;
    int step = 50;  // 每50ms检测一次
    
    while (timeout_ms == 0 || elapsed < timeout_ms) {
        int state = ir_sensor_check(dev);
        if (state == 1) return true;
        if (state < 0) return false;  // 错误
        
        usleep(step * 1000);
        elapsed += step;
    }
    return false;  // 超时
}