#ifndef TRACKPICKER_H
#define TRACKPICKER_H

#include <QDialog>
#include <QSet>
#include <QVector>

#include "trackdb.h"

class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

// 从地球侧数据库挑选航迹（点云窗口的航迹叠加来源）。
// 只有带测量坐标（prange/pvr/paz）的航迹可勾选；旧数据置灰并提示重新导入。
class TrackPicker : public QDialog
{
    Q_OBJECT
public:
    explicit TrackPicker(QWidget *parent = nullptr,
                         const QSet<QString> &preselected = QSet<QString>());

    void setPreselectedKeys(const QSet<QString> &keys);
    QVector<DbTrackSummary> selectedTracks() const;

private:
    void reload();
    void applyFilter(const QString &text);
    void updateBatchCheckState(QTreeWidgetItem *batchItem);

    QTreeWidget *m_tree = nullptr;
    QLineEdit *m_filter = nullptr;
    QVector<DbTrackSummary> m_summaries;
    QSet<QString> m_preselected;
    bool m_updating = false;
};

#endif // TRACKPICKER_H
