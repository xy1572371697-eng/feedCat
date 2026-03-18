#ifndef SETTINGPAGE_H
#define SETTINGPAGE_H

#include <QWidget>
#include <QSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QGroupBox>
#include <QLineEdit>
#include "feedcontroller.h"

class SettingPage : public QWidget
{
    Q_OBJECT
public:
    explicit SettingPage(FeedController *ctrl, QWidget *parent = nullptr);

private slots:
    void onCalibrateZero();
    void onCalibrateWeight();
    void onSaveConfig();
    void onLoadConfig();
    void onResetDefault();
    void onSetTime();

private:
    void setupUi();

    FeedController *m_ctrl;

    // 校准
    QSpinBox    *m_spinCaliGram;
    QLabel      *m_lblCaliStatus;

    // 配置
    QLineEdit   *m_editConfigPath;

    // 系统时间
    QLabel      *m_lblSysTime;
    QTimer      *m_clockTimer;
};

#endif // SETTINGPAGE_H
