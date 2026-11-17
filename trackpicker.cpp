#include "trackpicker.h"

#include "theme.h"

#include <QColor>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

TrackPicker::TrackPicker(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("从数据库选择航迹"));
    setModal(true);
    resize(780, 540);

    QVBoxLayout *root = new QVBoxLayout(this);

    QLabel *tip = new QLabel(QStringLiteral(
        "航迹来自主窗口已导入的数据库数据。勾选后叠加到点云视图，并与地图双向高亮。\n"
        "雷达航迹同时带「平滑」「关联」两套点列（点源下拉切换）；点数列显示“平滑 + 关联”。\n"
        "灰色条目没有测量坐标（升级前导入），请在地球侧重新导入该 MAT。"), this);
    tip->setWordWrap(true);
    root->addWidget(tip);

    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(QStringLiteral("按文件、批次、航班号过滤…"));
    root->addWidget(m_filter);

    m_tree = new QTreeWidget(this);
    m_tree->setHeaderLabels(QStringList() << QStringLiteral("航迹 / 批次")
                                          << QStringLiteral("点数（平滑+关联）")
                                          << QStringLiteral("时间范围"));
    m_tree->setColumnWidth(0, 400);
    m_tree->setColumnWidth(1, 70);
    m_tree->setRootIsDecorated(true);
    m_tree->setAlternatingRowColors(true);
    root->addWidget(m_tree, 1);

    QDialogButtonBox *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    box->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
    box->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    root->addWidget(box);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(m_filter, &QLineEdit::textChanged, this, &TrackPicker::applyFilter);
    connect(m_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int) {
        if (m_updating || item->parent())
            return;
        m_updating = true;
        const Qt::CheckState state = item->checkState(0);
        for (int i = 0; i < item->childCount(); ++i) {
            QTreeWidgetItem *child = item->child(i);
            if (child->flags() & Qt::ItemIsUserCheckable)
                child->setCheckState(0, state);
        }
        m_updating = false;
    });

    setThemedStyle(this, QStringLiteral(
        "QDialog { background: var(--bg-secondary); }"
        "QLabel { color: var(--text-secondary); font-family: %1; font-size: 12px; }"
        "QLineEdit { color: var(--text-primary); background: var(--input-bg);"
        " border: 1px solid var(--input-border); border-radius: 3px; padding: 4px 6px;"
        " font-family: %1; font-size: 12px; }"
        "QTreeWidget { color: var(--text-primary); background: var(--bg-primary);"
        " alternate-background-color: var(--bg-tertiary);"
        " border: 1px solid var(--border-primary); font-family: %1; font-size: 12px; }"
        "QTreeWidget::item:selected { background: var(--accent-primary); color: #ffffff; }"
        "QHeaderView::section { background: var(--bg-tertiary); color: var(--text-secondary);"
        " border: none; padding: 4px 6px; font-family: %1; font-size: 12px; }"
        "QPushButton { color: var(--text-secondary); background: var(--button-bg);"
        " border: 1px solid var(--border-primary); border-radius: 3px;"
        " padding: 4px 14px; font-family: %1; font-size: 12px; }"
        "QPushButton:hover { background: var(--button-hover); }").arg(ui::uiFamilies()));

    reload();
}

void TrackPicker::reload()
{
    m_tree->clear();
    QString err;
    m_summaries = trackdb::trackSummaries(QString(), &err);
    if (m_summaries.isEmpty()) {
        QTreeWidgetItem *empty = new QTreeWidgetItem(m_tree);
        empty->setText(0, err.isEmpty() ? QStringLiteral("数据库中没有可用的雷达航迹")
                                        : QStringLiteral("读取失败：%1").arg(err));
        empty->setFlags(Qt::NoItemFlags);
        return;
    }

    QTreeWidgetItem *batchItem = nullptr;
    qint64 currentBatch = -1;
    for (int i = 0; i < m_summaries.size(); ++i) {
        const DbTrackSummary &s = m_summaries.at(i);
        if (!batchItem || s.batchId != currentBatch) {
            currentBatch = s.batchId;
            batchItem = new QTreeWidgetItem(m_tree);
            batchItem->setText(0, QStringLiteral("%1  ·  %2")
                                      .arg(s.fileName, trackSourceName(s.source)));
            batchItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
            batchItem->setCheckState(0, Qt::Unchecked);
            batchItem->setExpanded(true);
        }
        QTreeWidgetItem *child = new QTreeWidgetItem(batchItem);
        child->setText(0, QStringLiteral("%1   %2")
                              .arg(s.icao, s.flightNo.isEmpty() ? QStringLiteral("-") : s.flightNo));
        QString pts = QString::number(s.pointCount);
        if (s.hasRaw)
            pts += QStringLiteral(" + %1").arg(s.rawCount);
        child->setText(1, pts);
        child->setText(2, QStringLiteral("%1 ~ %2")
                              .arg(formatBeijingTime(s.minMs), formatBeijingTime(s.maxMs)));
        child->setData(0, Qt::UserRole, i);

        const bool usable = s.hasMeasure || s.hasRaw;
        QString rowTip;
        if (s.hasMeasure && s.hasRaw)
            rowTip = QStringLiteral("平滑、关联两种点源都可叠加（点源下拉切换）");
        else if (s.hasMeasure)
            rowTip = QStringLiteral("只有平滑点列：该 MAT 的关联点列未入库（重新导入该 MAT 可补上）");
        else if (s.hasRaw)
            rowTip = QStringLiteral("只有关联点列");
        else
            rowTip = QStringLiteral("缺少测量坐标（prange/pvr/paz），请在地球侧重新导入该 MAT");
        child->setToolTip(0, rowTip);
        child->setToolTip(1, rowTip);
        child->setToolTip(2, rowTip);
        if (usable) {
            child->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
            child->setCheckState(0, Qt::Unchecked);
        } else {
            child->setFlags(Qt::ItemIsEnabled);
            child->setForeground(0, QColor(140, 146, 158));
            child->setForeground(1, QColor(140, 146, 158));
            child->setForeground(2, QColor(140, 146, 158));
        }
    }
    m_tree->expandAll();
}

void TrackPicker::applyFilter(const QString &text)
{
    const QString needle = text.trimmed();
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *batch = m_tree->topLevelItem(i);
        if (!needle.isEmpty() && batch->text(0).contains(needle, Qt::CaseInsensitive)) {
            batch->setHidden(false);
            continue;
        }
        bool any = false;
        for (int j = 0; j < batch->childCount(); ++j) {
            const bool match = needle.isEmpty()
                || batch->child(j)->text(0).contains(needle, Qt::CaseInsensitive)
                || batch->child(j)->text(2).contains(needle, Qt::CaseInsensitive);
            batch->child(j)->setHidden(!match);
            any = any || match;
        }
        batch->setHidden(!any);
    }
}

QVector<DbTrackSummary> TrackPicker::selectedTracks() const
{
    QVector<DbTrackSummary> out;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *batch = m_tree->topLevelItem(i);
        for (int j = 0; j < batch->childCount(); ++j) {
            QTreeWidgetItem *child = batch->child(j);
            if (!(child->flags() & Qt::ItemIsUserCheckable)
                    || child->checkState(0) != Qt::Checked)
                continue;
            const int idx = child->data(0, Qt::UserRole).toInt();
            if (idx >= 0 && idx < m_summaries.size())
                out.append(m_summaries.at(idx));
        }
    }
    return out;
}
