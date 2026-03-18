#ifndef CAMERA_H
#define CAMERA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 摄像头设备句柄
typedef struct camera_device camera_device_t;

// 视频帧格式
typedef enum {
    CAMERA_FMT_YUYV = 0,     // YUYV 4:2:2
    CAMERA_FMT_MJPEG,        // MJPEG 压缩格式
    CAMERA_FMT_RGB565,       // RGB565
} camera_pixel_format_t;

// 分辨率配置
typedef struct {
    int width;
    int height;
    camera_pixel_format_t format;
    int framerate;
} camera_config_t;

// 帧数据回调函数类型
typedef void (*frame_callback_t)(const uint8_t *data, size_t size, 
                                 int width, int height, 
                                 camera_pixel_format_t format,
                                 void *userdata);

// ============ API接口 ============

// 打开/关闭摄像头
camera_device_t* camera_open(int dev_id);  // dev_id: /dev/video0 = 0
void camera_close(camera_device_t *dev);

// 查询支持的分辨率
int camera_get_supported_resolutions(camera_device_t *dev, 
                                     camera_config_t *res_list, 
                                     int max_count);

// 设置参数
bool camera_set_resolution(camera_device_t *dev, int width, int height);
bool camera_set_format(camera_device_t *dev, camera_pixel_format_t format);
bool camera_set_framerate(camera_device_t *dev, int fps);
bool camera_apply_config(camera_device_t *dev, camera_config_t *config);

// 获取当前参数
int camera_get_width(camera_device_t *dev);
int camera_get_height(camera_device_t *dev);
camera_pixel_format_t camera_get_format(camera_device_t *dev);
int camera_get_framerate(camera_device_t *dev);

// 同步捕获一帧（阻塞）
bool camera_capture_frame(camera_device_t *dev, 
                          uint8_t *buffer, 
                          size_t buffer_size,
                          size_t *bytes_used);

// 异步捕获（回调方式）
bool camera_start_streaming(camera_device_t *dev, 
                            frame_callback_t callback, 
                            void *userdata);
void camera_stop_streaming(camera_device_t *dev);

// 帧格式转换（纯软件，不依赖硬件）
void camera_yuyv_to_rgb888(const uint8_t *yuyv, uint8_t *rgb, 
                           int width, int height);
void camera_yuyv_to_jpeg(const uint8_t *yuyv, uint8_t *jpeg, 
                         size_t *jpeg_size, int width, int height, 
                         int quality);
void camera_yuyv_to_gray(const uint8_t *yuyv, uint8_t *gray,
                         int width, int height);

// 错误信息
const char* camera_error_string(camera_device_t *dev);

#ifdef __cplusplus
}
#endif

#endif