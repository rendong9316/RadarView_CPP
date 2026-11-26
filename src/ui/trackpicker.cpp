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

TrackPicker::TrackPicker(QWidget *parent, const QSet<QString> &preselected)
    : QDialog(parent), m_preselected(preselected)
{
    setWindowTitle(QStringLiteral("从数据库选择航迹"));
    setModal(true);
    resize(780, 540);

    QVBoxLayout *root = new QVBoxLayout(this);
    ui::bindMargins(root, 16, 14, 16, 14);
    ui::bindSpacing(root, 10);

    QLabel *tip = new QLabel(QStringLiteral(
        "航迹来自主窗口已导入的数据库数据。勾选后叠加到点云视图，并与地图双向高亮。\n"
        "雷达航迹一次导入即包含「平滑 / 关联 / 预测」三套点列（左侧点源随时切换）；点数列显示“平滑 / 关联 / 预测”。\n"
        "灰色条目缺少测量坐标（升级前导入），请在地球侧重新导入该 MAT。"), this);
    tip->setObjectName(QStringLiteral("pickerTip"));
    tip->setWordWrap(true);
    root->addWidget(tip);

    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(QStringLiteral("按文件、批次、航班号过滤…"));
    root->addWidget(m_filter);

    m_tree = new QTreeWidget(this);
    m_tree->setHeaderLabels(QStringList() << QStringLiteral("航迹 / 批次")
                                          << QStringLiteral("点数（平滑/关联/预测）")
                                          << QStringLiteral("时间范围"));
    m_tree->setColumnWidth(0, 360);
    m_tree->setColumnWidth(1, 140);
    m_tree->setRootIsDecorated(true);
    m_tree->setAlternatingRowColors(true);
    root->addWidget(m_tree, 1);

    QDialogButtonBox *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    box->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
    box->button(QDialogButtonBox::Ok)->setDefault(true);
    box->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    root->addWidget(box);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(m_filter, &QLineEdit::textChanged, this, &TrackPicker::applyFilter);
    connect(m_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int) {
        if (m_updating || !item)
            return;
        m_updating = true;
        if (!item->parent()) {
            const Qt::CheckState state = item->checkState(0) == Qt::Unchecked ? Qt::Unchecked : Qt::Checked;
            item->setCheckState(0, state);
            for (int i = 0; i < item->childCount(); ++i) {
                QTreeWidgetItem *child = item->child(i);
                if (child->flags() & Qt::ItemIsUserCheckable)
                    child->setCheckState(0, state);
            }
        } else {
            updateBatchCheckState(item->parent());
        }
        m_updating = false;
    });

    setThemedStyle(this, ui::dialogQss() + QStringLiteral(
        "#pickerTip { color: var(--text-secondary); font-size: 11px; }"
        "QTreeWidget { color: var(--text-primary); background: var(--bg-primary);"
        " alternate-background-color: var(--bg-secondary);"
        " border: 1px solid var(--border-primary); border-radius: 4px; font-family: %1; font-size: 12px; outline: none; }"
        "QTreeWidget::item { padding: 3px 0px; }"
        "QTreeWidget::item:selected { background: var(--accent-primary); color: var(--accent-fg); }"
        "QHeaderView::section { background: var(--bg-secondary); color: var(--text-secondary);"
        " border: none; border-bottom: 1px solid var(--border-primary); padding: 5px 8px;"
        " font-family: %1; font-size: 11px; font-weight: 600; }").arg(ui::uiFamilies()));

    reload();
}

void TrackPicker::setPreselectedKeys(const QSet<QString> &keys)
{
    m_preselected = keys;
    reload();
}

void TrackPicker::updateBatchCheckState(QTreeWidgetItem *batchItem)
{
    if (!batchItem)
        return;
    int checkable = 0, checked = 0;
    for (int i = 0; i < batchItem->childCount(); ++i) {
        QTreeWidgetItem *child = batchItem->child(i);
        if (!(child->flags() & Qt::ItemIsUserCheckable))
            continue;
        ++checkable;
        if (child->checkState(0) == Qt::Checked)
            ++checked;
    }
    if (checkable == 0 || checked == 0)
        batchItem->setCheckState(0, Qt::Unchecked);
    else if (checked == checkable)
        batchItem->setCheckState(0, Qt::Checked);
    else
        batchItem->setCheckState(0, Qt::PartiallyChecked);
}

void TrackPicker::reload()
{
    m_updating = true;
    m_tree->clear();
    QString err;
    m_summaries = trackdb::trackSummaries(QString(), &err);
    if (m_summaries.isEmpty()) {
        QTreeWidgetItem *empty = new QTreeWidgetItem(m_tree);
        empty->setText(0, err.isEmpty() ? QStringLiteral("数据库中没有可用的雷达航迹")
                                        : QStringLiteral("读取失败：%1").arg(err));
        empty->setFlags(Qt::NoItemFlags);
        m_updating = false;
        return;
    }

    QTreeWidgetItem *batchItem = nullptr;
    qint64 currentBatch = -1;
    for (int i = 0; i < m_summaries.size(); ++i) {
        const DbTrackSummary &s = m_summaries.at(i);
        if (!batchItem || s.batchId != currentBatch) {
            if (batchItem)
                updateBatchCheckState(batchItem);
            currentBatch = s.batchId;
            batchItem = new QTreeWidgetItem(m_tree);
            batchItem->setText(0, QStringLiteral("%1  ·  %2")
                                      .arg(s.displayName.isEmpty() ? s.fileName : s.displayName,
                                           trackSourceName(s.source)));
            batchItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
            batchItem->setCheckState(0, Qt::Unchecked);
            batchItem->setExpanded(true);
        }
        QTreeWidgetItem *child = new QTreeWidgetItem(batchItem);
        child->setText(0, QStringLiteral("%1   %2")
                              .arg(s.icao, s.flightNo.isEmpty() ? QStringLiteral("-") : s.flightNo));
        const QString pts = QStringLiteral("%1 / %2 / %3")
                                .arg(s.pointCount)
                                .arg(s.rawCount > 0 ? QString::number(s.rawCount) : QStringLiteral("-"))
                                .arg(s.predCount > 0 ? QString::number(s.predCount) : QStringLiteral("-"));
        child->setText(1, pts);
        child->setText(2, QStringLiteral("%1 ~ %2")
                              .arg(formatBeijingTime(s.minMs), formatBeijingTime(s.maxMs)));
        child->setData(0, Qt::UserRole, i);

        const bool usable = s.hasMeasure || s.hasRaw || s.hasPred;
        QStringList cols;
        if (s.hasMeasure) cols << QStringLiteral("平滑");
        if (s.hasRaw) cols << QStringLiteral("关联");
        if (s.hasPred) cols << QStringLiteral("预测");
        const QString rowTip = cols.isEmpty()
            ? QStringLiteral("缺少测量坐标（prange/pvr/paz），请在地球侧重新导入该 MAT")
            : QStringLiteral("可叠加点列：%1（左侧点源按钮随时切换）").arg(cols.join(QStringLiteral(" / ")));
        child->setToolTip(0, rowTip);
        child->setToolTip(1, rowTip);
        child->setToolTip(2, rowTip);
        const QString key = s.icao + QStringLiteral("::") + trackSourceName(s.source)
                          + QStringLiteral("::") + s.fileName;
        if (usable) {
            child->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
            child->setCheckState(0, m_preselected.contains(key) ? Qt::Checked : Qt::Unchecked);
        } else {
            child->setFlags(Qt::ItemIsEnabled);
            child->setForeground(0, QColor(140, 146, 158));
            child->setForeground(1, QColor(140, 146, 158));
            child->setForeground(2, QColor(140, 146, 158));
        }
    }
    if (batchItem)
        updateBatchCheckState(batchItem);
    m_tree->expandAll();
    m_updating = false;
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
