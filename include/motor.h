#ifndef __MOTOR_H
#define __MOTOR_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>

typedef struct motor_device motor_device_t;

// 打开电机设备
motor_device_t* motor_open(void);

// 关闭电机设备
void motor_close(motor_device_t *dev);

// 喂食指定克数（核心功能）
bool motor_feed_gram(motor_device_t *dev, int gram);

// 紧急停止电机
bool motor_emergency_stop(motor_device_t *dev);

// 获取喂食次数
int motor_get_feed_count(motor_device_t *dev);

// 获取最大喂食量
int motor_get_max_feed(motor_device_t *dev);

// 获取PWM占空比
int motor_get_pwm_duty(motor_device_t *dev);

// 检查是否堵转
bool motor_is_blocked(motor_device_t *dev);

// 获取最后一次错误信息
const char* motor_get_error(motor_device_t *dev);

#endif