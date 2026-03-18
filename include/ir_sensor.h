#ifndef __IR_SENSOR_H
#define __IR_SENSOR_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>

typedef struct ir_sensor ir_sensor_t;

// 打开红外传感器设备
ir_sensor_t* ir_sensor_open(void);

// 关闭传感器
void ir_sensor_close(ir_sensor_t *dev);

// 获取当前遮挡状态 (返回: 0=无遮挡, 1=有遮挡, -1=错误)
int ir_sensor_get_state(ir_sensor_t *dev);

// 手动触发一次检测 (返回当前状态)
int ir_sensor_check(ir_sensor_t *dev);

// 设置反向逻辑 (1=反向, 0=默认)
bool ir_sensor_set_reverse(ir_sensor_t *dev, int reverse);

// 设置检测间隔(ms)
bool ir_sensor_set_interval(ir_sensor_t *dev, int interval_ms);

// 设置防抖时间(ms)
bool ir_sensor_set_debounce(ir_sensor_t *dev, int debounce_ms);

// 等待遮挡发生 (timeout_ms=0表示无限等待)
bool ir_sensor_wait_obstacle(ir_sensor_t *dev, int timeout_ms);

#endif