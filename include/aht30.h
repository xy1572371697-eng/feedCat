#ifndef AHT30_H
#define AHT30_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aht30_device aht30_device_t;

/* 打开 /dev/aht30 */
aht30_device_t* aht30_open(void);

/* 关闭设备 */
void aht30_close(aht30_device_t *dev);

/**
 * 读取温湿度
 * @param temp_c  输出温度（摄氏度，保留1位小数，如 25.3 → 253 / 10.0f）
 * @param hum_pct 输出湿度（%RH，保留1位小数，同上）
 * @return true=成功, false=失败
 */
bool aht30_read(aht30_device_t *dev, float *temp_c, float *hum_pct);

/* 获取最后一次错误信息 */
const char* aht30_get_error(aht30_device_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* AHT30_H */
