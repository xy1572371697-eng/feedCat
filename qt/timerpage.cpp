#include "timerpage.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QMessageBox>
#include <QGroupBox>

// ==================== TimerTaskDialog ====================
TimerTaskDialog::TimerTaskDialog(QWidget *parent)
    : QDialog(parent), m_taskId(-1)
{
    setWindowTitle("定时喂食任务");
    setFixedSize(320, 240);
    setStyleSheet("background:#1a1a2e; color:#e0e0e0;"
                  "QLabel{font-size:13px;} QGroupBox{border:1px solid #0f3460;border-radius:6px;}");

    auto *form = new QFormLayout;
    form->setSpacing(10);
    form->setContentsMargins(16, 16, 16, 8);

    m_timeEdit = new QTimeEdit;
    m_timeEdit->setDisplayFormat("HH:mm");
    m_timeEdit->setStyleSheet("background:#16213e; border:1px solid #0f3460; border-radius:4px;"
                              " color:#e0e0e0; font-size:16px; padding:2px;");

    m_spinGram = new QSpinBox;
    m_spinGram->setRange(5, 100);
    m_spinGram->setValue(25);
    m_spinGram->setSuffix(" g");
    m_spinGram->setStyleSheet("background:#16213e; border:1px solid #0f3460; border-radius:4px;"
                              " color:#e0e0e0; font-size:14px; padding:2px;");

    m_cmbRepeat = new QComboBox;
    m_cmbRepeat->addItems({"单次", "每天", "每周"});
    m_cmbRepeat->setStyleSheet("background:#16213e; border:1px solid #0f3460; border-radius:4px;"
                               " color:#e0e0e0; font-size:14px; padding:2px;");

    m_chkEnable = new QCheckBox("启用此任务");
    m_chkEnable->setChecked(true);
    m_chkEnable->setStyleSheet("font-size:13px; color:#a0a0c0;");

    form->addRow("喂食时间:", m_timeEdit);
    form->addRow("喂食量:  ", m_spinGram);
    form->addRow("重复:    ", m_cmbRepeat);
    form->addRow("",          m_chkEnable);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->setStyleSheet("QPushButton{background:#0f3460;border-radius:5px;padding:4px 12px;color:#e0e0e0;}"
                           "QPushButton:hover{background:#e94560;}");
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *root = new QVBoxLayout(this);
    root->addLayout(form);
    root->addWidget(buttons);
}

void TimerTaskDialog::loadTask(const timer_task_t &task)
{
    m_taskId = task.id;
    m_timeEdit->setTime(QTime(task.hour, task.minute, 0));
    m_spinGram->setValue(task.feed_amount);
    m_cmbRepeat->setCurrentIndex(task.repeat);
    m_chkEnable->setChecked(task.enabled);
}

timer_task_t TimerTaskDialog::getTask() const
{
    timer_task_t t;
    t.id          = m_taskId;
    t.hour        = m_timeEdit->time().hour();
    t.minute      = m_timeEdit->time().minute();
    t.second      = 0;
    t.feed_amount = m_spinGram->value();
    t.repeat      = m_cmbRepeat->currentIndex();
    t.enabled     = m_chkEnable->isChecked() ? 1 : 0;
    return t;
}

// ==================== TimerPage ====================
TimerPage::TimerPage(FeedController *ctrl, QWidget *parent)
    : QWidget(parent), m_ctrl(ctrl)
{
    setupUi();
    refreshTable();
}

void TimerPage::setupUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    auto *title = new QLabel("定时喂食任务 (最多20个)");
    title->setStyleSheet("font-size:15px; font-weight:bold; color:#e94560;");
    root->addWidget(title);

    // 表格
    m_table = new QTableWidget(0, 5);
    m_table->setHorizontalHeaderLabels({"ID", "时间", "喂食量", "重复", "状态"});
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->hide();
    m_table->setStyleSheet(
        "QTableWidget { background:#16213e; border:1px solid #0f3460; color:#e0e0e0;"
        "  gridline-color:#0f3460; font-size:13px; }"
        "QTableWidget::item:selected { background:#0f3460; color:#e94560; }"
        "QHeaderView::section { background:#0f3460; color:#e94560; border:none; padding:4px;"
        "  font-size:13px; font-weight:bold; }"
        "QTableWidget { alternate-background-color:#1a1a2e; }"
    );
    root->addWidget(m_table, 1);

    // 操作按钮
    auto *btnLayout = new QHBoxLayout;
    btnLayout->setSpacing(8);

    auto makeBtn = [&](const QString &text, const QString &color) {
        auto *btn = new QPushButton(text);
        btn->setFixedHeight(36);
        btn->setStyleSheet(QString(
            "QPushButton { background:%1; border-radius:6px; font-size:13px; color:white; }"
            "QPushButton:hover { opacity:0.8; }"
        ).arg(color));
        return btn;
    };

    m_btnAdd    = makeBtn("+ 添加",    "#e94560");
    m_btnEdit   = makeBtn("✎ 编辑",    "#0f3460");
    m_btnToggle = makeBtn("⏸ 启用/停用", "#4a4a7a");
    m_btnRemove = makeBtn("✕ 删除",    "#6a2040");

    btnLayout->addWidget(m_btnAdd);
    btnLayout->addWidget(m_btnEdit);
    btnLayout->addWidget(m_btnToggle);
    btnLayout->addWidget(m_btnRemove);
    btnLayout->addStretch();
    root->addLayout(btnLayout);

    connect(m_btnAdd,    &QPushButton::clicked, this, &TimerPage::onAddTask);
    connect(m_btnEdit,   &QPushButton::clicked, this, &TimerPage::onEditTask);
    connect(m_btnRemove, &QPushButton::clicked, this, &TimerPage::onRemoveTask);
    connect(m_btnToggle, &QPushButton::clicked, this, &TimerPage::onToggleTask);
}

void TimerPage::refreshTable()
{
    timer_task_t tasks[MAX_TASKS];
    int count = m_ctrl->getTimerTasks(tasks, MAX_TASKS);

    m_table->setRowCount(0);

    static const char *repeatStr[] = {"单次", "每天", "每周"};

    for (int i = 0; i < count; i++) {
        const auto &t = tasks[i];
        int row = m_table->rowCount();
        m_table->insertRow(row);

        auto cell = [&](const QString &text) {
            auto *item = new QTableWidgetItem(text);
            item->setTextAlignment(Qt::AlignCenter);
            return item;
        };

        m_table->setItem(row, 0, cell(QString::number(t.id)));
        m_table->setItem(row, 1, cell(QString("%1:%2")
                                      .arg(t.hour, 2, 10, QChar('0'))
                                      .arg(t.minute, 2, 10, QChar('0'))));
        m_table->setItem(row, 2, cell(QString("%1 g").arg(t.feed_amount)));
        m_table->setItem(row, 3, cell(repeatStr[qBound(0, t.repeat, 2)]));

        auto *statusItem = cell(t.enabled ? "✅ 已启用" : "⛔ 已停用");
        statusItem->setForeground(t.enabled ? QColor("#50c878") : QColor("#ff6b6b"));
        m_table->setItem(row, 4, statusItem);
    }
}

void TimerPage::onAddTask()
{
    int count = m_ctrl->getTimerTasks(nullptr, 0);
    if (count >= MAX_TASKS) {
        QMessageBox::information(this, "提示", "定时任务已达上限（20个）");
        return;
    }

    TimerTaskDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;

    timer_task_t task = dlg.getTask();
    int id = m_ctrl->addTimerTask(task);
    if (id < 0)
        QMessageBox::warning(this, "失败", "添加定时任务失败");
    refreshTable();
}

void TimerPage::onEditTask()
{
    int row = m_table->currentRow();
    if (row < 0) { QMessageBox::information(this, "提示", "请先选择一个任务"); return; }

    int taskId = m_table->item(row, 0)->text().toInt();
    timer_task_t tasks[MAX_TASKS];
    int count = m_ctrl->getTimerTasks(tasks, MAX_TASKS);

    timer_task_t *found = nullptr;
    for (int i = 0; i < count; i++) {
        if (tasks[i].id == taskId) { found = &tasks[i]; break; }
    }
    if (!found) return;

    TimerTaskDialog dlg(this);
    dlg.loadTask(*found);
    if (dlg.exec() != QDialog::Accepted) return;

    timer_task_t updated = dlg.getTask();
    updated.id = taskId;
    if (!m_ctrl->modifyTimerTask(updated))
        QMessageBox::warning(this, "失败", "修改定时任务失败");
    refreshTable();
}

void TimerPage::onRemoveTask()
{
    int row = m_table->currentRow();
    if (row < 0) { QMessageBox::information(this, "提示", "请先选择一个任务"); return; }

    int taskId = m_table->item(row, 0)->text().toInt();
    if (QMessageBox::question(this, "确认", "确定删除该任务？") != QMessageBox::Yes) return;

    m_ctrl->removeTimerTask(taskId);
    refreshTable();
}

void TimerPage::onToggleTask()
{
    int row = m_table->currentRow();
    if (row < 0) { QMessageBox::information(this, "提示", "请先选择一个任务"); return; }

    int taskId = m_table->item(row, 0)->text().toInt();
    timer_task_t tasks[MAX_TASKS];
    int count = m_ctrl->getTimerTasks(tasks, MAX_TASKS);
    for (int i = 0; i < count; i++) {
        if (tasks[i].id == taskId) {
            m_ctrl->enableTimerTask(taskId, !tasks[i].enabled);
            break;
        }
    }
    refreshTable();
}
