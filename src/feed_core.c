#include "feed_core.h"
#include "event_loop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sys/time.h>

// ==================== 全局配置 ====================
#define MAX_FOOD_CAPACITY       2000    // 最大容量2000克
#define LOW_FOOD_THRESHOLD      50      // 缺粮阈值50克
#define CALIBRATION_WEIGHT      100     // 校准用标准重量100克
#define MAX_HISTORY_RECORDS     100     // 最大历史记录数
#define MAX_TIMER_TASKS         20      // 最大定时任务数
#define DEFAULT_VIDEO_PORT      8080    // 默认视频端口
#define DEFAULT_JPEG_QUALITY    70      // 默认JPEG质量

// ==================== 喂食记录结构 ====================
typedef struct {
    time_t time;        // 喂食时间戳
    int gram;           // 喂食量（克）
    int type;           // 0=手动, 1=自动, 2=定时
    int success;        // 0=失败, 1=成功
} feed_record_t;

// ==================== 喂食器上下文结构 ====================
struct feeder_context {
    // 硬件设备
    weight_device_t *weight;
    motor_device_t *motor;
    ir_sensor_t *ir;
    camera_device_t *camera;
    video_stream_server_t *video_server;
    
    // 定时任务系统
    timer_task_t timer_tasks[MAX_TIMER_TASKS];
    int task_count;
    pthread_t timer_monitor_thread;
    int timer_running;
    timer_callback_t user_timer_cb;
    void *timer_userdata;
    
    // 状态缓存
    int current_food_gram;
    time_t last_weight_time;
    time_t last_save_time;
    
    // 喂食历史记录
    feed_record_t feed_history[MAX_HISTORY_RECORDS];
    int history_count;
    int today_feed_count;
    int today_total_gram;
    time_t last_stat_reset;
    
    // 回调函数
    key_callback_t key_cb;
    void *key_cb_data;
    feed_callback_t feed_cb;
    void *feed_cb_data;
    status_callback_t status_cb;
    void *status_cb_data;
    
    // 视频流状态
    char video_url[128];
    int video_port;
    int video_quality;
    int video_enabled;
    
    // 校准状态
    int weight_calibrated;
    int motor_calibrated;
    
    // 同步锁
    pthread_mutex_t lock;
    pthread_mutex_t timer_lock;
    pthread_mutex_t history_lock;

    // Epoll 事件驱动循环（替代 sleep(1) 轮询）
    event_loop_t *evloop;
};

// 全局上下文指针（用于回调）
static feeder_context_t *g_ctx = NULL;

// ==================== 内部辅助函数 ====================

/**
 * 重置今日统计（每天零点自动重置）
 */
static void reset_today_stats(feeder_context_t *ctx) {
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    
    // 计算今天的开始时间戳（零点）
    time_t today_start = now - (tm_now->tm_hour * 3600 + 
                                tm_now->tm_min * 60 + 
                                tm_now->tm_sec);
    
    pthread_mutex_lock(&ctx->history_lock);
    
    // 如果上次重置时间不是今天，则重置统计
    if (ctx->last_stat_reset < today_start) {
        ctx->today_feed_count = 0;
        ctx->today_total_gram = 0;
        ctx->last_stat_reset = now;
    }
    
    pthread_mutex_unlock(&ctx->history_lock);
}

/**
 * 添加喂食记录
 */
static void add_feed_record(feeder_context_t *ctx, int gram, int type, int success) {
    pthread_mutex_lock(&ctx->history_lock);
    
    // 滚动存储
    if (ctx->history_count >= MAX_HISTORY_RECORDS) {
        // 移动数组，删除最旧的记录
        memmove(&ctx->feed_history[0], &ctx->feed_history[1],
                (MAX_HISTORY_RECORDS - 1) * sizeof(feed_record_t));
        ctx->history_count = MAX_HISTORY_RECORDS - 1;
    }
    
    // 添加新记录
    feed_record_t *rec = &ctx->feed_history[ctx->history_count++];
    rec->time = time(NULL);
    rec->gram = gram;
    rec->type = type;
    rec->success = success;
    
    // 更新今日统计
    time_t now = rec->time;
    struct tm *tm_now = localtime(&now);
    time_t today_start = now - (tm_now->tm_hour * 3600 + 
                                tm_now->tm_min * 60 + 
                                tm_now->tm_sec);
    
    if (ctx->last_stat_reset < today_start) {
        ctx->today_feed_count = 1;
        ctx->today_total_gram = gram;
        ctx->last_stat_reset = now;
    } else {
        ctx->today_feed_count++;
        ctx->today_total_gram += gram;
    }
    
    pthread_mutex_unlock(&ctx->history_lock);
    
    // 触发喂食回调
    if (ctx->feed_cb) {
        ctx->feed_cb(gram, success, ctx->feed_cb_data);
    }
}

/**
 * 触发状态回调
 */
static void trigger_status_callback(feeder_context_t *ctx) {
    if (ctx->status_cb) {
        feeder_status_t status = feeder_get_status(ctx);
        ctx->status_cb(&status, ctx->status_cb_data);
    }
}

/**
 * 定时任务监控线程
 */
static void* timer_monitor_thread_func(void *arg) {
    feeder_context_t *ctx = (feeder_context_t*)arg;
    
    printf("[定时器] 监控线程已启动\n");
    
    while (ctx->timer_running) {
        time_t now = time(NULL);
        struct tm *tm_now = localtime(&now);
        
        pthread_mutex_lock(&ctx->timer_lock);
        
        for (int i = 0; i < ctx->task_count; i++) {
            timer_task_t *task = &ctx->timer_tasks[i];
            
            // 检查任务是否启用且时间匹配
            if (task->enabled) {
                if (tm_now->tm_hour == task->hour &&
                    tm_now->tm_min == task->minute &&
                    tm_now->tm_sec >= 0 && tm_now->tm_sec < 2) {  // 1秒窗口
                    
                    printf("[定时器] 触发任务 ID=%d: %02d:%02d 喂食%d克\n",
                           task->id, task->hour, task->minute, task->feed_amount);
                    
                    // 调用用户回调
                    if (ctx->user_timer_cb) {
                        ctx->user_timer_cb(task, ctx->timer_userdata);
                    } else {
                        // 默认动作：自动喂食
                        feeder_feed_auto(ctx, task->feed_amount);
                    }
                }
            }
        }
        
        pthread_mutex_unlock(&ctx->timer_lock);
        
        sleep(1);  // 每秒检查一次
    }
    
    printf("[定时器] 监控线程已退出\n");
    return NULL;
}

/* ==================== Epoll 事件回调 ==================== */

/**
 * timerfd 每秒触发一次，替代原 timer_monitor_thread_func 中的 sleep(1)
 * 完成定时任务检测和今日统计重置
 */
static void on_timer_tick(sensor_event_t *evt, void *userdata)
{
    (void)evt;
    feeder_context_t *ctx = (feeder_context_t *)userdata;
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);

    pthread_mutex_lock(&ctx->timer_lock);
    for (int i = 0; i < ctx->task_count; i++) {
        timer_task_t *task = &ctx->timer_tasks[i];
        if (!task->enabled) continue;
        if (tm_now->tm_hour == task->hour &&
            tm_now->tm_min  == task->minute &&
            tm_now->tm_sec  <  2) {
            printf("[事件] 触发定时任务 ID=%d: %02d:%02d 喂食%d克\n",
                   task->id, task->hour, task->minute, task->feed_amount);
            if (ctx->user_timer_cb) {
                ctx->user_timer_cb(task, ctx->timer_userdata);
            } else {
                pthread_mutex_unlock(&ctx->timer_lock);
                feeder_feed_auto(ctx, task->feed_amount);
                pthread_mutex_lock(&ctx->timer_lock);
            }
        }
    }
    pthread_mutex_unlock(&ctx->timer_lock);

    reset_today_stats(ctx);
}

/**
 * IR 传感器 fd 可读时触发，实现 <10ms 低延迟遮挡检测
 */
static void on_ir_event(sensor_event_t *evt, void *userdata)
{
    feeder_context_t *ctx = (feeder_context_t *)userdata;
    int obstacle = (int)evt->value;
    if (obstacle) {
        printf("[事件] 红外检测到遮挡，触发状态回调\n");
        trigger_status_callback(ctx);
    }
}

/**
 * 自动保存状态到文件
 */
static void save_state_to_file(feeder_context_t *ctx) {
    // 每5分钟保存一次
    time_t now = time(NULL);
    if (now - ctx->last_save_time < 300) return;
    
    ctx->last_save_time = now;
    
    // 保存定时任务配置
    FILE *fp = fopen("/etc/feeder_tasks.conf", "w");
    if (fp) {
        pthread_mutex_lock(&ctx->timer_lock);
        fprintf(fp, "# 喂食器定时任务配置文件\n");
        fprintf(fp, "# 格式: ID:启用:小时:分钟:喂食量:重复\n");
        for (int i = 0; i < ctx->task_count; i++) {
            timer_task_t *t = &ctx->timer_tasks[i];
            fprintf(fp, "%d:%d:%d:%d:%d:%d\n",
                    t->id, t->enabled, t->hour, t->minute,
                    t->feed_amount, t->repeat);
        }
        pthread_mutex_unlock(&ctx->timer_lock);
        fclose(fp);
    }
}

// ==================== 初始化/清理 ====================

feeder_context_t* feeder_init(void) {
    feeder_context_t *ctx = calloc(1, sizeof(feeder_context_t));
    if (!ctx) return NULL;
    
    // 初始化互斥锁
    pthread_mutex_init(&ctx->lock, NULL);
    pthread_mutex_init(&ctx->timer_lock, NULL);
    pthread_mutex_init(&ctx->history_lock, NULL);
    
    // 1. 初始化重量传感器
    ctx->weight = weight_open();
    if (!ctx->weight) {
        printf("[警告] 重量传感器初始化失败\n");
    } else {
        // 设置缺粮阈值
        weight_set_threshold(ctx->weight, LOW_FOOD_THRESHOLD);
        printf("[重量] 传感器初始化成功\n");
    }
    
    // 2. 初始化电机
    ctx->motor = motor_open();
    if (!ctx->motor) {
        printf("[警告] 电机初始化失败\n");
    } else {
        printf("[电机] 驱动初始化成功\n");
    }
    
    // 3. 初始化红外传感器
    ctx->ir = ir_sensor_open();
    if (!ctx->ir) {
        printf("[警告] 红外传感器初始化失败\n");
    } else {
        printf("[红外] 传感器初始化成功\n");
    }
    
    // 4. 初始化状态缓存
    ctx->current_food_gram = -1;
    ctx->last_weight_time = 0;
    ctx->last_save_time = time(NULL);
    ctx->last_stat_reset = time(NULL);
    
    // 5. 定时任务系统
    ctx->task_count = 0;
    ctx->timer_running = 1;
    memset(ctx->timer_tasks, 0, sizeof(ctx->timer_tasks));
    
    // 启动 Epoll + 线程池事件循环（替代 sleep(1) 轮询）
    ctx->evloop = event_loop_create(3);
    if (ctx->evloop) {
        // 注册 timerfd（每秒触发，替代 timer_monitor_thread_func 的 sleep(1)）
        event_loop_add_sensor(ctx->evloop, NULL, EVT_TIMER_TICK,
                              on_timer_tick, ctx);
        // 注册红外传感器（边沿触发，<10ms 响应）
        if (ctx->ir) {
            event_loop_add_sensor(ctx->evloop, "/dev/ir_obstacle",
                                  EVT_IR_OBSTACLE, on_ir_event, ctx);
        }
        event_loop_start(ctx->evloop);
        // evloop 已接管定时任务检测，无需旧轮询线程
        ctx->timer_running = 0;
    } else {
        // evloop 创建失败，回退到原始轮询线程
        printf("[警告] event_loop 创建失败，回退到 sleep(1) 轮询\n");
        ctx->timer_running = 1;
        if (pthread_create(&ctx->timer_monitor_thread, NULL,
                           timer_monitor_thread_func, ctx) != 0) {
            printf("[错误] 无法创建定时器监控线程\n");
            ctx->timer_running = 0;
        }
    }

    // 6. 视频流默认配置
    ctx->video_port = DEFAULT_VIDEO_PORT;
    ctx->video_quality = DEFAULT_JPEG_QUALITY;
    ctx->video_enabled = 0;
    snprintf(ctx->video_url, sizeof(ctx->video_url),
             "http://0.0.0.0:%d/stream", ctx->video_port);
    
    // 7. 校准状态
    ctx->weight_calibrated = 0;
    ctx->motor_calibrated = 0;
    
    // 8. 设置全局上下文
    g_ctx = ctx;
    
    printf("\n========================================\n");
    printf("✅ 智能喂食器核心初始化完成\n");
    printf("📊 缺粮阈值: %d克\n", LOW_FOOD_THRESHOLD);
    printf("🕒 当前时间: %s", ctime(&(time_t){time(NULL)}));
    printf("========================================\n\n");
    
    return ctx;
}

void feeder_deinit(feeder_context_t *ctx) {
    if (!ctx) return;
    
    printf("\n正在关闭喂食器系统...\n");
    
    // 1. 停止事件循环 / 定时器监控线程
    if (ctx->evloop) {
        event_loop_destroy(ctx->evloop);
        ctx->evloop = NULL;
    } else if (ctx->timer_running) {
        ctx->timer_running = 0;
        pthread_join(ctx->timer_monitor_thread, NULL);
    }
    
    // 2. 紧急停止电机
    feeder_emergency_stop(ctx);
    
    // 3. 停止视频流
    if (ctx->video_server) {
        video_stream_stop(ctx->video_server);
        ctx->video_server = NULL;
    }
    
    // 4. 关闭摄像头
    if (ctx->camera) {
        camera_close(ctx->camera);
        ctx->camera = NULL;
        printf("[摄像头] 已关闭\n");
    }
    
    // 5. 关闭硬件设备
    if (ctx->weight) {
        weight_close(ctx->weight);
        ctx->weight = NULL;
        printf("[重量] 传感器已关闭\n");
    }
    
    if (ctx->motor) {
        motor_close(ctx->motor);
        ctx->motor = NULL;
        printf("[电机] 驱动已关闭\n");
    }
    
    if (ctx->ir) {
        ir_sensor_close(ctx->ir);
        ctx->ir = NULL;
        printf("[红外] 传感器已关闭\n");
    }
    
    // 6. 销毁互斥锁
    pthread_mutex_destroy(&ctx->lock);
    pthread_mutex_destroy(&ctx->timer_lock);
    pthread_mutex_destroy(&ctx->history_lock);
    
    // 7. 清理全局指针
    if (g_ctx == ctx) {
        g_ctx = NULL;
    }
    
    free(ctx);
    printf("✅ 喂食器系统已完全关闭\n");
}

// ==================== 喂食控制 ====================

bool feeder_feed_manual(feeder_context_t *ctx, int gram) {
    if (!ctx) return false;
    if (gram <= 0) {
        printf("[喂食] 错误: 喂食量必须大于0\n");
        return false;
    }
    if (gram > 100) {
        printf("[喂食] 警告: 单次喂食量超过100克，已限制为100克\n");
        gram = 100;
    }
    
    pthread_mutex_lock(&ctx->lock);
    
    printf("\n🔵 [手动喂食] 目标: %d克\n", gram);
    
    // 1. 读取喂食前重量
    int before = weight_read_gram(ctx->weight);
    if (before < 0) {
        printf("[喂食] 错误: 无法读取重量传感器\n");
        pthread_mutex_unlock(&ctx->lock);
        add_feed_record(ctx, gram, 0, 0);
        return false;
    }
    
    // 2. 检查余粮是否足够
    if (before < gram + 50) {
        printf("[喂食] 失败: 余粮不足 (当前%d克, 需要%d克)\n", before, gram + 50);
        pthread_mutex_unlock(&ctx->lock);
        add_feed_record(ctx, gram, 0, 0);
        return false;
    }
    
    // 3. 启动电机喂食
    bool ret = motor_feed_gram(ctx->motor, gram);
    
    // 4. 读取喂食后重量
    int after = weight_read_gram(ctx->weight);
    int actual = before - after;
    if (actual < 0) actual = 0;
    
    // 5. 记录结果
    if (ret) {
        printf("✅ [手动喂食] 完成: 目标=%d克, 实际=%d克, 剩余=%d克\n",
               gram, actual, after);
    } else {
        printf("❌ [手动喂食] 失败: 电机故障\n");
    }
    
    pthread_mutex_unlock(&ctx->lock);
    
    // 6. 添加喂食记录
    add_feed_record(ctx, actual > 0 ? actual : gram, 0, ret ? 1 : 0);
    
    // 7. 触发状态回调
    trigger_status_callback(ctx);
    
    return ret;
}

bool feeder_feed_auto(feeder_context_t *ctx, int gram) {
    if (!ctx) return false;
    if (gram <= 0) return false;
    if (gram > 100) gram = 100;
    
    pthread_mutex_lock(&ctx->lock);
    
    printf("\n🟢 [自动喂食] 目标: %d克\n", gram);
    
    // 1. 读取当前重量
    int food = weight_read_gram(ctx->weight);
    if (food < 0) {
        pthread_mutex_unlock(&ctx->lock);
        return false;
    }
    
    // 2. 检查余粮
    if (food < gram + 50) {
        printf("[自动喂食] 取消: 余粮不足 (当前%d克, 需要%d克)\n", 
               food, gram + 50);
        pthread_mutex_unlock(&ctx->lock);
        add_feed_record(ctx, gram, 1, 0);
        return false;
    }
    
    // 3. 执行喂食
    bool ret = motor_feed_gram(ctx->motor, gram);
    
    if (ret) {
        printf("✅ [自动喂食] 成功: %d克\n", gram);
    } else {
        printf("❌ [自动喂食] 失败\n");
    }
    
    pthread_mutex_unlock(&ctx->lock);
    
    add_feed_record(ctx, gram, 1, ret ? 1 : 0);
    trigger_status_callback(ctx);
    
    return ret;
}

bool feeder_feed_timer(feeder_context_t *ctx, int gram) {
    // 定时喂食与自动喂食逻辑相同，但记录类型为2
    if (!ctx) return false;
    
    pthread_mutex_lock(&ctx->lock);
    bool ret = feeder_feed_auto(ctx, gram);
    pthread_mutex_unlock(&ctx->lock);
    
    if (ret) {
        // 覆盖记录类型为定时
        pthread_mutex_lock(&ctx->history_lock);
        if (ctx->history_count > 0) {
            ctx->feed_history[ctx->history_count - 1].type = 2;
        }
        pthread_mutex_unlock(&ctx->history_lock);
    }
    
    return ret;
}

void feeder_emergency_stop(feeder_context_t *ctx) {
    if (!ctx || !ctx->motor) return;
    
    pthread_mutex_lock(&ctx->lock);
    motor_emergency_stop(ctx->motor);
    pthread_mutex_unlock(&ctx->lock);
    
    printf("⚠️ [紧急停止] 电机已停止\n");
    trigger_status_callback(ctx);
}

void feeder_register_feed_callback(feeder_context_t *ctx, 
                                   feed_callback_t cb, 
                                   void *userdata) {
    if (!ctx) return;
    pthread_mutex_lock(&ctx->lock);
    ctx->feed_cb = cb;
    ctx->feed_cb_data = userdata;
    pthread_mutex_unlock(&ctx->lock);
}

// ==================== 定时任务管理 ====================

int feeder_add_timer_task(feeder_context_t *ctx, timer_task_t *task) {
    if (!ctx || !task) return -1;
    
    pthread_mutex_lock(&ctx->timer_lock);
    
    if (ctx->task_count >= MAX_TIMER_TASKS) {
        pthread_mutex_unlock(&ctx->timer_lock);
        printf("[定时器] 添加失败: 任务数量已达上限\n");
        return -1;
    }
    
    int id = ctx->task_count;
    ctx->timer_tasks[id] = *task;
    ctx->timer_tasks[id].id = id;
    ctx->task_count++;
    
    pthread_mutex_unlock(&ctx->timer_lock);
    
    printf("[定时器] 添加任务 ID=%d: %02d:%02d 喂食%d克 [%s]\n",
           id, task->hour, task->minute, task->feed_amount,
           task->repeat ? "每天" : "单次");
    
    // 保存到文件
    save_state_to_file(ctx);
    
    return id;
}

bool feeder_remove_timer_task(feeder_context_t *ctx, int task_id) {
    if (!ctx) return false;
    
    pthread_mutex_lock(&ctx->timer_lock);
    
    if (task_id < 0 || task_id >= ctx->task_count) {
        pthread_mutex_unlock(&ctx->timer_lock);
        return false;
    }
    
    // 删除任务（标记为未启用）
    ctx->timer_tasks[task_id].enabled = 0;
    
    pthread_mutex_unlock(&ctx->timer_lock);
    
    printf("[定时器] 删除任务 ID=%d\n", task_id);
    
    // 保存到文件
    save_state_to_file(ctx);
    
    return true;
}

bool feeder_modify_timer_task(feeder_context_t *ctx, timer_task_t *task) {
    if (!ctx || !task) return false;
    
    pthread_mutex_lock(&ctx->timer_lock);
    
    if (task->id < 0 || task->id >= ctx->task_count) {
        pthread_mutex_unlock(&ctx->timer_lock);
        return false;
    }
    
    ctx->timer_tasks[task->id] = *task;
    
    pthread_mutex_unlock(&ctx->timer_lock);
    
    printf("[定时器] 修改任务 ID=%d: %02d:%02d 喂食%d克\n",
           task->id, task->hour, task->minute, task->feed_amount);
    
    // 保存到文件
    save_state_to_file(ctx);
    
    return true;
}

int feeder_get_timer_tasks(feeder_context_t *ctx, timer_task_t *tasks, int max) {
    if (!ctx || !tasks || max <= 0) return 0;
    
    pthread_mutex_lock(&ctx->timer_lock);
    
    int count = ctx->task_count < max ? ctx->task_count : max;
    for (int i = 0; i < count; i++) {
        tasks[i] = ctx->timer_tasks[i];
    }
    
    pthread_mutex_unlock(&ctx->timer_lock);
    
    return count;
}

bool feeder_enable_timer_task(feeder_context_t *ctx, int task_id, int enable) {
    if (!ctx) return false;
    
    pthread_mutex_lock(&ctx->timer_lock);
    
    if (task_id < 0 || task_id >= ctx->task_count) {
        pthread_mutex_unlock(&ctx->timer_lock);
        return false;
    }
    
    ctx->timer_tasks[task_id].enabled = enable;
    
    pthread_mutex_unlock(&ctx->timer_lock);
    
    printf("[定时器] %s任务 ID=%d\n", enable ? "启用" : "禁用", task_id);
    
    // 保存到文件
    save_state_to_file(ctx);
    
    return true;
}

void feeder_set_timer_callback(feeder_context_t *ctx, 
                               timer_callback_t cb, 
                               void *userdata) {
    if (!ctx) return;
    
    pthread_mutex_lock(&ctx->timer_lock);
    ctx->user_timer_cb = cb;
    ctx->timer_userdata = userdata;
    pthread_mutex_unlock(&ctx->timer_lock);
}

// ==================== 状态查询 ====================

feeder_status_t feeder_get_status(feeder_context_t *ctx) {
    feeder_status_t status = {0};
    if (!ctx) return status;
    
    pthread_mutex_lock(&ctx->lock);
    
    // 1. 获取余粮
    status.food_gram = weight_read_gram(ctx->weight);
    if (status.food_gram < 0) {
        status.food_gram = ctx->current_food_gram > 0 ? ctx->current_food_gram : 0;
    } else {
        ctx->current_food_gram = status.food_gram;
    }
    
    status.food_percentage = (float)status.food_gram / MAX_FOOD_CAPACITY * 100;
    if (status.food_percentage > 100) status.food_percentage = 100;
    if (status.food_percentage < 0) status.food_percentage = 0;
    
    // 2. 检查缺粮
    status.low_food_warning = (status.food_gram < LOW_FOOD_THRESHOLD);
    
    // 3. 获取电机状态（motor.h中没有motor_is_running，使用其他方式）
    // 暂时无法获取电机运行状态，设为0
    status.motor_running = 0;
    
    // 4. 获取红外状态
    int ir_state = ir_sensor_get_state(ctx->ir);
    status.ir_obstacle = (ir_state == 1);
    
    // 5. 获取当前时间
    status.current_time = time(NULL);
    
    pthread_mutex_unlock(&ctx->lock);
    
    // 6. 获取今日统计
    reset_today_stats(ctx);
    
    pthread_mutex_lock(&ctx->history_lock);
    status.today_feed_count = ctx->today_feed_count;
    status.today_total_gram = ctx->today_total_gram;
    pthread_mutex_unlock(&ctx->history_lock);
    
    // 7. 摄像头状态
    if (ctx->camera) {
        status.camera_present = 1;
        status.camera_streaming = (ctx->video_server != NULL);
        status.camera_clients = ctx->video_server ? 
            video_stream_get_client_count(ctx->video_server) : 0;
        status.camera_width = camera_get_width(ctx->camera);
        status.camera_height = camera_get_height(ctx->camera);
        // camera.h中没有camera_get_framerate，使用默认值
        status.camera_fps = 15;
    }
    
    return status;
}

int feeder_get_food_gram(feeder_context_t *ctx) {
    if (!ctx) return 0;
    
    int weight = weight_read_gram(ctx->weight);
    if (weight > 0) {
        ctx->current_food_gram = weight;
        return weight;
    }
    
    return ctx->current_food_gram > 0 ? ctx->current_food_gram : 0;
}

float feeder_get_food_percentage(feeder_context_t *ctx) {
    int gram = feeder_get_food_gram(ctx);
    float percent = (float)gram / MAX_FOOD_CAPACITY * 100;
    if (percent > 100) percent = 100;
    if (percent < 0) percent = 0;
    return percent;
}

int feeder_get_today_feed_count(feeder_context_t *ctx) {
    if (!ctx) return 0;
    reset_today_stats(ctx);
    
    pthread_mutex_lock(&ctx->history_lock);
    int count = ctx->today_feed_count;
    pthread_mutex_unlock(&ctx->history_lock);
    
    return count;
}

int feeder_get_today_total_gram(feeder_context_t *ctx) {
    if (!ctx) return 0;
    reset_today_stats(ctx);
    
    pthread_mutex_lock(&ctx->history_lock);
    int total = ctx->today_total_gram;
    pthread_mutex_unlock(&ctx->history_lock);
    
    return total;
}

bool feeder_is_low_food(feeder_context_t *ctx) {
    int gram = feeder_get_food_gram(ctx);
    return gram < LOW_FOOD_THRESHOLD;
}

int feeder_get_feed_history_count(feeder_context_t *ctx) {
    if (!ctx) return 0;
    
    pthread_mutex_lock(&ctx->history_lock);
    int count = ctx->history_count;
    pthread_mutex_unlock(&ctx->history_lock);
    
    return count;
}

bool feeder_get_feed_history(feeder_context_t *ctx, feed_history_item_t *items, int max_items) {
    if (!ctx || !items || max_items <= 0) return false;
    
    pthread_mutex_lock(&ctx->history_lock);
    
    int count = ctx->history_count < max_items ? ctx->history_count : max_items;
    for (int i = 0; i < count; i++) {
        items[i].timestamp = ctx->feed_history[i].time;
        items[i].gram = ctx->feed_history[i].gram;
        items[i].type = ctx->feed_history[i].type;
        items[i].success = ctx->feed_history[i].success;
    }
    
    pthread_mutex_unlock(&ctx->history_lock);
    
    return true;
}

bool feeder_clear_feed_history(feeder_context_t *ctx) {
    if (!ctx) return false;
    
    pthread_mutex_lock(&ctx->history_lock);
    ctx->history_count = 0;
    ctx->today_feed_count = 0;
    ctx->today_total_gram = 0;
    pthread_mutex_unlock(&ctx->history_lock);
    
    return true;
}

void feeder_register_status_callback(feeder_context_t *ctx,
                                     status_callback_t cb,
                                     void *userdata) {
    if (!ctx) return;
    pthread_mutex_lock(&ctx->lock);
    ctx->status_cb = cb;
    ctx->status_cb_data = userdata;
    pthread_mutex_unlock(&ctx->lock);
}

// ==================== 校准功能 ====================

bool feeder_calibrate_weight_zero(feeder_context_t *ctx) {
    if (!ctx || !ctx->weight) return false;
    
    printf("\n⚖️ [重量校准] 正在进行零点校准...\n");
    
    bool ret = weight_calibrate_zero(ctx->weight);
    
    if (ret) {
        printf("✅ [重量校准] 零点校准完成\n");
        ctx->weight_calibrated = 1;
    } else {
        printf("❌ [重量校准] 失败\n");
    }
    
    return ret;
}

bool feeder_calibrate_weight_with_gram(feeder_context_t *ctx, int known_gram) {
    if (!ctx || !ctx->weight) return false;
    if (known_gram <= 0) return false;
    
    printf("\n⚖️ [重量校准] 正在进行重量校准...\n");
    
    bool ret = weight_calibrate_with_weight(ctx->weight, known_gram);
    
    if (ret) {
        printf("✅ [重量校准] 完成，校准系数已设置\n");
        ctx->weight_calibrated = 1;
    } else {
        printf("❌ [重量校准] 失败\n");
    }
    
    return ret;
}

bool feeder_calibrate_motor(feeder_context_t *ctx, int test_gram, int actual_ms) {
    if (!ctx || !ctx->motor) return false;
    if (test_gram <= 0 || actual_ms <= 0) return false;
    
    printf("\n⚙️ [电机校准] 校准功能已移除\n");
    printf("电机驱动不再支持时间校准，使用固定PWM占空比\n");
    
    ctx->motor_calibrated = 1;
    return true;
}

bool feeder_get_calibration_status(feeder_context_t *ctx,
                                   int *weight_calibrated,
                                   int *motor_calibrated) {
    if (!ctx) return false;
    
    if (weight_calibrated) *weight_calibrated = ctx->weight_calibrated;
    if (motor_calibrated) *motor_calibrated = ctx->motor_calibrated;
    
    return true;
}

// ==================== 摄像头/视频流 ====================

bool feeder_camera_init(feeder_context_t *ctx, int dev_id) {
    if (!ctx) return false;
    
    pthread_mutex_lock(&ctx->lock);
    
    // 关闭已有摄像头
    if (ctx->camera) {
        camera_close(ctx->camera);
        ctx->camera = NULL;
    }
    
    // 打开摄像头
    ctx->camera = camera_open(dev_id);
    if (!ctx->camera) {
        printf("[摄像头] 初始化失败: /dev/video%d\n", dev_id);
        pthread_mutex_unlock(&ctx->lock);
        return false;
    }
    
    // 设置默认参数
    camera_set_resolution(ctx->camera, 640, 480);
    camera_set_format(ctx->camera, CAMERA_FMT_YUYV);
    // camera.h中没有camera_set_framerate
    
    pthread_mutex_unlock(&ctx->lock);
    
    printf("[摄像头] 初始化成功: /dev/video%d, 640x480\n", dev_id);
    
    return true;
}

void feeder_camera_close(feeder_context_t *ctx) {
    if (!ctx) return;
    
    pthread_mutex_lock(&ctx->lock);
    
    if (ctx->camera) {
        camera_close(ctx->camera);
        ctx->camera = NULL;
        printf("[摄像头] 已关闭\n");
    }
    
    pthread_mutex_unlock(&ctx->lock);
}

bool feeder_video_start(feeder_context_t *ctx, int port, int quality) {
    if (!ctx || !ctx->camera) {
        printf("[视频] 错误: 摄像头未初始化\n");
        return false;
    }
    
    pthread_mutex_lock(&ctx->lock);
    
    // 停止已有服务器
    if (ctx->video_server) {
        video_stream_stop(ctx->video_server);
        ctx->video_server = NULL;
    }
    
    // 设置参数
    ctx->video_port = port > 0 ? port : DEFAULT_VIDEO_PORT;
    ctx->video_quality = quality > 0 ? quality : DEFAULT_JPEG_QUALITY;
    
    // 启动视频流服务器
    ctx->video_server = video_stream_start(ctx->video_port, ctx->camera, ctx->video_quality);
    if (!ctx->video_server) {
        printf("[视频] 服务器启动失败\n");
        pthread_mutex_unlock(&ctx->lock);
        return false;
    }
    
    ctx->video_enabled = 1;
    snprintf(ctx->video_url, sizeof(ctx->video_url),
             "http://0.0.0.0:%d/stream", ctx->video_port);
    
    pthread_mutex_unlock(&ctx->lock);
    
    printf("[视频] 流服务器启动成功\n");
    printf("[视频] 访问地址: %s\n", ctx->video_url);
    
    return true;
}

void feeder_video_stop(feeder_context_t *ctx) {
    if (!ctx) return;
    
    pthread_mutex_lock(&ctx->lock);
    
    if (ctx->video_server) {
        video_stream_stop(ctx->video_server);
        ctx->video_server = NULL;
        ctx->video_enabled = 0;
        printf("[视频] 流服务器已停止\n");
    }
    
    pthread_mutex_unlock(&ctx->lock);
}

camera_status_t feeder_camera_get_status(feeder_context_t *ctx) {
    camera_status_t status = {0};
    
    if (!ctx) return status;
    
    pthread_mutex_lock(&ctx->lock);
    
    if (ctx->camera) {
        status.camera_present = 1;
        status.streaming = (ctx->video_server != NULL);
        status.client_count = ctx->video_server ? 
            video_stream_get_client_count(ctx->video_server) : 0;
        status.width = camera_get_width(ctx->camera);
        status.height = camera_get_height(ctx->camera);
        status.fps = 15;  // 默认值
    }
    
    pthread_mutex_unlock(&ctx->lock);
    
    return status;
}

bool feeder_camera_set_resolution(feeder_context_t *ctx, int width, int height) {
    if (!ctx || !ctx->camera) return false;
    
    pthread_mutex_lock(&ctx->lock);
    
    bool ret = camera_set_resolution(ctx->camera, width, height);
    
    pthread_mutex_unlock(&ctx->lock);
    
    if (ret) {
        printf("[摄像头] 分辨率设置为: %dx%d\n", width, height);
    }
    
    return ret;
}

bool feeder_camera_set_framerate(feeder_context_t *ctx, int fps) {
    if (!ctx || !ctx->camera) return false;
    
    // camera.h中没有camera_set_framerate
    printf("[摄像头] 帧率设置暂不支持\n");
    return false;
}

bool feeder_camera_set_quality(feeder_context_t *ctx, int quality) {
    if (!ctx) return false;
    
    if (quality < 10) quality = 10;
    if (quality > 100) quality = 100;
    
    pthread_mutex_lock(&ctx->lock);
    ctx->video_quality = quality;
    
    if (ctx->video_server) {
        video_stream_set_quality(ctx->video_server, quality);
    }
    
    pthread_mutex_unlock(&ctx->lock);
    
    printf("[视频] JPEG质量设置为: %d%%\n", quality);
    
    return true;
}

bool feeder_camera_take_photo(feeder_context_t *ctx, const char *filename) {
    if (!ctx || !ctx->camera) {
        printf("[拍照] 错误: 摄像头未初始化\n");
        return false;
    }
    
    pthread_mutex_lock(&ctx->lock);
    
    // 分配缓冲区
    uint8_t *yuyv = malloc(1280 * 720 * 2);
    uint8_t *jpeg = malloc(2 * 1024 * 1024);
    
    if (!yuyv || !jpeg) {
        free(yuyv);
        free(jpeg);
        pthread_mutex_unlock(&ctx->lock);
        printf("[拍照] 错误: 内存不足\n");
        return false;
    }
    
    // 保存当前分辨率
    int width = camera_get_width(ctx->camera);
    int height = camera_get_height(ctx->camera);
    size_t frame_size = width * height * 2;
    
    // 捕获一帧
    size_t bytes_used;
    if (!camera_capture_frame(ctx->camera, yuyv, frame_size, &bytes_used)) {
        free(yuyv);
        free(jpeg);
        pthread_mutex_unlock(&ctx->lock);
        printf("[拍照] 错误: 捕获帧失败\n");
        return false;
    }
    
    // 转换为JPEG
    size_t jpeg_size = 2 * 1024 * 1024;
    camera_yuyv_to_jpeg(yuyv, jpeg, &jpeg_size, width, height, 
                        ctx->video_quality);
    
    // 保存文件
    FILE *fp = fopen(filename, "wb");
    if (fp) {
        fwrite(jpeg, 1, jpeg_size, fp);
        fclose(fp);
        printf("[拍照] 已保存: %s (%zu KB)\n", filename, jpeg_size / 1024);
    } else {
        printf("[拍照] 错误: 无法创建文件 %s\n", filename);
        free(yuyv);
        free(jpeg);
        pthread_mutex_unlock(&ctx->lock);
        return false;
    }
    
    free(yuyv);
    free(jpeg);
    
    pthread_mutex_unlock(&ctx->lock);
    
    return true;
}

const char* feeder_video_get_url(feeder_context_t *ctx) {
    return ctx ? ctx->video_url : NULL;
}

int feeder_video_get_client_count(feeder_context_t *ctx) {
    if (!ctx || !ctx->video_server) return 0;
    return video_stream_get_client_count(ctx->video_server);
}

// ==================== 红外传感器 ====================

bool feeder_ir_check(feeder_context_t *ctx) {
    if (!ctx || !ctx->ir) return false;
    
    int ret = ir_sensor_check(ctx->ir);
    return (ret == 1);
}

bool feeder_ir_wait_obstacle(feeder_context_t *ctx, int timeout_ms) {
    if (!ctx || !ctx->ir) return false;
    
    return ir_sensor_wait_obstacle(ctx->ir, timeout_ms);
}

// ==================== RTC时间管理 ====================

bool feeder_sync_time_from_rtc(feeder_context_t *ctx) {
    (void)ctx;  // 未使用参数
    
    // rtc_timer.h中没有rtc_sync_to_system，直接返回false
    printf("[RTC] 时间同步功能暂不可用\n");
    return false;
}

bool feeder_sync_time_to_rtc(feeder_context_t *ctx) {
    (void)ctx;
    
    // rtc_timer.h中没有rtc_sync_to_hardware，直接返回false
    printf("[RTC] 时间同步功能暂不可用\n");
    return false;
}

time_t feeder_get_current_time(feeder_context_t *ctx) {
    (void)ctx;
    return time(NULL);
}

bool feeder_set_system_time(feeder_context_t *ctx, time_t t) {
    (void)ctx;
    
    struct timeval tv = {t, 0};
    if (settimeofday(&tv, NULL) == 0) {
        printf("[时间] 系统时间设置为: %s", ctime(&t));
        return true;
    }
    
    printf("[时间] 设置失败（需要root权限）\n");
    return false;
}

void feeder_format_time(time_t t, char *buf, size_t len) {
    struct tm *tm_info = localtime(&t);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", tm_info);
}

// ==================== 按键回调 ====================

void feeder_register_key_callback(feeder_context_t *ctx, 
                                  key_callback_t cb, 
                                  void *userdata) {
    if (!ctx) return;
    
    pthread_mutex_lock(&ctx->lock);
    ctx->key_cb = cb;
    ctx->key_cb_data = userdata;
    pthread_mutex_unlock(&ctx->lock);
}

// 按键事件处理函数（由底层驱动调用）
void feeder_on_key_event(int key_id, int pressed) {
    if (!g_ctx) return;
    
    feeder_context_t *ctx = g_ctx;
    
    pthread_mutex_lock(&ctx->lock);
    
    if (ctx->key_cb) {
        ctx->key_cb(key_id, pressed, ctx->key_cb_data);
    } else {
        // 默认按键处理
        if (pressed) {
            switch (key_id) {
                case 0:
                    printf("[按键] KEY0: 手动喂食20克\n");
                    feeder_feed_manual(ctx, 20);
                    break;
                case 1:
                    printf("[按键] KEY1: 紧急停止\n");
                    feeder_emergency_stop(ctx);
                    break;
                case 2:
                    printf("[按键] KEY2: 显示状态\n");
                    // 状态显示由应用层处理
                    break;
            }
        }
    }
    
    pthread_mutex_unlock(&ctx->lock);
}

// ==================== 配置管理 ====================

bool feeder_save_config(feeder_context_t *ctx, const char *filename) {
    if (!ctx || !filename) return false;
    
    FILE *fp = fopen(filename, "w");
    if (!fp) return false;
    
    pthread_mutex_lock(&ctx->timer_lock);
    
    fprintf(fp, "# 喂食器配置文件\n");
    fprintf(fp, "weight_calibrated=%d\n", ctx->weight_calibrated);
    fprintf(fp, "motor_calibrated=%d\n", ctx->motor_calibrated);
    fprintf(fp, "video_quality=%d\n", ctx->video_quality);
    fprintf(fp, "video_port=%d\n", ctx->video_port);
    
    // 保存定时任务
    fprintf(fp, "\n# 定时任务\n");
    fprintf(fp, "timer_task_count=%d\n", ctx->task_count);
    for (int i = 0; i < ctx->task_count; i++) {
        timer_task_t *t = &ctx->timer_tasks[i];
        fprintf(fp, "task%d=%d:%d:%d:%d:%d\n", i,
                t->enabled, t->hour, t->minute,
                t->feed_amount, t->repeat);
    }
    
    pthread_mutex_unlock(&ctx->timer_lock);
    fclose(fp);
    
    printf("[配置] 已保存到 %s\n", filename);
    return true;
}

bool feeder_load_config(feeder_context_t *ctx, const char *filename) {
    if (!ctx || !filename) return false;
    
    FILE *fp = fopen(filename, "r");
    if (!fp) return false;
    
    char line[256];
    int task_count = 0;
    
    pthread_mutex_lock(&ctx->timer_lock);
    
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || strlen(line) < 3) continue;
        
        if (sscanf(line, "weight_calibrated=%d", &ctx->weight_calibrated) == 1) continue;
        if (sscanf(line, "motor_calibrated=%d", &ctx->motor_calibrated) == 1) continue;
        if (sscanf(line, "video_quality=%d", &ctx->video_quality) == 1) continue;
        if (sscanf(line, "video_port=%d", &ctx->video_port) == 1) continue;
        if (sscanf(line, "timer_task_count=%d", &task_count) == 1) continue;
        
        // 解析定时任务
        int id;
        if (sscanf(line, "task%d=%d:%d:%d:%d:%d", 
                   &id, &ctx->timer_tasks[id].enabled,
                   &ctx->timer_tasks[id].hour,
                   &ctx->timer_tasks[id].minute,
                   &ctx->timer_tasks[id].feed_amount,
                   &ctx->timer_tasks[id].repeat) == 6) {
            ctx->timer_tasks[id].id = id;
            if (id >= ctx->task_count) ctx->task_count = id + 1;
        }
    }
    
    pthread_mutex_unlock(&ctx->timer_lock);
    fclose(fp);
    
    printf("[配置] 已从 %s 加载\n", filename);
    return true;
}

bool feeder_reset_to_default(feeder_context_t *ctx) {
    if (!ctx) return false;
    
    pthread_mutex_lock(&ctx->timer_lock);
    
    ctx->weight_calibrated = 0;
    ctx->motor_calibrated = 0;
    ctx->video_quality = DEFAULT_JPEG_QUALITY;
    ctx->video_port = DEFAULT_VIDEO_PORT;
    ctx->task_count = 0;
    memset(ctx->timer_tasks, 0, sizeof(ctx->timer_tasks));
    
    pthread_mutex_unlock(&ctx->timer_lock);
    
    printf("[配置] 已恢复出厂设置\n");
    return true;
}

// ==================== 系统命令 ====================

bool feeder_system_command(feeder_context_t *ctx, const char *cmd, char *output, int max_len) {
    if (!ctx || !cmd || !output) return false;
    
    if (strcmp(cmd, "status") == 0) {
        feeder_status_t s = feeder_get_status(ctx);
        char time_buf[64];
        feeder_format_time(s.current_time, time_buf, sizeof(time_buf));
        
        snprintf(output, max_len,
            "余粮: %d克 (%.1f%%)\n"
            "今日喂食: %d次, 共%d克\n"
            "电机: %s\n"
            "红外: %s\n"
            "缺粮警告: %s\n"
            "摄像头: %s\n"
            "观看人数: %d\n"
            "时间: %s",
            s.food_gram, s.food_percentage,
            s.today_feed_count, s.today_total_gram,
            s.motor_running ? "运行" : "停止",
            s.ir_obstacle ? "遮挡" : "正常",
            s.low_food_warning ? "缺粮" : "正常",
            s.camera_present ? "已连接" : "未连接",
            s.camera_clients,
            time_buf);
        return true;
    }
    else if (strcmp(cmd, "help") == 0) {
        snprintf(output, max_len,
            "可用命令:\n"
            "  status        - 显示状态\n"
            "  feed <克>     - 手动喂食\n"
            "  stop          - 紧急停止\n"
            "  calibrate     - 重量校准\n"
            "  photo         - 拍照\n"
            "  video start   - 启动视频流\n"
            "  video stop    - 停止视频流\n"
            "  help          - 显示帮助\n");
        return true;
    }
    else if (strncmp(cmd, "feed ", 5) == 0) {
        int gram = atoi(cmd + 5);
        if (gram > 0) {
            bool ret = feeder_feed_manual(ctx, gram);
            snprintf(output, max_len, ret ? "喂食成功: %d克" : "喂食失败", gram);
            return true;
        }
    }
    else if (strcmp(cmd, "stop") == 0) {
        feeder_emergency_stop(ctx);
        snprintf(output, max_len, "已发送停止命令");
        return true;
    }
    else if (strcmp(cmd, "calibrate") == 0) {
        bool ret = feeder_calibrate_weight_zero(ctx);
        snprintf(output, max_len, ret ? "校准成功" : "校准失败");
        return true;
    }
    else if (strcmp(cmd, "photo") == 0) {
        char filename[128];
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        snprintf(filename, sizeof(filename), "/tmp/photo_%04d%02d%02d_%02d%02d%02d.jpg",
                 tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday,
                 tm->tm_hour, tm->tm_min, tm->tm_sec);
        
        bool ret = feeder_camera_take_photo(ctx, filename);
        snprintf(output, max_len, ret ? "照片已保存: %s" : "拍照失败", filename);
        return true;
    }
    else if (strcmp(cmd, "video start") == 0) {
        bool ret = feeder_video_start(ctx, ctx->video_port, ctx->video_quality);
        snprintf(output, max_len, ret ? "视频流已启动" : "启动失败");
        return true;
    }
    else if (strcmp(cmd, "video stop") == 0) {
        feeder_video_stop(ctx);
        snprintf(output, max_len, "视频流已停止");
        return true;
    }
    
    return false;
}

// ==================== 版本信息 ====================

const char* feeder_get_version(void) {
    return FEEDER_VERSION;
}

void feeder_dump_info(feeder_context_t *ctx) {
    if (!ctx) return;
    
    printf("\n========== 喂食器系统信息 ==========\n");
    printf("版本: %s\n", FEEDER_VERSION);
    
    feeder_status_t status = feeder_get_status(ctx);
    printf("余粮: %d克 (%.1f%%)\n", status.food_gram, status.food_percentage);
    printf("今日喂食: %d次, %d克\n", status.today_feed_count, status.today_total_gram);
    printf("红外状态: %s\n", status.ir_obstacle ? "遮挡" : "正常");
    printf("电机状态: %s\n", status.motor_running ? "运行" : "停止");
    printf("摄像头: %s\n", status.camera_present ? "已连接" : "未连接");
    if (status.camera_present) {
        printf("  分辨率: %dx%d\n", status.camera_width, status.camera_height);
        printf("  推流: %s\n", status.camera_streaming ? "是" : "否");
        printf("  观看人数: %d\n", status.camera_clients);
    }
    printf("校准状态: 重量%s, 电机%s\n",
           ctx->weight_calibrated ? "已校准" : "未校准",
           ctx->motor_calibrated ? "已校准" : "未校准");
    printf("视频URL: %s\n", ctx->video_url);
    printf("定时任务数量: %d\n", ctx->task_count);
    printf("=====================================\n");
}