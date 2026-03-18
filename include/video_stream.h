#ifndef VIDEO_STREAM_H
#define VIDEO_STREAM_H

#include <stdint.h>
#include <stdbool.h>
#include "camera.h"

#ifdef __cplusplus
extern "C" {
#endif

// 视频流服务器句柄
typedef struct video_stream_server video_stream_server_t;

// ============ API接口 ============

// 创建并启动视频流服务器
video_stream_server_t* video_stream_start(int port, 
                                          camera_device_t *camera,
                                          int jpeg_quality);

// 停止并销毁服务器
void video_stream_stop(video_stream_server_t *server);

// 动态切换摄像头参数
bool video_stream_set_resolution(video_stream_server_t *server, 
                                 int width, int height);
bool video_stream_set_quality(video_stream_server_t *server, int quality);

// 获取当前连接数
int video_stream_get_client_count(video_stream_server_t *server);

// 获取服务器URL（用于显示）
void video_stream_get_url(video_stream_server_t *server, 
                          char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif