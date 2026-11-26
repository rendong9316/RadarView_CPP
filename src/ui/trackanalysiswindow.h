#ifndef TRACKANALYSISWINDOW_H
#define TRACKANALYSISWINDOW_H

#include <QWidget>
#include <QVector>

#include "track.h"
#include "trackanalysis.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QVBoxLayout;
class TimeSeriesChart;

// 独立非模态「航迹时序分析与定量对比」窗口：
//   Tab 1: 单航迹多点列时序曲线（prange / paz / pvr / snr / Δt / 滤波新息；ADS-B 高度/地速/航向/垂直率）
//   Tab 2: 两航迹定量误差对比（RMSE / MAE / P95 / Max、误差曲线、地图误差连线叠加与 CSV 导出）
class TrackAnalysisWindow : public QWidget
{
    Q_OBJECT
public:
    explicit TrackAnalysisWindow(const TrackStore *store, QWidget *parent = nullptr);
    ~TrackAnalysisWindow() override;

    void refreshTracks();
    void openForTrack(int trackIndex, int tabIndex = 0);
    void openCompare(int trackIndexA, int trackIndexB);
    void setReplayTime(qint64 ms, bool active);

    // 自检/诊断接口
    int currentTab() const;
    const trackanalysis::TrackCompareResult &lastCompareResult() const { return m_lastCompare; }
    bool exportLastCompareCsv(const QString &path, QString *error = nullptr) const;
    int seriesChartCount() const { return m_seriesCharts.size(); }

signals:
    void timeLocateRequested(const QString &trackKey, qint64 timeMs, double lonDeg, double latDeg, const QString &label);
    void replaySeekRequested(qint64 timeMs);
    void compareOverlayChanged(const QVector<trackanalysis::TrackDiffSample> &samples,
                               int maxErrIdx, bool visible);

protected:
    void closeEvent(QCloseEvent *e) override;

private:
    void buildUi();
    QWidget *buildSeriesTab();
    QWidget *buildCompareTab();
    void rebuildSeriesCharts();
    void runComparison();
    void applyPresetCompare(RadarPointMode modeA, RadarPointMode modeB);
    void syncChartsXRange(const QVector<TimeSeriesChart *> &group, TimeSeriesChart *src, double lo, double hi);
    void syncChartsHover(const QVector<TimeSeriesChart *> &group, TimeSeriesChart *src, qint64 ms, bool valid);
    QString trackDisplayText(const Track &t) const;
    void emitCompareOverlay();

    const TrackStore *m_store = nullptr;
    QTabWidget *m_tabs = nullptr;

    // Tab 1: 时序曲线
    QComboBox *m_trackCombo = nullptr;
    QCheckBox *m_chkSmooth = nullptr;
    QCheckBox *m_chkAssc = nullptr;
    QCheckBox *m_chkPred = nullptr;
    QCheckBox *m_chkResiduals = nullptr;
    QLabel *m_seriesSummary = nullptr;
    QVBoxLayout *m_chartsLay = nullptr;
    QVector<TimeSeriesChart *> m_seriesCharts;

    // Tab 2: 误差对比
    QComboBox *m_cmpTrackA = nullptr;
    QComboBox *m_cmpModeA = nullptr;
    QComboBox *m_cmpTrackB = nullptr;
    QComboBox *m_cmpModeB = nullptr;
    QSpinBox *m_maxGapSpin = nullptr;
    QCheckBox *m_showMapLinks = nullptr;
    QLabel *m_cmpSummary = nullptr;
    QVector<TimeSeriesChart *> m_cmpCharts;
    trackanalysis::TrackCompareResult m_lastCompare;

    bool m_replayActive = false;
    qint64 m_replayMs = 0;
    bool m_syncingRange = false;
    bool m_populating = false;
};

#endif // TRACKANALYSISWINDOW_H
