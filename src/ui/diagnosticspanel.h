#ifndef DIAGNOSTICSPANEL_H
#define DIAGNOSTICSPANEL_H

#include <QSet>
#include <QVector>
#include <QWidget>

#include "point3dtypes.h"
#include "radarsite.h"
#include "track.h"
#include "trackdiag.h"

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTabWidget;
class QTableWidget;
class TimeSeriesChart;

namespace diagui {
class AsscRibbonWidget;
}

// 左侧侧栏「航迹诊断面板」：
//   Tab 1「虚假航迹」：门限试算沙盒、疑点航迹排序表、单批尸检卡（实测/外推色带 + 生命周期曲线 + 快捷联动）
//   Tab 2「消失/断航」：异常断航 & 波门尸检、断批重起配对、漏批挖掘 & 四级漏斗探针
class DiagnosticsPanel : public QWidget
{
    Q_OBJECT
public:
    DiagnosticsPanel(const TrackStore *store, const RadarSiteStore *sites, QWidget *parent = nullptr);
    ~DiagnosticsPanel() override;

    void setDiagBundle(const trackdiag::TrackDiagBundle *bundle);
    void setDetDataset(const Point3dDataset *detData);
    void refreshAll();

    // ---- 供自检的查询与操作接口 ----
    int falseTrackCount() const;
    int filteredFalseCount() const;
    int brokenTrackCount() const;
    int fragmentPairCount() const;
    int orphanClusterCount() const;
    int funnelFrameCount() const;

    void selectFalseRow(int row);
    void selectBrokenRow(int row);
    void selectFragmentRow(int row);
    void selectOrphanRow(int row);
    void selectFunnelTrack(int trackIndex);
    void setSandboxMinQuality(double q);

    void setCurrentTab(int index);
    int currentTab() const;
    void setDisappearSubView(int index);
    int disappearSubView() const;
    const trackdiag::GateAutopsyResult &lastGateAutopsy() const { return m_lastAutopsy; }

signals:
    void mutedTracksChanged(const QSet<int> &mutedIndices);
    void isolateTracksRequested(const QSet<int> &indices, bool zoomToFirst);
    void filterOverlayRequested(bool enabled);
    void gateAutopsyOverlayChanged(const trackdiag::GateAutopsyResult &res, bool visible);
    void fragmentOverlayChanged(int idxA, int idxB, bool visible);
    void orphanClusterSelected(const trackdiag::OrphanDetCluster &cluster);
    void focusPoint3dRegionRequested(double rMin, double rMax, double vMin, double vMax,
                                     double azMin, double azMax, int frameStart, int frameEnd);
    void loadDetPointsRequested();
    void replaySeekRequested(qint64 timeMs);

private:
    void buildUi();
    QWidget *buildFalseTrackTab();
    QWidget *buildDisappearTab();
    QWidget *buildBrokenSubView();
    QWidget *buildFragmentSubView();
    QWidget *buildOrphanFunnelSubView();

    const trackdiag::TrackDiagBundle &effectiveBundle() const;
    void syncConfigFromSandbox();
    void recomputeFalseTracks();
    void emitCurrentMutedTracks();
    void updateFalseAutopsyCard(int row);
    void emitPoint3dBoxForTrack(int trackIndex);

    void updateDetStatusUi();
    void recomputeDisappearViews();
    void rebuildBrokenTable();
    void rebuildFragmentTable();
    void rebuildOrphanTable();
    void rebuildFunnelTrackCombo();
    void runFunnelForSelectedTrack();
    void populateGateTable(const trackdiag::GateAutopsyResult &res);
    void emitPoint3dBoxForGate(const trackdiag::GateAutopsyResult &res);
    void emitPoint3dBoxForOrphan(const trackdiag::OrphanDetCluster &cl);

    const TrackStore *m_store = nullptr;
    const RadarSiteStore *m_sites = nullptr;
    const trackdiag::TrackDiagBundle *m_bundle = nullptr;
    const Point3dDataset *m_detData = nullptr;
    trackdiag::TrackDiagBundle m_emptyBundle;

    // 顶层双页
    QTabWidget *m_tabs = nullptr;

    // ---- Tab 1：虚假航迹 ----
    trackdiag::FalseTrackConfig m_falseCfg;
    QVector<trackdiag::FalseTrackItem> m_falseItems;
    int m_selectedFalseRow = -1;
    bool m_updatingSandbox = false;

    QWidget *m_sandboxBody = nullptr;
    QLabel *m_sandboxChevron = nullptr;
    QDoubleSpinBox *m_spinMinQuality = nullptr;
    QSpinBox *m_spinMinAsscPct = nullptr;
    QSpinBox *m_spinMinAsscPts = nullptr;
    QSpinBox *m_spinMaxSuccLoss = nullptr;
    QDoubleSpinBox *m_spinMinSnr = nullptr;
    QCheckBox *m_chkMuteFiltered = nullptr;
    QLabel *m_falseStatsLabel = nullptr;

    QTableWidget *m_falseTable = nullptr;
    QLabel *m_autopsyTitle = nullptr;
    diagui::AsscRibbonWidget *m_asscRibbon = nullptr;
    QLabel *m_autopsySummary = nullptr;
    TimeSeriesChart *m_lifeChart = nullptr;
    QPushButton *m_btnFocusTrack = nullptr;
    QPushButton *m_btnFilterOverlay = nullptr;
    QPushButton *m_btnPoint3dBox = nullptr;
    QPushButton *m_btnRestoreAll = nullptr;

    // ---- Tab 2：消失/断航 ----
    QLabel *m_detStatusLabel = nullptr;
    QButtonGroup *m_subViewGroup = nullptr;
    QPushButton *m_subViewBtns[3] = { nullptr, nullptr, nullptr };
    QStackedWidget *m_disappearStack = nullptr;

    // 子视图 (a)：异常断航 & 波门尸检
    QVector<trackdiag::BrokenTrackItem> m_brokenItems;
    trackdiag::GateAutopsyResult m_lastAutopsy;
    int m_selectedBrokenRow = -1;
    QLabel *m_brokenCountLabel = nullptr;
    QTableWidget *m_brokenTable = nullptr;
    QLabel *m_autopsyCauseLabel = nullptr;
    QTableWidget *m_gateTable = nullptr;
    QPushButton *m_btnBrokenFocus = nullptr;
    QPushButton *m_btnBrokenBox = nullptr;
    QPushButton *m_btnClearGate = nullptr;

    // 子视图 (b)：断批重起配对
    QVector<trackdiag::TrackFragmentPair> m_fragmentPairs;
    int m_selectedFragmentRow = -1;
    QLabel *m_fragmentCountLabel = nullptr;
    QTableWidget *m_fragmentTable = nullptr;
    QLabel *m_fragmentDetailLabel = nullptr;
    QPushButton *m_btnFragmentSeek = nullptr;
    QPushButton *m_btnClearFragment = nullptr;

    // 子视图 (c)：漏批挖掘 & 四级漏斗
    QVector<trackdiag::OrphanDetCluster> m_orphanClusters;
    int m_selectedOrphanRow = -1;
    QLabel *m_orphanCountLabel = nullptr;
    QTableWidget *m_orphanTable = nullptr;

    QVector<trackdiag::FunnelFrameStat> m_funnelStats;
    QComboBox *m_funnelTrackCombo = nullptr;
    QLabel *m_funnelSummaryLabel = nullptr;
    QTableWidget *m_funnelTable = nullptr;
    bool m_populatingFunnelCombo = false;
};

#endif // DIAGNOSTICSPANEL_H
