#ifndef TRACKPICKER_H
#define TRACKPICKER_H

#include <QDialog>
#include <QVector>

#include "trackdb.h"

class QLineEdit;
class QTreeWidget;

// 从地球侧数据库挑选航迹（点云窗口的航迹叠加来源）。
// 只有带测量坐标（prange/pvr/paz）的航迹可勾选；旧数据置灰并提示重新导入。
class TrackPicker : public QDialog
{
    Q_OBJECT
public:
    explicit TrackPicker(QWidget *parent = nullptr);

    QVector<DbTrackSummary> selectedTracks() const;

private:
    void reload();
    void applyFilter(const QString &text);

    QTreeWidget *m_tree = nullptr;
    QLineEdit *m_filter = nullptr;
    QVector<DbTrackSummary> m_summaries;
    bool m_updating = false;
};

#endif // TRACKPICKER_H
