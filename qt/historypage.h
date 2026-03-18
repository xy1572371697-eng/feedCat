#ifndef HISTORYPAGE_H
#define HISTORYPAGE_H

#include <QWidget>
#include <QTableWidget>
#include <QPushButton>
#include <QLabel>
#include "feedcontroller.h"

class HistoryPage : public QWidget
{
    Q_OBJECT
public:
    explicit HistoryPage(FeedController *ctrl, QWidget *parent = nullptr);
    void refresh();

private slots:
    void onClearHistory();

private:
    void setupUi();

    FeedController *m_ctrl;
    QTableWidget   *m_table;
    QLabel         *m_lblSummary;
    QPushButton    *m_btnClear;
    QPushButton    *m_btnRefresh;

    static const int MAX_RECORDS = 100;
};

#endif // HISTORYPAGE_H
