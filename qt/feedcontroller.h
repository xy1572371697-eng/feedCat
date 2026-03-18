#ifndef FEEDCONTROLLER_H
#define FEEDCONTROLLER_H

#include <QObject>
#include <QTimer>
#include <QMutex>
#include <QThread>
#include <QDateTime>

extern "C" {
#include "feed_core.h"
#include "aht30.h"
}

// ==================== 温湿度数据 ====================
struct TempHumData {
    float temp;       // 摄氏度
    float humidity;   // %RH
    QDateTime time;
    bool valid;
};

// ==================== 红外监控线程 ====================
class IrMonitorThread : public QThread
{
    Q_OBJECT
public:
    explicit IrMonitorThread(feeder_context_t *ctx, QObject *parent = nullptr);
    void stop();

signals:
    void obstacleTriggered();   // 30s 内二次遮挡

protected:
    void run() override;

private:
    feeder_context_t *m_ctx;
    volatile bool     m_running;

    // 二次遮挡状态机
    bool   m_firstDetected;     // 第一次遮挡已发生
    qint64 m_firstTimeMs;       // 第一次遮挡时间戳(ms)
    static const int DEBOUNCE_MS  = 50;    // 防抖
    static const int WINDOW_MS    = 30000; // 30秒窗口
};

// ==================== 温湿度采样线程 ====================
class TempHumThread : public QThread
{
    Q_OBJECT
public:
    explicit TempHumThread(aht30_device_t *dev, QObject *parent = nullptr);
    void stop();

signals:
    void dataReady(float temp, float hum);
    void alarmTriggered(QString reason);

protected:
    void run() override;

private:
    aht30_device_t *m_dev;
    volatile bool   m_running;
    static const int SAMPLE_INTERVAL_MS = 300000; // 5分钟
    static const float TEMP_HIGH;   // 30.0
    static const float TEMP_LOW;    // 5.0
    static const float HUM_HIGH;    // 80.0
};

// ==================== 主控制器 ====================
class FeedController : public QObject
{
    Q_OBJECT
public:
    explicit FeedController(QObject *parent = nullptr);
    ~FeedController();

    bool init();
    void deinit();

    // 喂食接口
    bool feedManual(int gram);
    bool feedAuto(int gram);     // 供定时/遮挡触发调用
    void emergencyStop();

    // 定时任务
    int  addTimerTask(const timer_task_t &task);
    bool removeTimerTask(int id);
    bool modifyTimerTask(const timer_task_t &task);
    bool enableTimerTask(int id, bool enable);
    int  getTimerTasks(timer_task_t *buf, int max);

    // 状态查询
    feeder_status_t getStatus();
    int  getFoodGram();
    float getFoodPercent();
    bool isLowFood();
    int  getTodayFeedCount();
    int  getTodayTotalGram();
    bool getFeedHistory(feed_history_item_t *items, int max);
    int  getFeedHistoryCount();

    // 摄像头
    bool cameraInit(int devId = 0);
    bool videoStart(int port = 8080, int quality = 70);
    void videoStop();
    camera_status_t cameraGetStatus();

    // 温湿度
    TempHumData getLastTempHum() const;

    // 校准
    bool calibrateZero();
    bool calibrateWithGram(int gram);

    // 配置
    bool saveConfig(const QString &path);
    bool loadConfig(const QString &path);

    feeder_context_t* ctx() const { return m_ctx; }

signals:
    void feedDone(int gram, bool success);        // 喂食完成
    void statusUpdated(feeder_status_t status);   // 状态刷新
    void lowFoodWarning();                         // 缺粮警告
    void tempHumUpdated(float temp, float hum);   // 温湿度更新
    void tempHumAlarm(QString reason);            // 温湿度异常
    void obstacleTriggered();                      // 红外触发喂食

private slots:
    void onObstacleTriggered();
    void onTempHumReady(float temp, float hum);
    void onTempHumAlarm(QString reason);
    void onStatusTimer();

private:
    feeder_context_t  *m_ctx;
    aht30_device_t    *m_aht30;

    IrMonitorThread   *m_irThread;
    TempHumThread     *m_thThread;
    QTimer            *m_statusTimer;  // 定期刷新状态到UI

    TempHumData        m_lastTempHum;
    QMutex             m_thMutex;

    // 高温减食量系数（温度>30℃时，喂食量乘以此系数）
    static const float HEAT_REDUCTION_FACTOR;  // 0.8

    int adjustFeedGram(int gram);

    static void feedCallback(int gram, int success, void *userdata);
};

#endif // FEEDCONTROLLER_H
