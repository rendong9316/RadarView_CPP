#ifndef POINT3DSIDEPANEL_H
#define POINT3DSIDEPANEL_H

#include <QJsonArray>
#include <QScrollArea>
#include <QSet>

#include "point3dtypes.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QSpinBox;

class Point3dCanvas;

// 左侧「显示」侧栏（设置页）：视图 / 显示 / 尺寸 / 色标 SNR / 区域筛选。
// 最小尺寸固定得很小，避免 QScrollArea 把内容的 minimumSizeHint 顶到窗口最小宽度上。
class Point3dSidePanel : public QScrollArea
{
    Q_OBJECT
public:
    Point3dSidePanel(Point3dCanvas *canvas, const Point3dDataset *data, QWidget *parent = nullptr);

    QSize minimumSizeHint() const override { return QSize(160, 160); }

    void restoreDefaults();
    void setRestoring(bool restoring) { m_restoring = restoring; }
    void setPanelActive(bool active) { m_panelActive = active; }

    Point3dViewMode viewMode() const;
    void setViewMode(Point3dViewMode mode);
    void setWindowLength(int len);

    void setTrackSource(Point3dTrackSource src);
    void toggleTrackSource();
    void toggleDetections();
    void toggleTracks();
    void toggleLabels();

    void applySnrRange();
    void syncSnrControls();

    // 内嵌叠加航迹列表：刷新列表项 / 同步高亮行
    void refreshTrackList(const QSet<QString> &hiddenKeys, int highlightedBatch = -1);
    void setHighlightedTrackBatch(int batchNo);

    // 区域筛选：每个三轴视图各存一套（切换视图时原样恢复）
    Point3dRegion regionForView(Point3dViewMode mode) const;
    bool regionAppliedForView(Point3dViewMode mode) const;
    bool setRegionForCurrentView(const Point3dRegion &r, bool apply);
    void loadRegionForCurrentView();
    void revalidateRegions();
    QJsonArray regionsToJson() const;
    void restoreRegionsFromJson(const QJsonArray &regions);

signals:
    void viewModeChanged(Point3dViewMode mode);
    void statusChanged();
    void statusMessage(const QString &text);
    void chooseTracksRequested();
    void hiddenTracksChanged(const QSet<QString> &hiddenKeys);
    void trackRowClicked(int batchNo, TrackSource source, qint64 t0, qint64 t1);
    void trackRowDoubleClicked(int batchNo, TrackSource source, qint64 t0, qint64 t1);

private:
    struct ViewRegion {
        Point3dRegion region;
        bool applied = false;
    };

    void buildUi();
    void connectSignals();
    void onViewModeChanged();
    void onWindowLengthChanged();
    void applyWindowLength(int len);
    void onTrackFilterChanged(const QString &text);
    void onTrackItemChanged(QListWidgetItem *item);
    void setAllTracksChecked(bool checked);
    void updateTrackSummary();
    void onRegionSpinEdited();
    void applyRegion();
    void resetRegion();
    void persistRegion(int mode);
    void updateRegionButtons();
    bool regionBoxVisible() const;

    Point3dCanvas *m_canvas = nullptr;
    const Point3dDataset *m_data = nullptr;

    QComboBox *m_viewCombo = nullptr;
    QComboBox *m_windowCombo = nullptr;
    QSpinBox *m_windowSpin = nullptr;
    QPushButton *m_sourceBtns[3] = { nullptr, nullptr, nullptr };   // 点源分段切换：平滑 / 关联 / 预测
    QPushButton *m_pointBtn = nullptr;
    QPushButton *m_trackBtn = nullptr;
    QPushButton *m_labelBtn = nullptr;

    // 叠加航迹快速勾选列表
    QPushButton *m_trackAllBtn = nullptr;
    QPushButton *m_trackNoneBtn = nullptr;
    QPushButton *m_trackChooseBtn = nullptr;
    QLineEdit *m_trackFilter = nullptr;
    QLabel *m_trackSummary = nullptr;
    QListWidget *m_trackList = nullptr;
    bool m_trackListSyncing = false;
    QDoubleSpinBox *m_pointSizeSpin = nullptr;
    QDoubleSpinBox *m_lineWidthSpin = nullptr;
    QDoubleSpinBox *m_labelSizeSpin = nullptr;
    QDoubleSpinBox *m_hoverSizeSpin = nullptr;
    QCheckBox *m_snrAuto = nullptr;       // 色标 SNR 自动
    QDoubleSpinBox *m_snrLoSpin = nullptr;
    QDoubleSpinBox *m_snrHiSpin = nullptr;

    // 区域筛选栏
    QLabel *m_regionAxisLabel[3] = { nullptr, nullptr, nullptr };
    QDoubleSpinBox *m_regionMinSpin[3] = { nullptr, nullptr, nullptr };
    QDoubleSpinBox *m_regionMaxSpin[3] = { nullptr, nullptr, nullptr };
    QPushButton *m_regionApplyBtn = nullptr;
    QPushButton *m_regionResetBtn = nullptr;
    ViewRegion m_viewRegions[3];
    bool m_regionLoading = false;
    bool m_panelActive = true;
    bool m_restoring = false;
};

#endif // POINT3DSIDEPANEL_H
