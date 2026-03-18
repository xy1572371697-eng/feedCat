#include "rtc_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <linux/rtc.h>
#include <errno.h>

#define RTC_DEVICE "/dev/rtc0"
#define MAX_TIMER_TASKS 20

typedef struct {
    timer_task_t task;
    int active;
    time_t next_trigger;
} timer_task_internal_t;

static struct {
    timer_task_internal_t tasks[MAX_TIMER_TASKS];
    int task_count;
    pthread_t monitor_thread;
    int thread_running;
    timer_callback_t user_callback;
    void *user_data;
    pthread_mutex_t lock;
} g_rtc_timer;

// 计算下一次触发时间
static time_t calculate_next_trigger(timer_task_t *task, time_t now) {
    struct tm tm_now, tm_task;
    time_t next;
    
    localtime_r(&now, &tm_now);
    tm_task = tm_now;
    tm_task.tm_hour = task->hour;
    tm_task.tm_min = task->minute;
    tm_task.tm_sec = task->second;
    
    next = mktime(&tm_task);
    
    if (next <= now) {
        if (task->repeat == 1) {  // 每天
            next += 24 * 3600;
        } else if (task->repeat == 2) {  // 每周
            next += 7 * 24 * 3600;
        } else {
            return -1;  // 单次任务且已过时
        }
    }
    
    return next;
}

// 定时器监控线程
static void* timer_monitor_thread(void *arg) {
    (void)arg;
    
    while (g_rtc_timer.thread_running) {
        time_t now = time(NULL);
        
        pthread_mutex_lock(&g_rtc_timer.lock);
        
        for (int i = 0; i < g_rtc_timer.task_count; i++) {
            if (!g_rtc_timer.tasks[i].active || 
                !g_rtc_timer.tasks[i].task.enabled) {
                continue;
            }
            
            // 检查是否到达触发时间
            if (g_rtc_timer.tasks[i].next_trigger <= now) {
                // 触发回调
                if (g_rtc_timer.user_callback) {
                    g_rtc_timer.user_callback(&g_rtc_timer.tasks[i].task, 
                                              g_rtc_timer.user_data);
                }
                
                // 计算下一次触发时间
                time_t next = calculate_next_trigger(
                    &g_rtc_timer.tasks[i].task, now);
                g_rtc_timer.tasks[i].next_trigger = next;
            }
        }
        
        pthread_mutex_unlock(&g_rtc_timer.lock);
        
        sleep(1);  // 每秒检查一次
    }
    
    return NULL;
}

bool rtc_timer_init(void) {
    memset(&g_rtc_timer, 0, sizeof(g_rtc_timer));
    pthread_mutex_init(&g_rtc_timer.lock, NULL);
    
    g_rtc_timer.thread_running = 1;
    if (pthread_create(&g_rtc_timer.monitor_thread, NULL, 
                       timer_monitor_thread, NULL) != 0) {
        printf("创建定时器线程失败\n");
        return false;
    }
    
    printf("RTC定时器系统初始化成功\n");
    return true;
}

int rtc_timer_add_task(timer_task_t *task) {
    pthread_mutex_lock(&g_rtc_timer.lock);
    
    if (g_rtc_timer.task_count >= MAX_TIMER_TASKS) {
        pthread_mutex_unlock(&g_rtc_timer.lock);
        return -1;
    }
    
    int id = g_rtc_timer.task_count++;
    g_rtc_timer.tasks[id].task = *task;
    g_rtc_timer.tasks[id].task.id = id;
    g_rtc_timer.tasks[id].active = 1;
    g_rtc_timer.tasks[id].next_trigger = 
        calculate_next_trigger(task, time(NULL));
    
    pthread_mutex_unlock(&g_rtc_timer.lock);
    
    printf("添加定时任务 ID=%d: %02d:%02d:%02d 喂食%d克 [%s]\n",
           id, task->hour, task->minute, task->second, 
           task->feed_amount, task->repeat ? "每天" : "单次");
    
    return id;
}

bool rtc_timer_remove_task(int task_id) {
    if (task_id < 0 || task_id >= g_rtc_timer.task_count) {
        return false;
    }
    
    pthread_mutex_lock(&g_rtc_timer.lock);
    g_rtc_timer.tasks[task_id].active = 0;
    pthread_mutex_unlock(&g_rtc_timer.lock);
    
    printf("删除定时任务 ID=%d\n", task_id);
    return true;
}

time_t rtc_get_current_time(void) {
    return time(NULL);
}

bool rtc_sync_to_system(void) {
    // RTC硬件时间 -> 系统时间
    int rtc_fd = open(RTC_DEVICE, O_RDONLY);
    if (rtc_fd < 0) {
        printf("打开RTC设备失败\n");
        return false;
    }
    
    struct rtc_time rtc_tm;
    if (ioctl(rtc_fd, RTC_RD_TIME, &rtc_tm) < 0) {
        printf("读取RTC时间失败\n");
        close(rtc_fd);
        return false;
    }
    close(rtc_fd);
    
    // 转换为系统时间
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = rtc_tm.tm_year + 1900 - 1900;
    tm.tm_mon = rtc_tm.tm_mon;
    tm.tm_mday = rtc_tm.tm_mday;
    tm.tm_hour = rtc_tm.tm_hour;
    tm.tm_min = rtc_tm.tm_min;
    tm.tm_sec = rtc_tm.tm_sec;
    tm.tm_isdst = -1;
    
    time_t t = mktime(&tm);
    struct timeval tv = {t, 0};
    
    if (settimeofday(&tv, NULL) < 0) {
        printf("设置系统时间失败（需要root权限）\n");
        return false;
    }
    
    printf("RTC时间同步到系统: %s", ctime(&t));
    return true;
}

void rtc_timer_set_callback(timer_callback_t cb, void *userdata) {
    pthread_mutex_lock(&g_rtc_timer.lock);
    g_rtc_timer.user_callback = cb;
    g_rtc_timer.user_data = userdata;
    pthread_mutex_unlock(&g_rtc_timer.lock);
}