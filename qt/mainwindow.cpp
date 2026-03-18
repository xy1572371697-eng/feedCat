#include "mainwindow.h"
#include "feedpage.h"
#include "timerpage.h"
#include "historypage.h"
#include "settingpage.h"
#include "camerapage.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QDateTime>
#include <QDebug>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_ctrl(new FeedController(this))
    , m_alertTimer(new QTimer(this))
    , m_clockTimer(new QTimer(this))
{
    setWindowTitle("智能喂猫器");
    // 正点原子 800x480 分辨率
    resize(800, 480);
    setStyleSheet("background-color:#1a1a2e; color:#e0e0e0; font-family:'Noto Sans CJK SC',sans-serif;");

    setupUi();
    connectSignals();

    if (!m_ctrl->init()) {
        showAlert("系统初始化失败，部分功能不可用", 5000);
    }

    // 时钟刷新
    m_clockTimer->start(1000);
}

MainWindow::~MainWindow()
{
    m_ctrl->deinit();
}

void MainWindow::setupUi()
{
    m_central = new QWidget(this);
    setCentralWidget(m_central);

    auto *rootLayout = new QVBoxLayout(m_central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // -------- 顶部状态栏 --------
    setupStatusBar();
    auto *statusBar = m_central->findChild<QFrame*>("statusBar");
    rootLayout->addWidget(statusBar);

    // -------- 内容区（导航+页面） --------
    auto *contentLayout = new QHBoxLayout;
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);

    setupNavBar();
    auto *navBar = m_central->findChild<QFrame*>("navBar");
    contentLayout->addWidget(navBar);

    // 页面栈
    m_stack = new QStackedWidget;
    m_feedPage    = new FeedPage(m_ctrl, m_stack);
    m_timerPage   = new TimerPage(m_ctrl, m_stack);
    m_historyPage = new HistoryPage(m_ctrl, m_stack);
    m_settingPage = new SettingPage(m_ctrl, m_stack);
    m_cameraPage  = new CameraPage(m_ctrl, m_stack);
    m_stack->addWidget(m_feedPage);     // 0
    m_stack->addWidget(m_timerPage);    // 1
    m_stack->addWidget(m_historyPage);  // 2
    m_stack->addWidget(m_settingPage);  // 3
    m_stack->addWidget(m_cameraPage);   // 4
    m_stack->setCurrentIndex(0);

    contentLayout->addWidget(m_stack, 1);
    rootLayout->addLayout(contentLayout, 1);

    // -------- 底部提示栏 --------
    m_lblAlert = new QLabel;
    m_lblAlert->setAlignment(Qt::AlignCenter);
    m_lblAlert->setStyleSheet("background:#16213e; color:#ff6b6b; font-size:14px; padding:4px;");
    m_lblAlert->setFixedHeight(28);
    m_lblAlert->hide();
    rootLayout->addWidget(m_lblAlert);
}

void MainWindow::setupStatusBar()
{
    auto *bar = new QFrame(m_central);
    bar->setObjectName("statusBar");
    bar->setFixedHeight(48);
    bar->setStyleSheet("background:#16213e; border-bottom:1px solid #0f3460;");

    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(10, 4, 10, 4);
    layout->setSpacing(20);

    auto makeLabel = [&](const QString &text) {
        auto *lbl = new QLabel(text, bar);
        lbl->setStyleSheet("font-size:14px; color:#a0a0c0;");
        return lbl;
    };

    m_lblFood = makeLabel("余粮: --g");
    m_lblTemp = makeLabel("温度: --°C");
    m_lblHum  = makeLabel("湿度: --%");
    m_lblTime = makeLabel(QDateTime::currentDateTime().toString("MM-dd HH:mm:ss"));

    auto *title = new QLabel("🐱 智能喂猫器", bar);
    title->setStyleSheet("font-size:18px; font-weight:bold; color:#e94560;");

    layout->addWidget(title);
    layout->addStretch();
    layout->addWidget(m_lblFood);
    layout->addWidget(m_lblTemp);
    layout->addWidget(m_lblHum);
    layout->addWidget(m_lblTime);
}

void MainWindow::setupNavBar()
{
    auto *nav = new QFrame(m_central);
    nav->setObjectName("navBar");
    nav->setFixedWidth(110);
    nav->setStyleSheet("background:#16213e; border-right:1px solid #0f3460;");

    auto *layout = new QVBoxLayout(nav);
    layout->setContentsMargins(4, 10, 4, 10);
    layout->setSpacing(6);

    auto makeBtn = [&](const QString &icon, const QString &text) {
        auto *btn = new QPushButton(icon + "\n" + text, nav);
        btn->setFixedHeight(70);
        btn->setCheckable(true);
        btn->setStyleSheet(
            "QPushButton { background:#1a1a2e; border-radius:8px; font-size:12px;"
            "  color:#a0a0c0; border:1px solid transparent; }"
            "QPushButton:checked { background:#0f3460; color:#e94560;"
            "  border:1px solid #e94560; }"
            "QPushButton:hover { background:#0f3460; }"
        );
        return btn;
    };

    m_btnFeed    = makeBtn("🍽", "喂食");
    m_btnTimer   = makeBtn("⏰", "定时");
    m_btnHistory = makeBtn("📋", "记录");
    m_btnSetting = makeBtn("⚙", "设置");
    m_btnCamera  = makeBtn("📷", "摄像头");

    m_btnFeed->setChecked(true);

    layout->addWidget(m_btnFeed);
    layout->addWidget(m_btnTimer);
    layout->addWidget(m_btnHistory);
    layout->addWidget(m_btnSetting);
    layout->addWidget(m_btnCamera);
    layout->addStretch();
}

void MainWindow::connectSignals()
{
    connect(m_ctrl, &FeedController::statusUpdated,   this, &MainWindow::onStatusUpdated);
    connect(m_ctrl, &FeedController::feedDone,        this, &MainWindow::onFeedDone);
    connect(m_ctrl, &FeedController::lowFoodWarning,  this, &MainWindow::onLowFoodWarning);
    connect(m_ctrl, &FeedController::tempHumUpdated,  this, &MainWindow::onTempHumUpdated);
    connect(m_ctrl, &FeedController::tempHumAlarm,    this, &MainWindow::onTempHumAlarm);
    connect(m_ctrl, &FeedController::obstacleTriggered, this, &MainWindow::onObstacleTriggered);

    // 导航按钮
    connect(m_btnFeed,    &QPushButton::clicked, this, [this]{ switchPage(0); });
    connect(m_btnTimer,   &QPushButton::clicked, this, [this]{ switchPage(1); });
    connect(m_btnHistory, &QPushButton::clicked, this, [this]{ switchPage(2); });
    connect(m_btnSetting, &QPushButton::clicked, this, [this]{ switchPage(3); });
    connect(m_btnCamera,  &QPushButton::clicked, this, [this]{ switchPage(4); });

    // 提示自动消失
    m_alertTimer->setSingleShot(true);
    connect(m_alertTimer, &QTimer::timeout, m_lblAlert, &QLabel::hide);

    // 时钟
    connect(m_clockTimer, &QTimer::timeout, this, [this]{
        m_lblTime->setText(QDateTime::currentDateTime().toString("MM-dd HH:mm:ss"));
    });
}

void MainWindow::switchPage(int index)
{
    m_stack->setCurrentIndex(index);
    m_btnFeed->setChecked(index == 0);
    m_btnTimer->setChecked(index == 1);
    m_btnHistory->setChecked(index == 2);
    m_btnSetting->setChecked(index == 3);
    m_btnCamera->setChecked(index == 4);

    if (index == 2) m_historyPage->refresh();
    if (index == 4) m_cameraPage->onEnter();
}

void MainWindow::onStatusUpdated(feeder_status_t status)
{
    m_lblFood->setText(QString("余粮: %1g (%2%)").arg(status.food_gram)
                        .arg(static_cast<int>(status.food_percentage)));
    m_feedPage->updateStatus(status);
}

void MainWindow::onFeedDone(int gram, bool success)
{
    if (success)
        showAlert(QString("喂食完成: %1克").arg(gram), 2000);
    else
        showAlert("喂食失败！", 3000);
}

void MainWindow::onLowFoodWarning()
{
    showAlert("⚠ 余粮不足，请及时添加！", 5000);
    m_lblFood->setStyleSheet("font-size:14px; color:#ff6b6b; font-weight:bold;");
}

void MainWindow::onTempHumUpdated(float temp, float hum)
{
    m_lblTemp->setText(QString("温度: %1°C").arg(temp, 0, 'f', 1));
    m_lblHum->setText(QString("湿度: %1%").arg(hum, 0, 'f', 1));
    // 恢复正常颜色
    m_lblTemp->setStyleSheet("font-size:14px; color:#a0a0c0;");
    m_lblHum->setStyleSheet("font-size:14px; color:#a0a0c0;");
}

void MainWindow::onTempHumAlarm(QString reason)
{
    showAlert("⚠ " + reason, 5000);
    if (reason.contains("温"))
        m_lblTemp->setStyleSheet("font-size:14px; color:#ff6b6b; font-weight:bold;");
    if (reason.contains("湿"))
        m_lblHum->setStyleSheet("font-size:14px; color:#ff6b6b; font-weight:bold;");
}

void MainWindow::onObstacleTriggered()
{
    showAlert("检测到猫咪，自动喂食中...", 3000);
    switchPage(0);
}

void MainWindow::showAlert(const QString &msg, int msec)
{
    m_lblAlert->setText(msg);
    m_lblAlert->show();
    m_alertTimer->start(msec);
}
