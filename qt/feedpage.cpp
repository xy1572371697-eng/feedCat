#include "feedpage.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QMessageBox>

FeedPage::FeedPage(FeedController *ctrl, QWidget *parent)
    : QWidget(parent), m_ctrl(ctrl)
{
    setupUi();
}

void FeedPage::setupUi()
{
    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(12);

    // -------- 左：余粮面板 --------
    auto *leftBox = new QGroupBox("余粮状态", this);
    leftBox->setStyleSheet("QGroupBox { border:1px solid #0f3460; border-radius:8px;"
                           " font-size:14px; color:#e94560; margin-top:6px; }"
                           "QGroupBox::title { subcontrol-origin:margin; left:10px; }");
    auto *leftLayout = new QVBoxLayout(leftBox);
    leftLayout->setSpacing(8);

    m_barFood = new QProgressBar;
    m_barFood->setRange(0, 100);
    m_barFood->setValue(0);
    m_barFood->setTextVisible(true);
    m_barFood->setFixedHeight(28);
    m_barFood->setStyleSheet(
        "QProgressBar { border:1px solid #0f3460; border-radius:6px; background:#1a1a2e; text-align:center; }"
        "QProgressBar::chunk { background:qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 #e94560,stop:1 #0f3460); border-radius:5px; }"
    );

    m_lblFoodGram = new QLabel("余粮: -- g");
    m_lblFoodGram->setStyleSheet("font-size:22px; font-weight:bold; color:#e0e0e0;");
    m_lblFoodGram->setAlignment(Qt::AlignCenter);

    m_lblTodayCount = new QLabel("今日喂食: 0 次");
    m_lblTodayGram  = new QLabel("今日总量: 0 g");
    m_lblIrStatus   = new QLabel("红外: 无遮挡");

    for (auto *l : {m_lblTodayCount, m_lblTodayGram, m_lblIrStatus}) {
        l->setStyleSheet("font-size:13px; color:#a0a0c0;");
        l->setAlignment(Qt::AlignCenter);
    }

    leftLayout->addWidget(m_lblFoodGram);
    leftLayout->addWidget(m_barFood);
    leftLayout->addSpacing(10);
    leftLayout->addWidget(m_lblTodayCount);
    leftLayout->addWidget(m_lblTodayGram);
    leftLayout->addWidget(m_lblIrStatus);
    leftLayout->addStretch();

    // -------- 右：喂食控制 --------
    auto *rightBox = new QGroupBox("手动喂食", this);
    rightBox->setStyleSheet(leftBox->styleSheet());
    auto *rightLayout = new QVBoxLayout(rightBox);
    rightLayout->setSpacing(12);

    auto *lblHint = new QLabel("设定喂食量 (克)");
    lblHint->setStyleSheet("font-size:13px; color:#a0a0c0;");
    lblHint->setAlignment(Qt::AlignCenter);

    m_spinGram = new QSpinBox;
    m_spinGram->setRange(5, 100);
    m_spinGram->setValue(25);
    m_spinGram->setSuffix(" g");
    m_spinGram->setFixedHeight(40);
    m_spinGram->setAlignment(Qt::AlignCenter);
    m_spinGram->setStyleSheet(
        "QSpinBox { background:#1a1a2e; border:1px solid #0f3460; border-radius:6px;"
        " color:#e0e0e0; font-size:16px; padding:2px 4px; }"
        "QSpinBox::up-button, QSpinBox::down-button { width:24px; background:#0f3460; }"
    );

    m_btnFeed = new QPushButton("立即喂食");
    m_btnFeed->setFixedHeight(50);
    m_btnFeed->setStyleSheet(
        "QPushButton { background:#e94560; border-radius:8px; font-size:16px;"
        "  font-weight:bold; color:white; }"
        "QPushButton:hover  { background:#c73652; }"
        "QPushButton:pressed{ background:#a02840; }"
    );

    m_btnStop = new QPushButton("紧急停止");
    m_btnStop->setFixedHeight(42);
    m_btnStop->setStyleSheet(
        "QPushButton { background:#3a3a5c; border-radius:8px; font-size:14px; color:#ff6b6b; border:1px solid #ff6b6b; }"
        "QPushButton:hover  { background:#ff6b6b; color:white; }"
        "QPushButton:pressed{ background:#cc4444; color:white; }"
    );

    // 快捷按钮
    auto *quickLayout = new QHBoxLayout;
    for (int g : {10, 20, 30, 50}) {
        auto *btn = new QPushButton(QString("%1g").arg(g));
        btn->setFixedHeight(36);
        btn->setStyleSheet(
            "QPushButton { background:#16213e; border:1px solid #0f3460; border-radius:6px;"
            "  font-size:13px; color:#a0a0c0; }"
            "QPushButton:hover { background:#0f3460; color:#e0e0e0; }"
        );
        connect(btn, &QPushButton::clicked, this, [this, g]{ m_spinGram->setValue(g); });
        quickLayout->addWidget(btn);
    }

    rightLayout->addWidget(lblHint);
    rightLayout->addWidget(m_spinGram);
    rightLayout->addLayout(quickLayout);
    rightLayout->addSpacing(8);
    rightLayout->addWidget(m_btnFeed);
    rightLayout->addWidget(m_btnStop);
    rightLayout->addStretch();

    root->addWidget(leftBox,  1);
    root->addWidget(rightBox, 1);

    connect(m_btnFeed, &QPushButton::clicked, this, &FeedPage::onFeedClicked);
    connect(m_btnStop, &QPushButton::clicked, this, &FeedPage::onStopClicked);
}

void FeedPage::updateStatus(const feeder_status_t &status)
{
    m_barFood->setValue(static_cast<int>(status.food_percentage));
    m_barFood->setFormat(QString("%1%").arg(static_cast<int>(status.food_percentage)));
    m_lblFoodGram->setText(QString("余粮: %1 g").arg(status.food_gram));
    m_lblTodayCount->setText(QString("今日喂食: %1 次").arg(status.today_feed_count));
    m_lblTodayGram->setText(QString("今日总量: %1 g").arg(status.today_total_gram));
    m_lblIrStatus->setText(status.ir_obstacle ? "红外: 有遮挡 🔴" : "红外: 无遮挡 🟢");

    if (status.low_food_warning)
        m_lblFoodGram->setStyleSheet("font-size:22px; font-weight:bold; color:#ff6b6b;");
    else
        m_lblFoodGram->setStyleSheet("font-size:22px; font-weight:bold; color:#e0e0e0;");
}

void FeedPage::onFeedClicked()
{
    int gram = m_spinGram->value();
    m_btnFeed->setEnabled(false);
    m_btnFeed->setText("喂食中...");
    bool ok = m_ctrl->feedManual(gram);
    m_btnFeed->setEnabled(true);
    m_btnFeed->setText("立即喂食");
    if (!ok)
        QMessageBox::warning(this, "喂食失败", "喂食操作失败，请检查余粮或设备状态。");
}

void FeedPage::onStopClicked()
{
    m_ctrl->emergencyStop();
}
