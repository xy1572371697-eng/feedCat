#ifndef CAMERAPAGE_H
#define CAMERAPAGE_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QImage>
#include <QComboBox>
#include "feedcontroller.h"

extern "C" {
#include "camera.h"
}

class CameraPage : public QWidget
{
    Q_OBJECT
public:
    explicit CameraPage(FeedController *ctrl, QWidget *parent = nullptr);
    void onEnter();  // 切换到此页面时调用

private slots:
    void onInitCamera();
    void onStartStream();
    void onStopStream();
    void onTakePhoto();
    void onRefreshStatus();
    void onFrameUpdate();

private:
    void setupUi();
    void updateStatusLabels();

    // 帧回调（静态函数转发到实例）
    static void frameCallback(const uint8_t *data, size_t size,
                              int width, int height,
                              camera_pixel_format_t format, void *userdata);
    void onNewFrame(const uint8_t *data, size_t size, int w, int h,
                    camera_pixel_format_t fmt);

    FeedController *m_ctrl;

    // 预览区
    QLabel      *m_lblPreview;
    QImage       m_previewImage;
    QTimer      *m_previewTimer;

    // 控制
    QPushButton *m_btnInit;
    QPushButton *m_btnStart;
    QPushButton *m_btnStop;
    QPushButton *m_btnPhoto;
    QSpinBox    *m_spinQuality;
    QComboBox   *m_cmbResolution;

    // 状态标签
    QLabel      *m_lblStatus;
    QLabel      *m_lblUrl;
    QLabel      *m_lblClients;

    QMutex      m_frameMutex;
    bool        m_newFrame;
};

#endif // CAMERAPAGE_H
