#ifndef TIMERPAGE_H
#define TIMERPAGE_H

#include <QWidget>
#include <QTableWidget>
#include <QPushButton>
#include <QDialog>
#include <QTimeEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include "feedcontroller.h"

// 添加/编辑定时任务对话框
class TimerTaskDialog : public QDialog
{
    Q_OBJECT
public:
    explicit TimerTaskDialog(QWidget *parent = nullptr);
    void loadTask(const timer_task_t &task);
    timer_task_t getTask() const;

private:
    QTimeEdit *m_timeEdit;
    QSpinBox  *m_spinGram;
    QComboBox *m_cmbRepeat;
    QCheckBox *m_chkEnable;
    int        m_taskId;
};

class TimerPage : public QWidget
{
    Q_OBJECT
public:
    explicit TimerPage(FeedController *ctrl, QWidget *parent = nullptr);

private slots:
    void onAddTask();
    void onEditTask();
    void onRemoveTask();
    void onToggleTask();
    void refreshTable();

private:
    void setupUi();
    void applyStyle(QTableWidget *t);

    FeedController *m_ctrl;
    QTableWidget   *m_table;
    QPushButton    *m_btnAdd;
    QPushButton    *m_btnEdit;
    QPushButton    *m_btnRemove;
    QPushButton    *m_btnToggle;

    static const int MAX_TASKS = 20;
};

#endif // TIMERPAGE_H
