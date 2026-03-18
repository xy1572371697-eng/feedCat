#include "aht30.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define AHT30_DEVICE_PATH "/dev/aht30"

struct aht30_device {
    int fd;
    char error[256];
};

aht30_device_t* aht30_open(void)
{
    aht30_device_t *dev = calloc(1, sizeof(aht30_device_t));
    if (!dev) return NULL;

    dev->fd = open(AHT30_DEVICE_PATH, O_RDONLY);
    if (dev->fd < 0) {
        snprintf(dev->error, sizeof(dev->error),
                 "open %s failed: %s", AHT30_DEVICE_PATH, strerror(errno));
        free(dev);
        return NULL;
    }

    return dev;
}

void aht30_close(aht30_device_t *dev)
{
    if (dev) {
        if (dev->fd >= 0) close(dev->fd);
        free(dev);
    }
}

bool aht30_read(aht30_device_t *dev, float *temp_c, float *hum_pct)
{
    if (!dev || dev->fd < 0 || !temp_c || !hum_pct) return false;

    /* 驱动返回 int[2]: [0]=温度(0.1℃精度), [1]=湿度(0.1%精度) */
    int data[2];
    lseek(dev->fd, 0, SEEK_SET);
    ssize_t ret = read(dev->fd, data, sizeof(data));
    if (ret != (ssize_t)sizeof(data)) {
        snprintf(dev->error, sizeof(dev->error),
                 "read aht30 failed: %s", strerror(errno));
        return false;
    }

    *temp_c  = data[0] / 10.0f;
    *hum_pct = data[1] / 10.0f;
    return true;
}

const char* aht30_get_error(aht30_device_t *dev)
{
    return dev ? dev->error : "NULL device";
}
