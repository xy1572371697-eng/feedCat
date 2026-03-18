#include "video_stream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#define MAX_CLIENTS 10
#define STREAM_BOUNDARY "FRAME"
#define STREAM_BOUNDARY_LEN 5
#define JPEG_BUFFER_SIZE (1024 * 1024)  // 1MB JPEG缓冲区

typedef struct stream_client {
    int fd;
    struct sockaddr_in addr;
    int active;
    pthread_t thread;
} stream_client_t;

struct video_stream_server {
    int listen_fd;
    int port;
    int running;
    int client_count;
    int jpeg_quality;
    
    camera_device_t *camera;
    
    // 客户端列表
    stream_client_t clients[MAX_CLIENTS];
    pthread_mutex_t clients_mutex;
    
    // 服务器线程
    pthread_t accept_thread;
    
    // 最新帧缓存
    uint8_t *jpeg_buffer;
    size_t jpeg_size;
    pthread_mutex_t frame_mutex;
    pthread_cond_t frame_cond;
};

// 全局服务器实例（用于信号处理）
static video_stream_server_t *g_server = NULL;

// 信号处理：忽略SIGPIPE（防止客户端断开时崩溃）
static void sigpipe_handler(int sig) {
    // 忽略
}

// 设置非阻塞
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// 发送HTTP头部
static void send_http_headers(int client_fd) {
    const char *headers = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=FRAME\r\n"
        "Cache-Control: no-cache\r\n"
        "Pragma: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n";
    
    send(client_fd, headers, strlen(headers), 0);
}

// 发送一帧JPEG
static void send_jpeg_frame(int client_fd, 
                            const uint8_t *jpeg_data, 
                            size_t jpeg_size) {
    char header[256];
    int header_len = snprintf(header, sizeof(header),
        "--%s\r\n"
        "Content-Type: image/jpeg\r\n"
        "Content-Length: %zu\r\n"
        "\r\n",
        STREAM_BOUNDARY, jpeg_size);
    
    send(client_fd, header, header_len, 0);
    send(client_fd, jpeg_data, jpeg_size, 0);
    send(client_fd, "\r\n", 2, 0);
}

// 摄像头帧回调
static void on_camera_frame(const uint8_t *data, size_t size,
                            int width, int height,
                            camera_pixel_format_t format,
                            void *userdata) {
    video_stream_server_t *server = (video_stream_server_t*)userdata;
    
    if (!server->running) return;
    
    // 转换为JPEG
    if (format == CAMERA_FMT_YUYV) {
        pthread_mutex_lock(&server->frame_mutex);
        
        server->jpeg_size = JPEG_BUFFER_SIZE;
        camera_yuyv_to_jpeg(data, server->jpeg_buffer, 
                           &server->jpeg_size, 
                           width, height, 
                           server->jpeg_quality);
        
        // 通知所有等待的客户端
        pthread_cond_broadcast(&server->frame_cond);
        pthread_mutex_unlock(&server->frame_mutex);
    } else if (format == CAMERA_FMT_MJPEG) {
        // 已经是JPEG，直接使用
        pthread_mutex_lock(&server->frame_mutex);
        
        size_t copy_size = size < JPEG_BUFFER_SIZE ? size : JPEG_BUFFER_SIZE;
        memcpy(server->jpeg_buffer, data, copy_size);
        server->jpeg_size = copy_size;
        
        pthread_cond_broadcast(&server->frame_cond);
        pthread_mutex_unlock(&server->frame_mutex);
    }
}

// 客户端处理线程
static void* client_thread_func(void *arg) {
    stream_client_t *client = (stream_client_t*)arg;
    video_stream_server_t *server = g_server;
    
    printf("新客户端连接: %s:%d (fd=%d)\n",
           inet_ntoa(client->addr.sin_addr),
           ntohs(client->addr.sin_port),
           client->fd);
    
    // 发送HTTP头部
    send_http_headers(client->fd);
    
    // 流式传输帧
    while (client->active && server->running) {
        pthread_mutex_lock(&server->frame_mutex);
        
        // 等待新帧（超时1秒）
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        
        int ret = pthread_cond_timedwait(&server->frame_cond, 
                                         &server->frame_mutex, &ts);
        
        if (ret == 0 && server->jpeg_size > 0) {
            // 有新帧，发送
            send_jpeg_frame(client->fd, 
                           server->jpeg_buffer, 
                           server->jpeg_size);
        }
        
        pthread_mutex_unlock(&server->frame_mutex);
    }
    
    close(client->fd);
    client->active = 0;
    
    printf("客户端断开: %s:%d\n",
           inet_ntoa(client->addr.sin_addr),
           ntohs(client->addr.sin_port));
    
    return NULL;
}

// 接受客户端连接线程
static void* accept_thread_func(void *arg) {
    video_stream_server_t *server = (video_stream_server_t*)arg;
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    while (server->running) {
        int client_fd = accept(server->listen_fd, 
                              (struct sockaddr*)&client_addr, 
                              &client_len);
        
        if (client_fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                usleep(100000);
            }
            continue;
        }
        
        // 设置非阻塞
        set_nonblocking(client_fd);
        
        // 查找空闲客户端槽位
        pthread_mutex_lock(&server->clients_mutex);
        
        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!server->clients[i].active) {
                slot = i;
                break;
            }
        }
        
        if (slot >= 0) {
            // 分配客户端
            server->clients[slot].fd = client_fd;
            server->clients[slot].addr = client_addr;
            server->clients[slot].active = 1;
            server->client_count++;
            
            // 创建客户端线程
            pthread_create(&server->clients[slot].thread, NULL,
                          client_thread_func, &server->clients[slot]);
        } else {
            // 客户端已满，拒绝连接
            const char *msg = "HTTP/1.1 503 Service Unavailable\r\n\r\n";
            send(client_fd, msg, strlen(msg), 0);
            close(client_fd);
        }
        
        pthread_mutex_unlock(&server->clients_mutex);
    }
    
    return NULL;
}

// 启动视频流服务器
video_stream_server_t* video_stream_start(int port, 
                                          camera_device_t *camera,
                                          int jpeg_quality) {
    // 设置SIGPIPE处理
    signal(SIGPIPE, sigpipe_handler);
    
    video_stream_server_t *server = calloc(1, sizeof(video_stream_server_t));
    if (!server) return NULL;
    
    server->port = port;
    server->camera = camera;
    server->jpeg_quality = jpeg_quality;
    server->running = 1;
    server->client_count = 0;
    
    pthread_mutex_init(&server->clients_mutex, NULL);
    pthread_mutex_init(&server->frame_mutex, NULL);
    pthread_cond_init(&server->frame_cond, NULL);
    
    // 分配JPEG缓冲区
    server->jpeg_buffer = malloc(JPEG_BUFFER_SIZE);
    if (!server->jpeg_buffer) {
        free(server);
        return NULL;
    }
    
    // 创建监听socket
    server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_fd < 0) {
        free(server->jpeg_buffer);
        free(server);
        return NULL;
    }
    
    // 允许地址重用
    int opt = 1;
    setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(server->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(server->listen_fd);
        free(server->jpeg_buffer);
        free(server);
        return NULL;
    }
    
    if (listen(server->listen_fd, 10) < 0) {
        close(server->listen_fd);
        free(server->jpeg_buffer);
        free(server);
        return NULL;
    }
    
    set_nonblocking(server->listen_fd);
    
    // 设置全局服务器实例
    g_server = server;
    
    // 启动摄像头流（如果还没启动）
    camera_start_streaming(camera, on_camera_frame, server);
    
    // 启动接受线程
    pthread_create(&server->accept_thread, NULL, 
                   accept_thread_func, server);
    
    // 获取本机IP
    char ip_buf[64] = "0.0.0.0";
    // 可以添加获取实际IP的逻辑
    
    printf("\n========================================\n");
    printf("✅ 视频流服务器已启动\n");
    printf("📡 端口: %d\n", port);
    printf("🌐 访问地址: http://%s:%d/stream\n", ip_buf, port);
    printf("📷 摄像头: %dx%d, JPEG质量: %d%%\n", 
           camera_get_width(camera), 
           camera_get_height(camera),
           jpeg_quality);
    printf("========================================\n\n");
    
    return server;
}

void video_stream_stop(video_stream_server_t *server) {
    if (!server) return;
    
    server->running = 0;
    g_server = NULL;
    
    // 停止接受新连接
    close(server->listen_fd);
    
    // 等待接受线程结束
    pthread_join(server->accept_thread, NULL);
    
    // 断开所有客户端
    pthread_mutex_lock(&server->clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (server->clients[i].active) {
            server->clients[i].active = 0;
            pthread_join(server->clients[i].thread, NULL);
        }
    }
    pthread_mutex_unlock(&server->clients_mutex);
    
    // 停止摄像头流
    camera_stop_streaming(server->camera);
    
    // 释放资源
    free(server->jpeg_buffer);
    pthread_mutex_destroy(&server->clients_mutex);
    pthread_mutex_destroy(&server->frame_mutex);
    pthread_cond_destroy(&server->frame_cond);
    
    free(server);
    
    printf("视频流服务器已停止\n");
}

bool video_stream_set_quality(video_stream_server_t *server, int quality) {
    if (!server) return false;
    
    if (quality < 10) quality = 10;
    if (quality > 100) quality = 100;
    
    server->jpeg_quality = quality;
    printf("视频流JPEG质量设置为: %d%%\n", quality);
    
    return true;
}

int video_stream_get_client_count(video_stream_server_t *server) {
    return server ? server->client_count : 0;
}

void video_stream_get_url(video_stream_server_t *server, 
                          char *buf, size_t buf_size) {
    if (!server || !buf) return;
    
    // 简单返回本地地址
    snprintf(buf, buf_size, "http://%s:%d/stream", "0.0.0.0", server->port);
}