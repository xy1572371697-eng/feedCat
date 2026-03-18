#ifndef __WEIGHT_H
#define __WEIGHT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>

typedef struct weight_device weight_device_t;

// 打开重量传感器设备
weight_device_t* weight_open(void);

// 关闭传感器
void weight_close(weight_device_t *dev);

// 读取当前重量（克）
int weight_read_gram(weight_device_t *dev);

// 零点校准
bool weight_calibrate_zero(weight_device_t *dev);

// 设置校准系数（传入已知重量进行校准）
bool weight_calibrate_with_weight(weight_device_t *dev, int known_weight);

// 设置缺粮阈值
bool weight_set_threshold(weight_device_t *dev, int threshold);

// 检查是否缺粮
bool weight_is_low(weight_device_t *dev);

// 获取最后一次错误信息
const char* weight_get_error(weight_device_t *dev);

#endif