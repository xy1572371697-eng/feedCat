#include "settingpage.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QMessageBox>
#include <QFileDialog>
#include <QTimer>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDialog>
#include <QDialogButtonBox>

SettingPage::SettingPage(FeedController *ctrl, QWidget *parent)
    : QWidget(parent), m_ctrl(ctrl)
{
    setupUi();
}

void SettingPage::setupUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 12, 16, 12);
    root->setSpacing(10);

    auto groupStyle = QString(
        "QGroupBox { border:1px solid #0f3460; border-radius:8px;"
        " font-size:14px; color:#e94560; margin-top:8px; padding-top:4px; }"
        "QGroupBox::title { subcontrol-origin:margin; left:10px; }"
    );
    auto btnStyle = QString(
        "QPushButton { background:#0f3460; border-radius:6px; color:#e0e0e0;"
        "  font-size:13px; padding:6px 14px; }"
        "QPushButton:hover { background:#e94560; }"
        "QPushButton:pressed { background:#a02840; }"
    );

    // -------- 重量校准 --------
    auto *caliBox = new QGroupBox("重量传感器校准", this);
    caliBox->setStyleSheet(groupStyle);
    auto *caliLayout = new QHBoxLayout(caliBox);
    caliLayout->setSpacing(8);

    auto *btnZero = new QPushButton("零点校准（空载）");
    btnZero->setStyleSheet(btnStyle);

    m_spinCaliGram = new QSpinBox;
    m_spinCaliGram->setRange(10, 1000);
    m_spinCaliGram->setValue(100);
    m_spinCaliGram->setSuffix(" g");
    m_spinCaliGram->setStyleSheet(
        "QSpinBox { background:#16213e; border:1px solid #0f3460; border-radius:4px;"
        "  color:#e0e0e0; font-size:14px; padding:2px; }");

    auto *btnWeight = new QPushButton("砝码校准");
    btnWeight->setStyleSheet(btnStyle);

    m_lblCaliStatus = new QLabel("校准状态: 未校准");
    m_lblCaliStatus->setStyleSheet("font-size:13px; color:#a0a0c0;");

    caliLayout->addWidget(btnZero);
    caliLayout->addWidget(m_spinCaliGram);
    caliLayout->addWidget(btnWeight);
    caliLayout->addStretch();
    caliLayout->addWidget(m_lblCaliStatus);

    // -------- 配置文件 --------
    auto *confBox = new QGroupBox("配置管理", this);
    confBox->setStyleSheet(groupStyle);
    auto *confLayout = new QHBoxLayout(confBox);
    confLayout->setSpacing(8);

    auto *lblPath = new QLabel("配置文件路径:");
    lblPath->setStyleSheet("font-size:13px; color:#a0a0c0;");

    m_editConfigPath = new QLineEdit("/etc/feedcat.conf");
    m_editConfigPath->setStyleSheet(
        "QLineEdit { background:#16213e; border:1px solid #0f3460; border-radius:4px;"
        "  color:#e0e0e0; font-size:13px; padding:4px; }");

    auto *btnBrowse = new QPushButton("浏览");
    auto *btnSave   = new QPushButton("保存配置");
    auto *btnLoad   = new QPushButton("加载配置");
    auto *btnReset  = new QPushButton("恢复默认");
    for (auto *b : {btnBrowse, btnSave, btnLoad, btnReset})
        b->setStyleSheet(btnStyle);
    btnReset->setStyleSheet(
        "QPushButton { background:#6a2040; border-radius:6px; color:#e0e0e0;"
        "  font-size:13px; padding:6px 14px; }"
        "QPushButton:hover { background:#e94560; }"
    );

    connect(btnBrowse, &QPushButton::clicked, this, [this]{
        QString f = QFileDialog::getSaveFileName(this, "选择配置文件",
                    m_editConfigPath->text(), "Config (*.conf *.ini *.txt)");
        if (!f.isEmpty()) m_editConfigPath->setText(f);
    });

    confLayout->addWidget(lblPath);
    confLayout->addWidget(m_editConfigPath, 1);
    confLayout->addWidget(btnBrowse);
    confLayout->addWidget(btnSave);
    confLayout->addWidget(btnLoad);
    confLayout->addWidget(btnReset);

    // -------- 系统时间 --------
    auto *timeBox = new QGroupBox("系统时间", this);
    timeBox->setStyleSheet(groupStyle);
    auto *timeLayout = new QHBoxLayout(timeBox);

    m_lblSysTime = new QLabel;
    m_lblSysTime->setStyleSheet("font-size:18px; color:#e0e0e0; font-weight:bold;");

    auto *btnSetTime = new QPushButton("设置时间");
    btnSetTime->setStyleSheet(btnStyle);

    timeLayout->addWidget(m_lblSysTime, 1);
    timeLayout->addWidget(btnSetTime);

    // 时钟刷新
    m_clockTimer = new QTimer(this);
    connect(m_clockTimer, &QTimer::timeout, this, [this]{
        m_lblSysTime->setText(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
    });
    m_clockTimer->start(1000);
    m_lblSysTime->setText(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));

    // -------- 关于 --------
    auto *aboutBox = new QGroupBox("关于", this);
    aboutBox->setStyleSheet(groupStyle);
    auto *aboutLayout = new QHBoxLayout(aboutBox);
    auto *lblAbout = new QLabel(
        "智能喂猫器 v1.0.0  |  正点原子 i.MX6UL  |  OV5460 摄像头  |  AHT30 温湿度");
    lblAbout->setStyleSheet("font-size:12px; color:#6a6a8a;");
    aboutLayout->addWidget(lblAbout);

    root->addWidget(caliBox);
    root->addWidget(confBox);
    root->addWidget(timeBox);
    root->addWidget(aboutBox);
    root->addStretch();

    // 连接信号
    connect(btnZero,    &QPushButton::clicked, this, &SettingPage::onCalibrateZero);
    connect(btnWeight,  &QPushButton::clicked, this, &SettingPage::onCalibrateWeight);
    connect(btnSave,    &QPushButton::clicked, this, &SettingPage::onSaveConfig);
    connect(btnLoad,    &QPushButton::clicked, this, &SettingPage::onLoadConfig);
    connect(btnReset,   &QPushButton::clicked, this, &SettingPage::onResetDefault);
    connect(btnSetTime,  &QPushButton::clicked, this, &SettingPage::onSetTime);
}

void SettingPage::onCalibrateZero()
{
    bool ok = m_ctrl->calibrateZero();
    m_lblCaliStatus->setText(ok ? "校准状态: 零点已校准 ✅" : "校准状态: 零点校准失败 ❌");
    m_lblCaliStatus->setStyleSheet(ok ? "font-size:13px;color:#50c878;" : "font-size:13px;color:#ff6b6b;");
}

void SettingPage::onCalibrateWeight()
{
    int gram = m_spinCaliGram->value();
    bool ok = m_ctrl->calibrateWithGram(gram);
    m_lblCaliStatus->setText(ok ? QString("校准状态: 已使用 %1g 砝码校准 ✅").arg(gram)
                                : "校准状态: 砝码校准失败 ❌");
    m_lblCaliStatus->setStyleSheet(ok ? "font-size:13px;color:#50c878;" : "font-size:13px;color:#ff6b6b;");
}

void SettingPage::onSaveConfig()
{
    bool ok = m_ctrl->saveConfig(m_editConfigPath->text());
    QMessageBox::information(this, "配置", ok ? "配置已保存" : "保存失败");
}

void SettingPage::onLoadConfig()
{
    bool ok = m_ctrl->loadConfig(m_editConfigPath->text());
    QMessageBox::information(this, "配置", ok ? "配置已加载" : "加载失败");
}

void SettingPage::onResetDefault()
{
    if (QMessageBox::question(this, "确认", "确定恢复出厂设置？") != QMessageBox::Yes) return;
    feeder_reset_to_default(m_ctrl->ctx());
    m_lblCaliStatus->setText("校准状态: 未校准");
    m_lblCaliStatus->setStyleSheet("font-size:13px; color:#a0a0c0;");
    QMessageBox::information(this, "完成", "已恢复出厂设置");
}

void SettingPage::onSetTime()
{
    QDialog dlg(this);
    dlg.setWindowTitle("设置系统时间");
    dlg.setStyleSheet("background:#1a1a2e; color:#e0e0e0;");
    auto *layout = new QVBoxLayout(&dlg);

    auto *dte = new QDateTimeEdit(QDateTime::currentDateTime(), &dlg);
    dte->setDisplayFormat("yyyy-MM-dd HH:mm:ss");
    dte->setCalendarPopup(true);
    dte->setStyleSheet(
        "QDateTimeEdit { background:#16213e; border:1px solid #0f3460;"
        "  border-radius:4px; color:#e0e0e0; font-size:15px; padding:4px; }");

    auto *btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    btns->setStyleSheet("QPushButton{background:#0f3460;border-radius:5px;padding:4px 12px;color:#e0e0e0;}"
                        "QPushButton:hover{background:#e94560;}");
    connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    layout->addWidget(dte);
    layout->addWidget(btns);

    if (dlg.exec() == QDialog::Accepted) {
        time_t t = dte->dateTime().toSecsSinceEpoch();
        bool ok = feeder_set_system_time(m_ctrl->ctx(), t);
        QMessageBox::information(this, "设置时间", ok ? "系统时间已更新" : "设置失败（需要root权限）");
    }
}
