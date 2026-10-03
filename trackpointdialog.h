#ifndef TRACKPOINTDIALOG_H
#define TRACKPOINTDIALOG_H

#include <QDialog>
#include <QAbstractTableModel>

#include "track.h"

class QTableView;
class QSortFilterProxyModel;

// 点迹表：一行一个点；DisplayRole 为格式化文本，Qt::UserRole 为排序用的原始数值
class TrackPointModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column { Seq, Time, Lat, Lon, AltFt, Heading, Speed, VRate, ColumnCount };

    explicit TrackPointModel(const Track &track, QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation o, int role = Qt::DisplayRole) const override;

    static QString text(const TrackPoint &p, int row, int column);

private:
    QVector<TrackPoint> m_points;
};

// 「查看点迹数据」：可按列排序，可导出 CSV（UTF-8 带 BOM，Excel 直接打开不乱码）
class TrackPointDialog : public QDialog
{
    Q_OBJECT
public:
    explicit TrackPointDialog(const Track &track, QWidget *parent = nullptr);

    int rowCount() const;
    // 按当前排序导出；成功返回 true
    bool exportCsv(const QString &path, QString *error = nullptr) const;

private:
    void onExport();

    Track m_track;
    TrackPointModel *m_model = nullptr;
    QSortFilterProxyModel *m_proxy = nullptr;
    QTableView *m_view = nullptr;
};

#endif // TRACKPOINTDIALOG_H
