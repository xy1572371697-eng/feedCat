#include "camera.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <jpeglib.h>  // 需要链接 libjpeg

#define CAMERA_DEVICE_PREFIX "/dev/video0"
#define DEFAULT_BUFFER_COUNT 4

struct camera_device {
    int fd;
    int width;
    int height;
    int framerate;
    camera_pixel_format_t format;
    unsigned int pixelformat;  // V4L2像素格式
    
    // 内存映射缓冲区
    struct {
        void *start;
        size_t length;
    } buffers[DEFAULT_BUFFER_COUNT];
    unsigned int n_buffers;
    
    // 异步捕获线程
    pthread_t stream_thread;
    int streaming;
    frame_callback_t user_callback;
    void *user_data;
    
    // 错误信息
    char error[256];
};

// V4L2像素格式到内部格式的转换
static unsigned int camera_fmt_to_v4l2(camera_pixel_format_t fmt) {
    switch (fmt) {
        case CAMERA_FMT_YUYV:  return V4L2_PIX_FMT_YUYV;
        case CAMERA_FMT_MJPEG: return V4L2_PIX_FMT_MJPEG;
        case CAMERA_FMT_RGB565: return V4L2_PIX_FMT_RGB565;
        default: return V4L2_PIX_FMT_YUYV;
    }
}

static camera_pixel_format_t v4l2_to_camera_fmt(unsigned int v4l2_fmt) {
    switch (v4l2_fmt) {
        case V4L2_PIX_FMT_YUYV:  return CAMERA_FMT_YUYV;
        case V4L2_PIX_FMT_MJPEG: return CAMERA_FMT_MJPEG;
        case V4L2_PIX_FMT_RGB565: return CAMERA_FMT_RGB565;
        default: return CAMERA_FMT_YUYV;
    }
}

// 打开摄像头
camera_device_t* camera_open(int dev_id) {
    camera_device_t *dev = calloc(1, sizeof(camera_device_t));
    if (!dev) return NULL;
    
    char device_path[32];
    snprintf(device_path, sizeof(device_path), "%s%d", 
             CAMERA_DEVICE_PREFIX, dev_id);
    
    dev->fd = open(device_path, O_RDWR);
    if (dev->fd < 0) {
        snprintf(dev->error, sizeof(dev->error),
                 "open %s failed: %s", device_path, strerror(errno));
        free(dev);
        return NULL;
    }
    
    // 查询摄像头能力
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (ioctl(dev->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        snprintf(dev->error, sizeof(dev->error),
                 "not a V4L2 device: %s", strerror(errno));
        close(dev->fd);
        free(dev);
        return NULL;
    }
    
    // 检查是否支持视频捕获
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        snprintf(dev->error, sizeof(dev->error),
                 "device does not support video capture");
        close(dev->fd);
        free(dev);
        return NULL;
    }
    
    printf("摄像头打开成功: %s\n", cap.card);
    printf("驱动: %s\n", cap.driver);
    printf("总线: %s\n", cap.bus_info);
    
    // 默认设置
    dev->width = 640;
    dev->height = 480;
    dev->format = CAMERA_FMT_YUYV;
    dev->pixelformat = V4L2_PIX_FMT_YUYV;
    dev->framerate = 30;
    dev->streaming = 0;
    
    return dev;
}

void camera_close(camera_device_t *dev) {
    if (!dev) return;
    
    camera_stop_streaming(dev);
    
    // 释放缓冲区
    for (unsigned int i = 0; i < dev->n_buffers; i++) {
        if (dev->buffers[i].start) {
            munmap(dev->buffers[i].start, dev->buffers[i].length);
        }
    }
    
    if (dev->fd >= 0) {
        close(dev->fd);
    }
    
    free(dev);
    printf("摄像头已关闭\n");
}

// 查询支持的分辨率
int camera_get_supported_resolutions(camera_device_t *dev, 
                                     camera_config_t *res_list, 
                                     int max_count) {
    if (!dev || !res_list || max_count <= 0) return 0;
    
    struct v4l2_fmtdesc fmt;
    struct v4l2_frmsizeenum frmsize;
    int count = 0;
    
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.index = 0;
    
    while (ioctl(dev->fd, VIDIOC_ENUM_FMT, &fmt) == 0) {
        memset(&frmsize, 0, sizeof(frmsize));
        frmsize.pixel_format = fmt.pixelformat;
        frmsize.index = 0;
        
        while (ioctl(dev->fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0) {
            if (count < max_count) {
                res_list[count].width = frmsize.discrete.width;
                res_list[count].height = frmsize.discrete.height;
                res_list[count].format = v4l2_to_camera_fmt(fmt.pixelformat);
                res_list[count].framerate = 30;
                count++;
            }
            
            frmsize.index++;
        }
        
        fmt.index++;
    }
    
    return count;
}

// 设置像素格式
bool camera_set_format(camera_device_t *dev, camera_pixel_format_t format) {
    if (!dev) return false;
    
    struct v4l2_format v4l2_fmt;
    memset(&v4l2_fmt, 0, sizeof(v4l2_fmt));
    v4l2_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    v4l2_fmt.fmt.pix.width = dev->width;
    v4l2_fmt.fmt.pix.height = dev->height;
    v4l2_fmt.fmt.pix.pixelformat = camera_fmt_to_v4l2(format);
    v4l2_fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;
    
    if (ioctl(dev->fd, VIDIOC_S_FMT, &v4l2_fmt) < 0) {
        snprintf(dev->error, sizeof(dev->error),
                 "set format failed: %s", strerror(errno));
        return false;
    }
    
    dev->format = format;
    dev->pixelformat = v4l2_fmt.fmt.pix.pixelformat;
    dev->width = v4l2_fmt.fmt.pix.width;
    dev->height = v4l2_fmt.fmt.pix.height;
    
    printf("摄像头格式设置: %dx%d, 格式: %c%c%c%c\n",
           dev->width, dev->height,
           (dev->pixelformat >> 0) & 0xFF,
           (dev->pixelformat >> 8) & 0xFF,
           (dev->pixelformat >> 16) & 0xFF,
           (dev->pixelformat >> 24) & 0xFF);
    
    return true;
}

bool camera_set_resolution(camera_device_t *dev, int width, int height) {
    if (!dev) return false;
    dev->width = width;
    dev->height = height;
    return camera_set_format(dev, dev->format);  // 重新应用格式
}

bool camera_set_framerate(camera_device_t *dev, int fps) {
    if (!dev) return false;
    
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    
    if (ioctl(dev->fd, VIDIOC_G_PARM, &parm) == 0) {
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = fps;
        
        if (ioctl(dev->fd, VIDIOC_S_PARM, &parm) == 0) {
            dev->framerate = fps;
            printf("摄像头帧率设置: %d fps\n", fps);
            return true;
        }
    }
    
    snprintf(dev->error, sizeof(dev->error),
             "set framerate failed: %s", strerror(errno));
    return false;
}

bool camera_apply_config(camera_device_t *dev, camera_config_t *config) {
    if (!dev || !config) return false;
    
    if (!camera_set_format(dev, config->format)) return false;
    if (!camera_set_resolution(dev, config->width, config->height)) return false;
    if (!camera_set_framerate(dev, config->framerate)) return false;
    
    return true;
}

// 初始化内存映射缓冲区
static bool camera_init_mmap(camera_device_t *dev) {
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = DEFAULT_BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    
    if (ioctl(dev->fd, VIDIOC_REQBUFS, &req) < 0) {
        snprintf(dev->error, sizeof(dev->error),
                 "request buffers failed: %s", strerror(errno));
        return false;
    }
    
    if (req.count < 2) {
        snprintf(dev->error, sizeof(dev->error),
                 "insufficient buffer memory");
        return false;
    }
    
    dev->n_buffers = req.count;
    
    for (unsigned int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        
        if (ioctl(dev->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            snprintf(dev->error, sizeof(dev->error),
                     "query buffer failed: %s", strerror(errno));
            return false;
        }
        
        dev->buffers[i].length = buf.length;
        dev->buffers[i].start = mmap(NULL, buf.length,
                                      PROT_READ | PROT_WRITE,
                                      MAP_SHARED, dev->fd, buf.m.offset);
        
        if (dev->buffers[i].start == MAP_FAILED) {
            snprintf(dev->error, sizeof(dev->error),
                     "mmap failed: %s", strerror(errno));
            return false;
        }
        
        // 入队缓冲区
        if (ioctl(dev->fd, VIDIOC_QBUF, &buf) < 0) {
            snprintf(dev->error, sizeof(dev->error),
                     "queue buffer failed: %s", strerror(errno));
            return false;
        }
    }
    
    return true;
}

// 开始视频流
static bool camera_start_capture(camera_device_t *dev) {
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    
    if (ioctl(dev->fd, VIDIOC_STREAMON, &type) < 0) {
        snprintf(dev->error, sizeof(dev->error),
                 "stream on failed: %s", strerror(errno));
        return false;
    }
    
    return true;
}

// 同步捕获一帧
bool camera_capture_frame(camera_device_t *dev, 
                          uint8_t *buffer, 
                          size_t buffer_size,
                          size_t *bytes_used) {
    if (!dev || !buffer || buffer_size == 0) return false;
    
    // 如果还没初始化缓冲区，先初始化
    if (dev->n_buffers == 0) {
        if (!camera_init_mmap(dev)) return false;
        if (!camera_start_capture(dev)) return false;
    }
    
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    
    // 出队缓冲区（等待帧）
    if (ioctl(dev->fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno != EAGAIN) {
            snprintf(dev->error, sizeof(dev->error),
                     "dequeue buffer failed: %s", strerror(errno));
        }
        return false;
    }
    
    // 复制数据
    size_t copy_size = buf.bytesused < buffer_size ? buf.bytesused : buffer_size;
    memcpy(buffer, dev->buffers[buf.index].start, copy_size);
    if (bytes_used) *bytes_used = copy_size;
    
    // 重新入队
    if (ioctl(dev->fd, VIDIOC_QBUF, &buf) < 0) {
        snprintf(dev->error, sizeof(dev->error),
                 "queue buffer failed: %s", strerror(errno));
        return false;
    }
    
    return true;
}

// 异步捕获线程
static void* camera_stream_thread(void *arg) {
    camera_device_t *dev = (camera_device_t*)arg;
    
    while (dev->streaming) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        
        // 等待帧（阻塞）
        if (ioctl(dev->fd, VIDIOC_DQBUF, &buf) == 0) {
            if (dev->user_callback) {
                dev->user_callback(dev->buffers[buf.index].start,
                                   buf.bytesused,
                                   dev->width,
                                   dev->height,
                                   dev->format,
                                   dev->user_data);
            }
            
            // 重新入队
            ioctl(dev->fd, VIDIOC_QBUF, &buf);
        }
    }
    
    return NULL;
}

bool camera_start_streaming(camera_device_t *dev, 
                            frame_callback_t callback, 
                            void *userdata) {
    if (!dev || dev->streaming) return false;
    
    // 初始化缓冲区
    if (dev->n_buffers == 0) {
        if (!camera_init_mmap(dev)) return false;
        if (!camera_start_capture(dev)) return false;
    }
    
    dev->user_callback = callback;
    dev->user_data = userdata;
    dev->streaming = 1;
    
    if (pthread_create(&dev->stream_thread, NULL, 
                       camera_stream_thread, dev) != 0) {
        dev->streaming = 0;
        snprintf(dev->error, sizeof(dev->error),
                 "create thread failed: %s", strerror(errno));
        return false;
    }
    
    printf("摄像头流启动成功\n");
    return true;
}

void camera_stop_streaming(camera_device_t *dev) {
    if (!dev || !dev->streaming) return;
    
    dev->streaming = 0;
    pthread_join(dev->stream_thread, NULL);
    
    // 停止流
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(dev->fd, VIDIOC_STREAMOFF, &type);
    
    // 释放缓冲区
    for (unsigned int i = 0; i < dev->n_buffers; i++) {
        if (dev->buffers[i].start) {
            munmap(dev->buffers[i].start, dev->buffers[i].length);
            dev->buffers[i].start = NULL;
        }
    }
    dev->n_buffers = 0;
    
    printf("摄像头流停止\n");
}

// YUYV 转 RGB888
void camera_yuyv_to_rgb888(const uint8_t *yuyv, uint8_t *rgb, 
                           int width, int height) {
    int i;
    for (i = 0; i < width * height / 2; i++) {
        uint8_t y0 = yuyv[0];
        uint8_t u  = yuyv[1];
        uint8_t y1 = yuyv[2];
        uint8_t v  = yuyv[3];
        
        int r, g, b;
        
        // 第一个像素
        r = y0 + 1.402 * (v - 128);
        g = y0 - 0.344 * (u - 128) - 0.714 * (v - 128);
        b = y0 + 1.772 * (u - 128);
        
        rgb[0] = r < 0 ? 0 : (r > 255 ? 255 : r);
        rgb[1] = g < 0 ? 0 : (g > 255 ? 255 : g);
        rgb[2] = b < 0 ? 0 : (b > 255 ? 255 : b);
        
        // 第二个像素
        r = y1 + 1.402 * (v - 128);
        g = y1 - 0.344 * (u - 128) - 0.714 * (v - 128);
        b = y1 + 1.772 * (u - 128);
        
        rgb[3] = r < 0 ? 0 : (r > 255 ? 255 : r);
        rgb[4] = g < 0 ? 0 : (g > 255 ? 255 : g);
        rgb[5] = b < 0 ? 0 : (b > 255 ? 255 : b);
        
        yuyv += 4;
        rgb += 6;
    }
}

// YUYV 转 JPEG
void camera_yuyv_to_jpeg(const uint8_t *yuyv, uint8_t *jpeg, 
                         size_t *jpeg_size, int width, int height, 
                         int quality) {
    // 先转换为RGB
    uint8_t *rgb = malloc(width * height * 3);
    camera_yuyv_to_rgb888(yuyv, rgb, width, height);
    
    // JPEG编码
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    JSAMPROW row_pointer[1];
    int row_stride = width * 3;
    
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    
    // 输出到内存
    unsigned char *jpeg_buffer = NULL;
    unsigned long jpeg_size_ul = 0;
    jpeg_mem_dest(&cinfo, &jpeg_buffer, &jpeg_size_ul);
    
    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    
    while (cinfo.next_scanline < cinfo.image_height) {
        row_pointer[0] = &rgb[cinfo.next_scanline * row_stride];
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }
    
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    
    // 复制到输出缓冲区
    size_t copy_size = jpeg_size_ul < *jpeg_size ? jpeg_size_ul : *jpeg_size;
    memcpy(jpeg, jpeg_buffer, copy_size);
    *jpeg_size = jpeg_size_ul;
    
    free(rgb);
    free(jpeg_buffer);
}

int camera_get_width(camera_device_t *dev) {
    return dev ? dev->width : 0;
}

int camera_get_height(camera_device_t *dev) {
    return dev ? dev->height : 0;
}

camera_pixel_format_t camera_get_format(camera_device_t *dev) {
    return dev ? dev->format : CAMERA_FMT_YUYV;
}

const char* camera_error_string(camera_device_t *dev) {
    return dev ? dev->error : "Unknown error";
}