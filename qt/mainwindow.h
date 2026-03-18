#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>

#include "feedcontroller.h"

class FeedPage;
class TimerPage;
class HistoryPage;
class SettingPage;
class CameraPage;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void onStatusUpdated(feeder_status_t status);
    void onFeedDone(int gram, bool success);
    void onLowFoodWarning();
    void onTempHumUpdated(float temp, float hum);
    void onTempHumAlarm(QString reason);
    void onObstacleTriggered();

private:
    void setupUi();
    void setupStatusBar();
    void setupNavBar();
    void connectSignals();
    void switchPage(int index);
    void showAlert(const QString &msg, int msec = 3000);

    FeedController  *m_ctrl;

    // 中心布局
    QWidget         *m_central;
    QStackedWidget  *m_stack;

    // 页面
    FeedPage        *m_feedPage;
    TimerPage       *m_timerPage;
    HistoryPage     *m_historyPage;
    SettingPage     *m_settingPage;
    CameraPage      *m_cameraPage;

    // 顶部状态栏标签
    QLabel          *m_lblFood;
    QLabel          *m_lblTemp;
    QLabel          *m_lblHum;
    QLabel          *m_lblTime;

    // 导航按钮
    QPushButton     *m_btnFeed;
    QPushButton     *m_btnTimer;
    QPushButton     *m_btnHistory;
    QPushButton     *m_btnSetting;
    QPushButton     *m_btnCamera;

    // 底部提示
    QLabel          *m_lblAlert;
    QTimer          *m_alertTimer;
    QTimer          *m_clockTimer;
};

#endif // MAINWINDOW_H
