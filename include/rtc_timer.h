#ifndef RTC_TIMER_H
#define RTC_TIMER_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// 定时任务结构
typedef struct {
    int id;              // 任务ID
    int enabled;         // 是否启用
    int hour;            // 小时 0-23
    int minute;          // 分钟 0-59
    int second;          // 秒 0-59
    int feed_amount;     // 喂食量（克）
    int repeat;          // 重复类型：0=单次，1=每天，2=每周
} timer_task_t;

// 定时器回调函数类型
typedef void (*timer_callback_t)(timer_task_t *task, void *userdata);

// ============ API接口 ============
// 初始化RTC定时器系统
bool rtc_timer_init(void);

// 添加定时任务
int rtc_timer_add_task(timer_task_t *task);

// 删除定时任务
bool rtc_timer_remove_task(int task_id);

// 修改定时任务
bool rtc_timer_modify_task(timer_task_t *task);

// 获取所有任务
int rtc_timer_get_tasks(timer_task_t *tasks, int max_count);

// 设置回调（当定时任务触发时）
void rtc_timer_set_callback(timer_callback_t cb, void *userdata);

// 获取当前系统时间
time_t rtc_get_current_time(void);

// 设置系统时间（需要root权限）
bool rtc_set_system_time(time_t t);

// 同步RTC硬件时间到系统
bool rtc_sync_to_system(void);

// 同步系统时间到RTC硬件
bool rtc_sync_to_hardware(void);

#ifdef __cplusplus
}
#endif

#endif