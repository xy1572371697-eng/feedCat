#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * event_loop.h - 基于 Epoll + 线程池的传感器事件驱动框架
 *
 * 替代 feed_core.c 中的 sleep(1) 轮询定时器，改为 epoll 监听
 * /dev/ir_obstacle、/dev/weight_sensor、/dev/aht30 三个设备文件，
 * 由固定大小线程池并发处理各传感器事件，降低 CPU 占用并提升响应实时性。
 */

/* 传感器事件类型 */
typedef enum {
    EVT_IR_OBSTACLE  = 0,   /* 红外遮挡事件 */
    EVT_WEIGHT_LOW   = 1,   /* 缺粮事件 */
    EVT_AHT30_ALARM  = 2,   /* 温湿度异常事件 */
    EVT_TIMER_TICK   = 3,   /* 定时器到期事件（通过 timerfd） */
} sensor_event_type_t;

/* 传感器事件结构 */
typedef struct {
    sensor_event_type_t type;
    int                 fd;      /* 触发事件的设备 fd */
    long long           value;   /* 读取到的原始值 */
} sensor_event_t;

/* 事件回调函数类型 */
typedef void (*event_handler_t)(sensor_event_t *evt, void *userdata);

/* 不透明上下文 */
typedef struct event_loop event_loop_t;

/* 创建事件循环（nthreads: 工作线程数，建议 3-4） */
event_loop_t *event_loop_create(int nthreads);

/* 注册传感器设备文件，关联事件类型和处理回调 */
int event_loop_add_sensor(event_loop_t *loop, const char *devpath,
                          sensor_event_type_t type,
                          event_handler_t handler, void *userdata);

/* 启动事件循环（内部启动 epoll + 线程池，非阻塞返回） */
int event_loop_start(event_loop_t *loop);

/* 停止并销毁事件循环 */
void event_loop_destroy(event_loop_t *loop);

#ifdef __cplusplus
}
#endif
#endif /* EVENT_LOOP_H */
