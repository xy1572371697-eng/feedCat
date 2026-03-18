#include "feedcontroller.h"
#include <QDebug>
#include <QElapsedTimer>

// ==================== 常量定义 ====================
const float TempHumThread::TEMP_HIGH = 30.0f;
const float TempHumThread::TEMP_LOW  = 5.0f;
const float TempHumThread::HUM_HIGH  = 80.0f;
const float FeedController::HEAT_REDUCTION_FACTOR = 0.8f;

// ==================== IrMonitorThread ====================
IrMonitorThread::IrMonitorThread(feeder_context_t *ctx, QObject *parent)
    : QThread(parent)
    , m_ctx(ctx)
    , m_running(false)
    , m_firstDetected(false)
    , m_firstTimeMs(0)
{}

void IrMonitorThread::stop()
{
    m_running = false;
}

void IrMonitorThread::run()
{
    m_running = true;
    m_firstDetected = false;

    QElapsedTimer elapsed;
    elapsed.start();

    while (m_running) {
        // 等待遮挡（每次最多阻塞 100ms，保持可取消）
        bool detected = feeder_ir_wait_obstacle(m_ctx, 100);

        if (!detected) {
            // 超时：检查第一次遮挡是否已超窗口
            if (m_firstDetected) {
                qint64 now = elapsed.elapsed();
                if (now - m_firstTimeMs > WINDOW_MS) {
                    m_firstDetected = false;
                }
            }
            continue;
        }

        // 有遮挡：防抖
        QThread::msleep(DEBOUNCE_MS);
        if (!feeder_ir_check(m_ctx)) continue;  // 防抖后无效

        qint64 now = elapsed.elapsed();

        if (!m_firstDetected) {
            // 第一次遮挡
            m_firstDetected = true;
            m_firstTimeMs   = now;
        } else {
            // 第二次遮挡：判断窗口
            if (now - m_firstTimeMs <= WINDOW_MS) {
                m_firstDetected = false;
                emit obstacleTriggered();
            } else {
                // 超过30s，重新计第一次
                m_firstTimeMs = now;
            }
        }

        // 等待传感器恢复（避免重复触发同一次遮挡）
        QThread::msleep(500);
    }
}

// ==================== TempHumThread ====================
TempHumThread::TempHumThread(aht30_device_t *dev, QObject *parent)
    : QThread(parent)
    , m_dev(dev)
    , m_running(false)
{}

void TempHumThread::stop()
{
    m_running = false;
}

void TempHumThread::run()
{
    m_running = true;

    while (m_running) {
        float temp = 0, hum = 0;
        if (aht30_read(m_dev, &temp, &hum)) {
            emit dataReady(temp, hum);

            // 异常判断
            if (temp > TEMP_HIGH) {
                emit alarmTriggered(QString("高温警告: %.1f°C").arg(temp));
            } else if (temp < TEMP_LOW) {
                emit alarmTriggered(QString("低温警告: %.1f°C").arg(temp));
            }
            if (hum > HUM_HIGH) {
                emit alarmTriggered(QString("高湿警告: %.1f%%").arg(hum));
            }
        }

        // 每5分钟采样一次，分段休眠保持可取消
        for (int i = 0; i < SAMPLE_INTERVAL_MS / 1000 && m_running; i++) {
            QThread::msleep(1000);
        }
    }
}

// ==================== FeedController ====================
FeedController::FeedController(QObject *parent)
    : QObject(parent)
    , m_ctx(nullptr)
    , m_aht30(nullptr)
    , m_irThread(nullptr)
    , m_thThread(nullptr)
    , m_statusTimer(nullptr)
{
    m_lastTempHum = {0, 0, QDateTime(), false};
}

FeedController::~FeedController()
{
    deinit();
}

bool FeedController::init()
{
    // 1. 初始化后端
    m_ctx = feeder_init();
    if (!m_ctx) {
        qWarning() << "[FeedController] feeder_init() 失败";
        return false;
    }

    // 注册喂食完成回调
    feeder_register_feed_callback(m_ctx, feedCallback, this);

    // 2. 打开 AHT30
    m_aht30 = aht30_open();
    if (!m_aht30) {
        qWarning() << "[FeedController] AHT30 打开失败";
    }

    // 3. 启动红外监控线程
    m_irThread = new IrMonitorThread(m_ctx, this);
    connect(m_irThread, &IrMonitorThread::obstacleTriggered,
            this, &FeedController::onObstacleTriggered);
    m_irThread->start();

    // 4. 启动温湿度线程
    if (m_aht30) {
        m_thThread = new TempHumThread(m_aht30, this);
        connect(m_thThread, &TempHumThread::dataReady,
                this, &FeedController::onTempHumReady);
        connect(m_thThread, &TempHumThread::alarmTriggered,
                this, &FeedController::onTempHumAlarm);
        m_thThread->start();
    }

    // 5. 定期刷新状态（2秒一次）
    m_statusTimer = new QTimer(this);
    connect(m_statusTimer, &QTimer::timeout,
            this, &FeedController::onStatusTimer);
    m_statusTimer->start(2000);

    qDebug() << "[FeedController] 初始化完成";
    return true;
}

void FeedController::deinit()
{
    if (m_statusTimer) {
        m_statusTimer->stop();
        m_statusTimer = nullptr;
    }

    if (m_irThread) {
        m_irThread->stop();
        m_irThread->wait(3000);
        m_irThread = nullptr;
    }

    if (m_thThread) {
        m_thThread->stop();
        m_thThread->wait(3000);
        m_thThread = nullptr;
    }

    if (m_aht30) {
        aht30_close(m_aht30);
        m_aht30 = nullptr;
    }

    if (m_ctx) {
        feeder_deinit(m_ctx);
        m_ctx = nullptr;
    }
}

// ---------- 喂食 ----------
int FeedController::adjustFeedGram(int gram)
{
    QMutexLocker lk(&m_thMutex);
    if (m_lastTempHum.valid && m_lastTempHum.temp > 30.0f) {
        gram = static_cast<int>(gram * HEAT_REDUCTION_FACTOR);
        if (gram < 5) gram = 5;
        qDebug() << "[FeedController] 高温减食量 -> " << gram << "克";
    }
    return gram;
}

bool FeedController::feedManual(int gram)
{
    if (!m_ctx) return false;
    gram = adjustFeedGram(gram);
    return feeder_feed_manual(m_ctx, gram);
}

bool FeedController::feedAuto(int gram)
{
    if (!m_ctx) return false;
    gram = adjustFeedGram(gram);
    return feeder_feed_auto(m_ctx, gram);
}

void FeedController::emergencyStop()
{
    if (m_ctx) feeder_emergency_stop(m_ctx);
}

// ---------- 定时任务 ----------
int FeedController::addTimerTask(const timer_task_t &task)
{
    if (!m_ctx) return -1;
    timer_task_t t = task;
    return feeder_add_timer_task(m_ctx, &t);
}

bool FeedController::removeTimerTask(int id)
{
    if (!m_ctx) return false;
    return feeder_remove_timer_task(m_ctx, id);
}

bool FeedController::modifyTimerTask(const timer_task_t &task)
{
    if (!m_ctx) return false;
    timer_task_t t = task;
    return feeder_modify_timer_task(m_ctx, &t);
}

bool FeedController::enableTimerTask(int id, bool enable)
{
    if (!m_ctx) return false;
    return feeder_enable_timer_task(m_ctx, id, enable ? 1 : 0);
}

int FeedController::getTimerTasks(timer_task_t *buf, int max)
{
    if (!m_ctx) return 0;
    return feeder_get_timer_tasks(m_ctx, buf, max);
}

// ---------- 状态查询 ----------
feeder_status_t FeedController::getStatus()     { return m_ctx ? feeder_get_status(m_ctx)           : feeder_status_t{}; }
int  FeedController::getFoodGram()               { return m_ctx ? feeder_get_food_gram(m_ctx)         : 0; }
float FeedController::getFoodPercent()           { return m_ctx ? feeder_get_food_percentage(m_ctx)   : 0; }
bool FeedController::isLowFood()                 { return m_ctx ? feeder_is_low_food(m_ctx)           : false; }
int  FeedController::getTodayFeedCount()         { return m_ctx ? feeder_get_today_feed_count(m_ctx)  : 0; }
int  FeedController::getTodayTotalGram()         { return m_ctx ? feeder_get_today_total_gram(m_ctx)  : 0; }
int  FeedController::getFeedHistoryCount()       { return m_ctx ? feeder_get_feed_history_count(m_ctx): 0; }

bool FeedController::getFeedHistory(feed_history_item_t *items, int max)
{
    if (!m_ctx) return false;
    return feeder_get_feed_history(m_ctx, items, max);
}

// ---------- 摄像头 ----------
bool FeedController::cameraInit(int devId)
{
    if (!m_ctx) return false;
    return feeder_camera_init(m_ctx, devId);
}

bool FeedController::videoStart(int port, int quality)
{
    if (!m_ctx) return false;
    return feeder_video_start(m_ctx, port, quality);
}

void FeedController::videoStop()
{
    if (m_ctx) feeder_video_stop(m_ctx);
}

camera_status_t FeedController::cameraGetStatus()
{
    return m_ctx ? feeder_camera_get_status(m_ctx) : camera_status_t{};
}

// ---------- 温湿度 ----------
TempHumData FeedController::getLastTempHum() const
{
    return m_lastTempHum;
}

// ---------- 校准 ----------
bool FeedController::calibrateZero()             { return m_ctx ? feeder_calibrate_weight_zero(m_ctx)             : false; }
bool FeedController::calibrateWithGram(int gram) { return m_ctx ? feeder_calibrate_weight_with_gram(m_ctx, gram) : false; }

// ---------- 配置 ----------
bool FeedController::saveConfig(const QString &path)
{
    if (!m_ctx) return false;
    return feeder_save_config(m_ctx, path.toLocal8Bit().constData());
}

bool FeedController::loadConfig(const QString &path)
{
    if (!m_ctx) return false;
    return feeder_load_config(m_ctx, path.toLocal8Bit().constData());
}

// ==================== 私有槽 ====================
void FeedController::onObstacleTriggered()
{
    qDebug() << "[FeedController] 红外二次遮挡触发喂食";
    emit obstacleTriggered();
    // 默认喂食量25克（目标20-30g中间值）
    feedAuto(25);
}

void FeedController::onTempHumReady(float temp, float hum)
{
    QMutexLocker lk(&m_thMutex);
    m_lastTempHum.temp     = temp;
    m_lastTempHum.humidity = hum;
    m_lastTempHum.time     = QDateTime::currentDateTime();
    m_lastTempHum.valid    = true;
    lk.unlock();

    emit tempHumUpdated(temp, hum);
}

void FeedController::onTempHumAlarm(QString reason)
{
    qWarning() << "[FeedController] 温湿度异常:" << reason;
    emit tempHumAlarm(reason);
}

void FeedController::onStatusTimer()
{
    if (!m_ctx) return;
    feeder_status_t s = feeder_get_status(m_ctx);
    emit statusUpdated(s);
    if (s.low_food_warning) {
        emit lowFoodWarning();
    }
}

// ==================== C 回调桥接 ====================
void FeedController::feedCallback(int gram, int success, void *userdata)
{
    FeedController *self = static_cast<FeedController*>(userdata);
    // 通过 QMetaObject 投递到主线程
    QMetaObject::invokeMethod(self, [self, gram, success]() {
        emit self->feedDone(gram, success != 0);
    }, Qt::QueuedConnection);
}
