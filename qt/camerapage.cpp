#include "camerapage.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QMutexLocker>
#include <QPainter>
#include <QFileDialog>
#include <QDateTime>
#include <QMessageBox>

CameraPage::CameraPage(FeedController *ctrl, QWidget *parent)
    : QWidget(parent)
    , m_ctrl(ctrl)
    , m_newFrame(false)
{
    setupUi();

    m_previewTimer = new QTimer(this);
    connect(m_previewTimer, &QTimer::timeout, this, &CameraPage::onFrameUpdate);
    m_previewTimer->start(66);  // ~15fps 预览刷新
}

void CameraPage::setupUi()
{
    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(12);

    auto groupStyle = QString(
        "QGroupBox { border:1px solid #0f3460; border-radius:8px;"
        " font-size:13px; color:#e94560; margin-top:8px; }"
        "QGroupBox::title { subcontrol-origin:margin; left:8px; }"
    );
    auto btnStyle = QString(
        "QPushButton { background:#0f3460; border-radius:6px; color:#e0e0e0;"
        "  font-size:13px; padding:6px 12px; }"
        "QPushButton:hover  { background:#e94560; }"
        "QPushButton:pressed{ background:#a02840; }"
        "QPushButton:disabled{ background:#2a2a4a; color:#6a6a8a; }"
    );

    // -------- 左：预览区 --------
    auto *previewBox = new QGroupBox("实时预览 (OV5460)", this);
    previewBox->setStyleSheet(groupStyle);
    auto *previewLayout = new QVBoxLayout(previewBox);

    m_lblPreview = new QLabel;
    m_lblPreview->setMinimumSize(480, 320);
    m_lblPreview->setAlignment(Qt::AlignCenter);
    m_lblPreview->setStyleSheet(
        "background:#0a0a1a; border:1px solid #0f3460; border-radius:4px;"
        " color:#4a4a6a; font-size:14px;"
    );
    m_lblPreview->setText("摄像头未初始化\n请点击右侧 [初始化摄像头]");

    previewLayout->addWidget(m_lblPreview);

    // -------- 右：控制面板 --------
    auto *ctrlBox = new QGroupBox("摄像头控制", this);
    ctrlBox->setStyleSheet(groupStyle);
    ctrlBox->setFixedWidth(190);
    auto *ctrlLayout = new QVBoxLayout(ctrlBox);
    ctrlLayout->setSpacing(8);

    // 分辨率选择
    auto *lblRes = new QLabel("分辨率:");
    lblRes->setStyleSheet("font-size:12px; color:#a0a0c0;");
    m_cmbResolution = new QComboBox;
    m_cmbResolution->addItems({"640x480", "1280x720", "320x240"});
    m_cmbResolution->setStyleSheet(
        "QComboBox { background:#16213e; border:1px solid #0f3460; border-radius:4px;"
        "  color:#e0e0e0; font-size:12px; padding:2px; }"
        "QComboBox::drop-down { background:#0f3460; }"
    );

    // JPEG质量
    auto *lblQ = new QLabel("JPEG质量:");
    lblQ->setStyleSheet("font-size:12px; color:#a0a0c0;");
    m_spinQuality = new QSpinBox;
    m_spinQuality->setRange(20, 100);
    m_spinQuality->setValue(70);
    m_spinQuality->setSuffix("%");
    m_spinQuality->setStyleSheet(
        "QSpinBox { background:#16213e; border:1px solid #0f3460; border-radius:4px;"
        "  color:#e0e0e0; font-size:12px; padding:2px; }"
    );

    // 按钮
    m_btnInit  = new QPushButton("初始化摄像头");
    m_btnStart = new QPushButton("启动视频流");
    m_btnStop  = new QPushButton("停止视频流");
    m_btnPhoto = new QPushButton("拍照保存");
    for (auto *b : {m_btnInit, m_btnStart, m_btnStop, m_btnPhoto}) {
        b->setStyleSheet(btnStyle);
        b->setFixedHeight(36);
    }
    m_btnStart->setEnabled(false);
    m_btnStop->setEnabled(false);
    m_btnPhoto->setEnabled(false);

    // 状态信息
    auto *sepLine = new QFrame;
    sepLine->setFrameShape(QFrame::HLine);
    sepLine->setStyleSheet("color:#0f3460;");

    m_lblStatus  = new QLabel("状态: 未初始化");
    m_lblUrl     = new QLabel("流地址: --");
    m_lblClients = new QLabel("在线: 0");
    for (auto *l : {m_lblStatus, m_lblUrl, m_lblClients}) {
        l->setStyleSheet("font-size:11px; color:#6a6a8a;");
        l->setWordWrap(true);
    }

    ctrlLayout->addWidget(lblRes);
    ctrlLayout->addWidget(m_cmbResolution);
    ctrlLayout->addWidget(lblQ);
    ctrlLayout->addWidget(m_spinQuality);
    ctrlLayout->addSpacing(4);
    ctrlLayout->addWidget(m_btnInit);
    ctrlLayout->addWidget(m_btnStart);
    ctrlLayout->addWidget(m_btnStop);
    ctrlLayout->addWidget(m_btnPhoto);
    ctrlLayout->addWidget(sepLine);
    ctrlLayout->addWidget(m_lblStatus);
    ctrlLayout->addWidget(m_lblUrl);
    ctrlLayout->addWidget(m_lblClients);
    ctrlLayout->addStretch();

    root->addWidget(previewBox, 1);
    root->addWidget(ctrlBox);

    connect(m_btnInit,  &QPushButton::clicked, this, &CameraPage::onInitCamera);
    connect(m_btnStart, &QPushButton::clicked, this, &CameraPage::onStartStream);
    connect(m_btnStop,  &QPushButton::clicked, this, &CameraPage::onStopStream);
    connect(m_btnPhoto, &QPushButton::clicked, this, &CameraPage::onTakePhoto);
}

void CameraPage::onEnter()
{
    updateStatusLabels();
}

void CameraPage::onInitCamera()
{
    bool ok = m_ctrl->cameraInit(0);
    if (!ok) {
        QMessageBox::warning(this, "摄像头", "摄像头初始化失败，请检查 /dev/video0");
        return;
    }

    // 设置分辨率
    QString res = m_cmbResolution->currentText();
    QStringList wh = res.split('x');
    if (wh.size() == 2) {
        feeder_camera_set_resolution(m_ctrl->ctx(),
                                     wh[0].toInt(), wh[1].toInt());
    }

    m_btnStart->setEnabled(true);
    m_btnPhoto->setEnabled(true);
    m_btnInit->setEnabled(false);
    updateStatusLabels();

    // 启动本地预览流（不对外提供HTTP，仅内部回调）
    camera_start_streaming(
        reinterpret_cast<camera_device_t*>(m_ctrl->ctx()),  // 通过 ctx 拿不到，改用直接访问
        nullptr, nullptr
    );
    // 注：实际预览帧通过 video_stream 的 on_camera_frame 获取
    // 这里简化为：直接使用 feeder_video_start 后从 HTTP 流抓帧（见 onStartStream）
}

void CameraPage::onStartStream()
{
    int quality = m_spinQuality->value();
    bool ok = m_ctrl->videoStart(8080, quality);
    if (!ok) {
        QMessageBox::warning(this, "视频流", "视频流服务器启动失败");
        return;
    }
    m_btnStart->setEnabled(false);
    m_btnStop->setEnabled(true);
    updateStatusLabels();
}

void CameraPage::onStopStream()
{
    m_ctrl->videoStop();
    m_btnStart->setEnabled(true);
    m_btnStop->setEnabled(false);
    m_lblPreview->setText("视频流已停止");
    updateStatusLabels();
}

void CameraPage::onTakePhoto()
{
    QString defaultPath = QString("/tmp/photo_%1.jpg")
        .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
    QString path = QFileDialog::getSaveFileName(
        this, "保存照片", defaultPath, "JPEG (*.jpg)");
    if (path.isEmpty()) return;

    bool ok = feeder_camera_take_photo(m_ctrl->ctx(), path.toLocal8Bit().constData());
    QMessageBox::information(this, "拍照", ok
        ? QString("照片已保存:\n%1").arg(path)
        : "拍照失败");
}

void CameraPage::onRefreshStatus()
{
    updateStatusLabels();
}

void CameraPage::updateStatusLabels()
{
    camera_status_t cs = m_ctrl->cameraGetStatus();
    if (!cs.camera_present) {
        m_lblStatus->setText("状态: 未初始化");
        m_lblUrl->setText("流地址: --");
        m_lblClients->setText("在线: 0");
        return;
    }

    m_lblStatus->setText(QString("状态: %1  %2x%3")
                          .arg(cs.streaming ? "推流中" : "已就绪")
                          .arg(cs.width).arg(cs.height));
    const char *url = feeder_video_get_url(m_ctrl->ctx());
    m_lblUrl->setText(QString("流地址:\n%1").arg(url ? url : "--"));
    m_lblClients->setText(QString("在线观看: %1 人").arg(cs.client_count));
}

void CameraPage::onFrameUpdate()
{
    // 定时刷新状态标签（摄像头在线人数等）
    if (m_ctrl->cameraGetStatus().streaming) {
        updateStatusLabels();
    }

    // 本地预览：若视频流已启动，从内存帧缓存中取最新 YUYV 帧转 QImage
    // 实际预览依赖 camera_start_streaming 的回调
    QMutexLocker lk(&m_frameMutex);
    if (!m_newFrame) return;
    m_newFrame = false;
    lk.unlock();

    m_lblPreview->setPixmap(QPixmap::fromImage(
        m_previewImage.scaled(m_lblPreview->size(),
                              Qt::KeepAspectRatio,
                              Qt::SmoothTransformation)));
}

void CameraPage::frameCallback(const uint8_t *data, size_t size,
                               int width, int height,
                               camera_pixel_format_t format, void *userdata)
{
    auto *self = static_cast<CameraPage*>(userdata);
    self->onNewFrame(data, size, width, height, format);
}

void CameraPage::onNewFrame(const uint8_t *data, size_t size,
                            int w, int h, camera_pixel_format_t fmt)
{
    (void)size;
    if (fmt != CAMERA_FMT_YUYV) return;

    // YUYV -> RGB888 for QImage
    QImage img(w, h, QImage::Format_RGB888);
    const uint8_t *src = data;
    uint8_t *dst = img.bits();
    int pixels = w * h / 2;
    for (int i = 0; i < pixels; i++) {
        uint8_t y0 = src[0], u = src[1], y1 = src[2], v = src[3];
        src += 4;
        auto clamp = [](int v) -> uint8_t { return v < 0 ? 0 : (v > 255 ? 255 : v); };
        int r, g, b;
        r = y0 + 1.402f*(v-128); g = y0 - 0.344f*(u-128) - 0.714f*(v-128); b = y0 + 1.772f*(u-128);
        dst[0]=clamp(r); dst[1]=clamp(g); dst[2]=clamp(b); dst+=3;
        r = y1 + 1.402f*(v-128); g = y1 - 0.344f*(u-128) - 0.714f*(v-128); b = y1 + 1.772f*(u-128);
        dst[0]=clamp(r); dst[1]=clamp(g); dst[2]=clamp(b); dst+=3;
    }

    QMutexLocker lk(&m_frameMutex);
    m_previewImage = img;
    m_newFrame = true;
}
