#ifndef FEEDPAGE_H
#define FEEDPAGE_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QProgressBar>
#include "feedcontroller.h"

class FeedPage : public QWidget
{
    Q_OBJECT
public:
    explicit FeedPage(FeedController *ctrl, QWidget *parent = nullptr);
    void updateStatus(const feeder_status_t &status);

private slots:
    void onFeedClicked();
    void onStopClicked();

private:
    void setupUi();

    FeedController *m_ctrl;

    QProgressBar *m_barFood;
    QLabel       *m_lblFoodGram;
    QLabel       *m_lblTodayCount;
    QLabel       *m_lblTodayGram;
    QLabel       *m_lblIrStatus;
    QSpinBox     *m_spinGram;
    QPushButton  *m_btnFeed;
    QPushButton  *m_btnStop;
};

#endif // FEEDPAGE_H
