#ifndef TRACKPOINTDIALOG_H
#define TRACKPOINTDIALOG_H

#include <QDialog>
#include <QAbstractTableModel>

#include "track.h"

class QLabel;
class QPushButton;
class QTableView;
class QSortFilterProxyModel;

// 点迹表：一行一个点；DisplayRole 为格式化文本，Qt::UserRole 为排序用的原始数值。
// 显示的列随数据源变化：ADS-B 显示高度/航向/地速/垂直率；雷达（含原始测量）
// 只显示时间、经纬度与测量量 prange/paz/pvr/snr。
class TrackPointModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column { Seq, Time, Lat, Lon, AltFt, Heading, Speed, VRate,
                  PRange, PAz, PVr, Snr, ColumnCount };

    explicit TrackPointModel(const Track &track, QObject *parent = nullptr,
                             RadarPointMode mode = RadarPointMode::Smooth);
    void setPointMode(RadarPointMode mode);
    RadarPointMode pointMode() const { return m_mode; }
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation o, int role = Qt::DisplayRole) const override;

    // column 是显示列序号（本航迹只显示相关列），不是 Column 枚举值
    QString text(const TrackPoint &p, int row, int column) const;
    // 模型里的点（SNR 可能已按同一帧关联点补过，与 Track::points 不一定相同）
    const TrackPoint &pointAt(int row) const { return m_points.at(row); }
    bool isRadar() const { return m_radar; }

private:
    void loadMode(RadarPointMode mode);
    Track m_track;
    RadarPointMode m_mode = RadarPointMode::Smooth;
    QVector<TrackPoint> m_points;
    QVector<Column> m_columns;   // 本航迹显示的列，按显示顺序
    bool m_radar = false;
};

// 「查看点迹数据」：可按列排序、切换平滑/关联/预测点列，可导出 CSV（UTF-8 带 BOM，Excel 直接打开不乱码）
class TrackPointDialog : public QDialog
{
    Q_OBJECT
public:
    explicit TrackPointDialog(const Track &track, QWidget *parent = nullptr,
                              RadarPointMode initialMode = RadarPointMode::Smooth);

    int rowCount() const;
    // 当前显示的列数与表头文字（自检用；雷达与 ADS-B 的列不同）
    int columnCount() const;
    QString headerText(int column) const;
    // 按当前排序导出；成功返回 true
    bool exportCsv(const QString &path, QString *error = nullptr) const;
    // 模拟双击某显示行（自检与快捷键共用）
    void activateRow(int proxyRow);

signals:
    void pointActivated(const QString &trackKey, qint64 timeMs, double lonDeg, double latDeg, const QString &label);

private:
    void onExport();
    void switchMode(RadarPointMode mode);
    void updateMetaText();

    Track m_track;
    RadarPointMode m_mode = RadarPointMode::Smooth;
    QLabel *m_meta = nullptr;
    QPushButton *m_modeBtns[3] = { nullptr, nullptr, nullptr };
    TrackPointModel *m_model = nullptr;
    QSortFilterProxyModel *m_proxy = nullptr;
    QTableView *m_view = nullptr;
};

#endif // TRACKPOINTDIALOG_H
