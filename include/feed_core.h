#ifndef FEED_CORE_H
#define FEED_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

// ==================== 各硬件模块头文件 ====================
#include "weight.h"
#include "motor.h"
#include "ir_sensor.h"
#include "camera.h"
#include "video_stream.h"

// rtc_timer.h 可能不存在或功能不完整，暂时不包含
// #include "rtc_timer.h"

#ifdef __cplusplus
extern "C" {
#endif

// ==================== 常量定义 ====================
#define FEEDER_VERSION         "1.0.0"
#define FEEDER_MAX_FOOD       2000    // 最大容量2000克
#define FEEDER_LOW_THRESHOLD  50      // 缺粮阈值50克
#define FEEDER_MAX_TIMER_TASKS 20     // 最大定时任务数
#define FEEDER_MAX_HISTORY    100     // 最大历史记录数
#define FEEDER_DEFAULT_VIDEO_PORT 8080 // 默认视频端口

// ==================== 喂食记录项（供Qt显示） ====================
typedef struct {
    time_t timestamp;        // 喂食时间戳
    int gram;               // 喂食量（克）
    int type;               // 0=手动, 1=自动, 2=定时
    int success;            // 0=失败, 1=成功
} feed_history_item_t;

// ==================== 定时任务结构（完整定义） ====================
typedef struct {
    int id;                 // 任务ID（自动生成）
    int enabled;            // 是否启用：0=禁用, 1=启用
    int hour;               // 小时 0-23
    int minute;             // 分钟 0-59
    int second;             // 秒 0-59（默认为0）
    int feed_amount;        // 喂食量（克）
    int repeat;             // 重复类型：0=单次, 1=每天, 2=每周
} timer_task_t;

// ==================== 系统状态结构（供Qt实时刷新） ====================
typedef struct {
    // 余粮信息
    int food_gram;          // 当前余粮（克）
    float food_percentage;  // 余粮百分比 0-100
    int low_food_warning;   // 缺粮警告：0=正常, 1=缺粮
    
    // 今日统计
    int today_feed_count;   // 今日喂食次数
    int today_total_gram;   // 今日总喂食量（克）
    
    // 设备状态
    int motor_running;      // 电机状态：0=停止, 1=运行
    int ir_obstacle;        // 红外遮挡：0=无遮挡, 1=有遮挡
    
    // 时间信息
    time_t current_time;    // 当前系统时间
    
    // 摄像头状态
    int camera_present;     // 摄像头是否存在
    int camera_streaming;   // 是否正在推流
    int camera_clients;     // 当前观看人数
    int camera_width;       // 视频宽度
    int camera_height;      // 视频高度
    int camera_fps;         // 视频帧率
} feeder_status_t;

// ==================== 摄像头状态结构 ====================
typedef struct {
    int camera_present;     // 摄像头是否存在
    int streaming;          // 是否正在推流
    int client_count;       // 当前观看人数
    int width;             // 分辨率宽度
    int height;            // 分辨率高度
    int fps;               // 帧率
} camera_status_t;

// ==================== 喂食器上下文（不透明指针） ====================
typedef struct feeder_context feeder_context_t;

// ==================== 回调函数类型定义 ====================

/**
 * 定时任务触发回调
 * @param task 触发的定时任务
 * @param userdata 用户数据
 */
typedef void (*timer_callback_t)(timer_task_t *task, void *userdata);

/**
 * 按键事件回调
 * @param key_id 按键ID (0,1,2...)
 * @param pressed 0=释放, 1=按下
 * @param userdata 用户数据
 */
typedef void (*key_callback_t)(int key_id, int pressed, void *userdata);

/**
 * 喂食完成回调
 * @param gram 实际喂食量
 * @param success 是否成功
 * @param userdata 用户数据
 */
typedef void (*feed_callback_t)(int gram, int success, void *userdata);

/**
 * 状态变化回调
 * @param status 当前状态
 * @param userdata 用户数据
 */
typedef void (*status_callback_t)(feeder_status_t *status, void *userdata);

// ==================== 初始化/清理 ====================

/**
 * @brief 初始化喂食器系统
 * @return 喂食器上下文指针，失败返回NULL
 */
feeder_context_t* feeder_init(void);

/**
 * @brief 反初始化，释放所有资源
 * @param ctx 喂食器上下文
 */
void feeder_deinit(feeder_context_t *ctx);

// ==================== 喂食控制 ====================

/**
 * @brief 手动喂食（按键触发）
 * @param ctx 喂食器上下文
 * @param gram 喂食量（克），范围1-100
 * @return true=成功, false=失败
 */
bool feeder_feed_manual(feeder_context_t *ctx, int gram);

/**
 * @brief 自动喂食（定时器触发）
 * @param ctx 喂食器上下文
 * @param gram 喂食量（克）
 * @return true=成功, false=失败
 */
bool feeder_feed_auto(feeder_context_t *ctx, int gram);

/**
 * @brief 定时喂食（内部使用）
 * @param ctx 喂食器上下文
 * @param gram 喂食量（克）
 * @return true=成功, false=失败
 */
bool feeder_feed_timer(feeder_context_t *ctx, int gram);

/**
 * @brief 紧急停止电机
 * @param ctx 喂食器上下文
 */
void feeder_emergency_stop(feeder_context_t *ctx);

/**
 * @brief 注册喂食完成回调
 * @param ctx 喂食器上下文
 * @param cb 回调函数
 * @param userdata 用户数据
 */
void feeder_register_feed_callback(feeder_context_t *ctx, 
                                   feed_callback_t cb, 
                                   void *userdata);

// ==================== 定时任务管理 ====================

/**
 * @brief 添加定时任务
 * @param ctx 喂食器上下文
 * @param task 任务配置（id字段会被忽略）
 * @return 任务ID（>=0），失败返回-1
 */
int feeder_add_timer_task(feeder_context_t *ctx, timer_task_t *task);

/**
 * @brief 删除定时任务
 * @param ctx 喂食器上下文
 * @param task_id 任务ID
 * @return true=成功, false=失败
 */
bool feeder_remove_timer_task(feeder_context_t *ctx, int task_id);

/**
 * @brief 修改定时任务
 * @param ctx 喂食器上下文
 * @param task 任务配置（必须包含有效的id字段）
 * @return true=成功, false=失败
 */
bool feeder_modify_timer_task(feeder_context_t *ctx, timer_task_t *task);

/**
 * @brief 获取所有定时任务
 * @param ctx 喂食器上下文
 * @param tasks 输出缓冲区
 * @param max 缓冲区大小
 * @return 实际获取的任务数量
 */
int feeder_get_timer_tasks(feeder_context_t *ctx, timer_task_t *tasks, int max);

/**
 * @brief 启用/禁用定时任务
 * @param ctx 喂食器上下文
 * @param task_id 任务ID
 * @param enable true=启用, false=禁用
 * @return true=成功, false=失败
 */
bool feeder_enable_timer_task(feeder_context_t *ctx, int task_id, int enable);

/**
 * @brief 设置定时任务触发回调
 * @param ctx 喂食器上下文
 * @param cb 回调函数
 * @param userdata 用户数据
 */
void feeder_set_timer_callback(feeder_context_t *ctx, 
                               timer_callback_t cb, 
                               void *userdata);

// ==================== 状态查询 ====================

/**
 * @brief 获取完整系统状态
 * @param ctx 喂食器上下文
 * @return 系统状态结构体
 */
feeder_status_t feeder_get_status(feeder_context_t *ctx);

/**
 * @brief 获取当前余粮（克）
 * @param ctx 喂食器上下文
 * @return 余粮克数，失败返回0
 */
int feeder_get_food_gram(feeder_context_t *ctx);

/**
 * @brief 获取余粮百分比
 * @param ctx 喂食器上下文
 * @return 百分比 0-100
 */
float feeder_get_food_percentage(feeder_context_t *ctx);

/**
 * @brief 检查是否缺粮
 * @param ctx 喂食器上下文
 * @return true=缺粮, false=正常
 */
bool feeder_is_low_food(feeder_context_t *ctx);

/**
 * @brief 获取今日喂食次数
 * @param ctx 喂食器上下文
 * @return 今日喂食次数
 */
int feeder_get_today_feed_count(feeder_context_t *ctx);

/**
 * @brief 获取今日总喂食量
 * @param ctx 喂食器上下文
 * @return 今日总喂食量（克）
 */
int feeder_get_today_total_gram(feeder_context_t *ctx);

/**
 * @brief 获取喂食历史记录数量
 * @param ctx 喂食器上下文
 * @return 历史记录数量
 */
int feeder_get_feed_history_count(feeder_context_t *ctx);

/**
 * @brief 获取喂食历史记录
 * @param ctx 喂食器上下文
 * @param items 输出缓冲区
 * @param max_items 缓冲区大小
 * @return true=成功, false=失败
 */
bool feeder_get_feed_history(feeder_context_t *ctx, 
                             feed_history_item_t *items, 
                             int max_items);

/**
 * @brief 清空喂食历史记录
 * @param ctx 喂食器上下文
 * @return true=成功, false=失败
 */
bool feeder_clear_feed_history(feeder_context_t *ctx);

/**
 * @brief 注册状态变化回调
 * @param ctx 喂食器上下文
 * @param cb 回调函数
 * @param userdata 用户数据
 */
void feeder_register_status_callback(feeder_context_t *ctx,
                                     status_callback_t cb,
                                     void *userdata);

// ==================== 校准功能 ====================

/**
 * @brief 重量传感器零点校准（空载校准）
 * @param ctx 喂食器上下文
 * @return true=成功, false=失败
 */
bool feeder_calibrate_weight_zero(feeder_context_t *ctx);

/**
 * @brief 重量传感器砝码校准（使用已知重量）
 * @param ctx 喂食器上下文
 * @param known_gram 已知重量（克）
 * @return true=成功, false=失败
 */
bool feeder_calibrate_weight_with_gram(feeder_context_t *ctx, int known_gram);

/**
 * @brief 电机喂食时间校准（当前驱动不支持，保留接口）
 * @param ctx 喂食器上下文
 * @param test_gram 测试喂食量（克）
 * @param actual_ms 实际运行时间（毫秒）
 * @return true=成功, false=失败
 */
bool feeder_calibrate_motor(feeder_context_t *ctx, int test_gram, int actual_ms);

/**
 * @brief 获取校准状态
 * @param ctx 喂食器上下文
 * @param weight_calibrated 重量是否已校准
 * @param motor_calibrated 电机是否已校准
 * @return true=成功
 */
bool feeder_get_calibration_status(feeder_context_t *ctx,
                                   int *weight_calibrated,
                                   int *motor_calibrated);

// ==================== 摄像头/视频流 ====================

/**
 * @brief 初始化摄像头
 * @param ctx 喂食器上下文
 * @param dev_id 设备ID，通常为0 (/dev/video0)
 * @return true=成功, false=失败
 */
bool feeder_camera_init(feeder_context_t *ctx, int dev_id);

/**
 * @brief 关闭摄像头
 * @param ctx 喂食器上下文
 */
void feeder_camera_close(feeder_context_t *ctx);

/**
 * @brief 启动视频流服务器
 * @param ctx 喂食器上下文
 * @param port HTTP端口（默认8080）
 * @param quality JPEG质量 1-100（默认70）
 * @return true=成功, false=失败
 */
bool feeder_video_start(feeder_context_t *ctx, int port, int quality);

/**
 * @brief 停止视频流服务器
 * @param ctx 喂食器上下文
 */
void feeder_video_stop(feeder_context_t *ctx);

/**
 * @brief 获取摄像头状态
 * @param ctx 喂食器上下文
 * @return 摄像头状态结构体
 */
camera_status_t feeder_camera_get_status(feeder_context_t *ctx);

/**
 * @brief 设置摄像头分辨率
 * @param ctx 喂食器上下文
 * @param width 宽度
 * @param height 高度
 * @return true=成功, false=失败
 */
bool feeder_camera_set_resolution(feeder_context_t *ctx, int width, int height);

/**
 * @brief 设置摄像头帧率（当前驱动不支持）
 * @param ctx 喂食器上下文
 * @param fps 帧率（15/30）
 * @return true=成功, false=失败
 */
bool feeder_camera_set_framerate(feeder_context_t *ctx, int fps);

/**
 * @brief 设置JPEG压缩质量
 * @param ctx 喂食器上下文
 * @param quality 质量 1-100
 * @return true=成功, false=失败
 */
bool feeder_camera_set_quality(feeder_context_t *ctx, int quality);

/**
 * @brief 拍照并保存
 * @param ctx 喂食器上下文
 * @param filename 保存路径（如"/home/photo.jpg"）
 * @return true=成功, false=失败
 */
bool feeder_camera_take_photo(feeder_context_t *ctx, const char *filename);

/**
 * @brief 获取视频流访问URL
 * @param ctx 喂食器上下文
 * @return URL字符串（如"http://0.0.0.0:8080/stream"）
 */
const char* feeder_video_get_url(feeder_context_t *ctx);

/**
 * @brief 获取当前观看人数
 * @param ctx 喂食器上下文
 * @return 客户端数量
 */
int feeder_video_get_client_count(feeder_context_t *ctx);

// ==================== 红外传感器 ====================

/**
 * @brief 检测红外遮挡状态
 * @param ctx 喂食器上下文
 * @return true=有遮挡, false=无遮挡
 */
bool feeder_ir_check(feeder_context_t *ctx);

/**
 * @brief 阻塞等待红外遮挡
 * @param ctx 喂食器上下文
 * @param timeout_ms 超时时间（毫秒），0=无限等待
 * @return true=检测到遮挡, false=超时或失败
 */
bool feeder_ir_wait_obstacle(feeder_context_t *ctx, int timeout_ms);

// ==================== RTC时间管理（简化版）====================

/**
 * @brief 从RTC硬件同步到系统时间（当前不支持）
 * @param ctx 喂食器上下文
 * @return true=成功, false=失败
 */
bool feeder_sync_time_from_rtc(feeder_context_t *ctx);

/**
 * @brief 从系统时间同步到RTC硬件（当前不支持）
 * @param ctx 喂食器上下文
 * @return true=成功, false=失败
 */
bool feeder_sync_time_to_rtc(feeder_context_t *ctx);

/**
 * @brief 获取当前系统时间
 * @param ctx 喂食器上下文
 * @return 时间戳
 */
time_t feeder_get_current_time(feeder_context_t *ctx);

/**
 * @brief 设置系统时间（需要root权限）
 * @param ctx 喂食器上下文
 * @param t 时间戳
 * @return true=成功, false=失败
 */
bool feeder_set_system_time(feeder_context_t *ctx, time_t t);

/**
 * @brief 格式化时间字符串
 * @param t 时间戳
 * @param buf 输出缓冲区
 * @param len 缓冲区大小
 */
void feeder_format_time(time_t t, char *buf, size_t len);

// ==================== 按键回调 ====================

/**
 * @brief 注册按键事件回调
 * @param ctx 喂食器上下文
 * @param cb 回调函数
 * @param userdata 用户数据
 */
void feeder_register_key_callback(feeder_context_t *ctx, 
                                  key_callback_t cb, 
                                  void *userdata);

/**
 * @brief 按键事件处理函数（供驱动调用）
 * @param key_id 按键ID
 * @param pressed 按下状态
 */
void feeder_on_key_event(int key_id, int pressed);

// ==================== 配置管理 ====================

/**
 * @brief 保存配置到文件
 * @param ctx 喂食器上下文
 * @param filename 配置文件路径
 * @return true=成功, false=失败
 */
bool feeder_save_config(feeder_context_t *ctx, const char *filename);

/**
 * @brief 从文件加载配置
 * @param ctx 喂食器上下文
 * @param filename 配置文件路径
 * @return true=成功, false=失败
 */
bool feeder_load_config(feeder_context_t *ctx, const char *filename);

/**
 * @brief 恢复出厂设置
 * @param ctx 喂食器上下文
 * @return true=成功, false=失败
 */
bool feeder_reset_to_default(feeder_context_t *ctx);

// ==================== 系统命令 ====================

/**
 * @brief 执行系统命令（命令行接口）
 * @param ctx 喂食器上下文
 * @param cmd 命令字符串
 * @param output 输出缓冲区
 * @param max_len 缓冲区大小
 * @return true=命令执行成功, false=命令失败
 */
bool feeder_system_command(feeder_context_t *ctx, 
                           const char *cmd, 
                           char *output, 
                           int max_len);

// ==================== 版本信息 ====================

/**
 * @brief 获取版本号
 * @return 版本字符串
 */
const char* feeder_get_version(void);

/**
 * @brief 打印系统信息（调试用）
 * @param ctx 喂食器上下文
 */
void feeder_dump_info(feeder_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif // FEEDER_CORE_H