#include "historypage.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QDateTime>

HistoryPage::HistoryPage(FeedController *ctrl, QWidget *parent)
    : QWidget(parent), m_ctrl(ctrl)
{
    setupUi();
}

void HistoryPage::setupUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    auto *topLayout = new QHBoxLayout;
    auto *title = new QLabel("喂食记录");
    title->setStyleSheet("font-size:15px; font-weight:bold; color:#e94560;");

    m_lblSummary = new QLabel;
    m_lblSummary->setStyleSheet("font-size:13px; color:#a0a0c0;");

    topLayout->addWidget(title);
    topLayout->addStretch();
    topLayout->addWidget(m_lblSummary);
    root->addLayout(topLayout);

    m_table = new QTableWidget(0, 5);
    m_table->setHorizontalHeaderLabels({"时间", "喂食量", "类型", "结果", ""});
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->hide();
    m_table->setStyleSheet(
        "QTableWidget { background:#16213e; border:1px solid #0f3460; color:#e0e0e0;"
        "  gridline-color:#0f3460; font-size:12px; }"
        "QTableWidget::item:selected { background:#0f3460; color:#e94560; }"
        "QHeaderView::section { background:#0f3460; color:#e94560; border:none; padding:4px;"
        "  font-size:13px; font-weight:bold; }"
        "QTableWidget { alternate-background-color:#1a1a2e; }"
    );
    root->addWidget(m_table, 1);

    auto *btnLayout = new QHBoxLayout;
    m_btnRefresh = new QPushButton("刷新");
    m_btnClear   = new QPushButton("清空记录");
    for (auto *b : {m_btnRefresh, m_btnClear}) {
        b->setFixedHeight(34);
        b->setStyleSheet("QPushButton{background:#0f3460;border-radius:5px;color:#e0e0e0;font-size:13px;padding:0 12px;}"
                         "QPushButton:hover{background:#e94560;}");
    }
    btnLayout->addStretch();
    btnLayout->addWidget(m_btnRefresh);
    btnLayout->addWidget(m_btnClear);
    root->addLayout(btnLayout);

    connect(m_btnRefresh, &QPushButton::clicked, this, &HistoryPage::refresh);
    connect(m_btnClear,   &QPushButton::clicked, this, &HistoryPage::onClearHistory);
}

void HistoryPage::refresh()
{
    feed_history_item_t items[MAX_RECORDS];
    int count = m_ctrl->getFeedHistoryCount();
    if (count <= 0) {
        m_table->setRowCount(0);
        m_lblSummary->setText("暂无记录");
        return;
    }

    m_ctrl->getFeedHistory(items, MAX_RECORDS);

    m_table->setRowCount(0);
    static const char *typeStr[]   = {"手动", "自动", "定时"};
    int todayCount = 0, todayGram = 0;
    QDate today = QDate::currentDate();

    for (int i = count - 1; i >= 0; i--) {  // 最新在前
        const auto &item = items[i];
        int row = m_table->rowCount();
        m_table->insertRow(row);

        QDateTime dt = QDateTime::fromSecsSinceEpoch(item.timestamp);
        auto cell = [&](const QString &text) {
            auto *it = new QTableWidgetItem(text);
            it->setTextAlignment(Qt::AlignCenter);
            return it;
        };

        m_table->setItem(row, 0, cell(dt.toString("MM-dd HH:mm:ss")));
        m_table->setItem(row, 1, cell(QString("%1 g").arg(item.gram)));
        m_table->setItem(row, 2, cell(typeStr[qBound(0, item.type, 2)]));

        auto *resultItem = cell(item.success ? "✅ 成功" : "❌ 失败");
        resultItem->setForeground(item.success ? QColor("#50c878") : QColor("#ff6b6b"));
        m_table->setItem(row, 3, resultItem);
        m_table->setItem(row, 4, new QTableWidgetItem);

        if (dt.date() == today && item.success) {
            todayCount++;
            todayGram += item.gram;
        }
    }

    m_lblSummary->setText(QString("今日: %1次 / %2g  |  共 %3 条记录")
                           .arg(todayCount).arg(todayGram).arg(count));
}

void HistoryPage::onClearHistory()
{
    if (QMessageBox::question(this, "确认", "确定清空所有喂食记录？") != QMessageBox::Yes) return;
    m_ctrl->getFeedHistory(nullptr, 0);  // unused
    feeder_clear_feed_history(m_ctrl->ctx());
    refresh();
}
