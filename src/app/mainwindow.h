#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QAbstractButton>
#include <QColor>
#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QWidget>

#include "appshellwidgets.h"
#include "drawingmanager.h"
#include "lucide.h"
#include "settingspanel.h"
#include "sidepanels.h"
#include "track.h"
#include "trackdiag.h"
#include "tracklayer.h"

class GlobeWidget;
class QAction;
class QCloseEvent;
class QDialog;
class QSplitter;
class QStackedWidget;
class QTimer;
class QUndoStack;
class ReplayController;
class AppStatusBar;
class SourceStrip;
class ManageState;
class ManagePanel;
class FlagStore;
class RadarSiteStore;
class RulerState;
class SpatialLasso;
class DrawingManager;
class Point3dWindow;
class TrackAnalysisWindow;
class DiagnosticsPanel;
struct Point3dDataset;
class DrawingController;
class MapSourceController;
class Point3dCoordinator;
class TrackImportCoordinator;
namespace ui { class UndoToast; }

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    GlobeWidget *globe() const { return m_globe; }
    bool openTiles(const QString &path);
    void loadDefaultTiles();
    void setMapSource(const QString &id);          // offline / tdt / amap
    bool openOnlineMap(const QString &token, bool useAmap = false);
    void syncSourceActions();

    // 导入数据文件（后台解析，完成后后台入库）；displayName 为空时雷达数据自动命名 Radar / Radar2 ...
    void importFile(TrackSource kind, const QString &path, const QString &displayName = QString(),
                    const QColor &color = QColor());
    bool isImporting() const;
    bool isPersisting() const;
    TrackStore *trackStore() { return &m_store; }
    ReplayController *replay() const { return m_replay; }

    // 单独显示 / 多选隔离航迹（TrackStore 下标），-1 返回全部；回放范围随之切换
    void isolateTrack(int index);
    int isolatedTrack() const;
    void toggleIsolatedTrack(int index);          // Ctrl+点击：加入/移出多选对比集合
    QSet<int> isolatedTracks() const;
    void zoomToTrack(int index);                  // 右键「缩放至该航迹」
    void setTrackColorMode(TrackLayer::TrackColorMode mode);
    TrackLayer::TrackColorMode trackColorMode() const;
    void stepReplay(int dir);                     // 按当前显示航迹采样点单步前后跳
    void stepReplayByMs(qint64 deltaMs);          // 按毫秒前后步进
    // 打开「查看点迹数据」窗口（非模态），返回窗口指针
    QDialog *showTrackPoints(int index);
    // 打开「航迹时序分析与定量对比」窗口（tabIndex: 0=时序曲线, 1=误差对比）
    TrackAnalysisWindow *showTrackAnalysis(int index = -1, int tabIndex = 0);
    TrackAnalysisWindow *analysisWindow() const;
    void setFilterOverlayEnabled(bool on);
    bool filterOverlayEnabled() const;
    // 新建一个「点迹三维可视化」独立窗口（可多开）
    void showPoint3dWindow();
    QList<Point3dWindow *> point3dWindows() const;   // 当前打开的点迹窗口（回归/演示用）
    AppStatusBar *appStatusBar() const { return m_statusBar; }
    SourceStrip *sourceStrip() const { return m_sourceStrip; }
    void setSourceStripOpen(bool open);   // 展开/收起状态栏上方的数据源条

    // ---- 侧栏 / 功能入口（菜单、快捷键、自检共用）----
    void activatePanel(PanelId id);      // 已打开同一面板时关闭侧栏
    void closeSidebar();
    int activePanel() const;             // -1 表示侧栏关闭
    ManageState *manageState() const { return m_manage; }
    ManagePanel *managePanel() const { return m_managePanel; }
    FilterPanel *filterPanel() const { return m_filterPanel; }
    TrackFilterState *filterPanelState() { return &m_filter; }   // 自检用：读筛选状态
    FlagPanel *flagPanel() const { return m_flagPanel; }
    FlagStore *flagStore() const { return m_flags; }
    RadarSiteStore *radarSites() const { return m_radarSites; }
    void showRadarSiteDialog();
    RulerState *ruler() const { return m_ruler; }
    SpatialLasso *spatialLasso() const { return m_lasso; }
    DrawingManager *drawing() const;
    QUndoStack *undoStack() const;
    void setDrawTool(DrawTool t);            // 绘图工具栏切换（Esc 也会回到漫游）
    // 绘图工具栏平时不显示：左上角「绘图」按钮点开才展开
    void setDrawingBarVisible(bool on);
    bool drawingBarVisible() const;
    ui::UndoToast *undoToast() const { return m_undoToast; }
    SettingsPanel *settingsPanel() const { return m_settingsPanel; }
    DiagnosticsPanel *diagnosticsPanel() const { return m_diagPanel; }
    const trackdiag::TrackDiagBundle &diagBundle() const { return m_diagBundle; }
    void toggleLabels();
    void togglePointDots(int index);     // 「显示/隐藏所有对应点迹」
    bool pointDotsShown(int index) const;
    void setTrackColor(int index);       // 右键「设置颜色」：改该航迹所属数据源（文件）的全部航迹颜色
    void applyTrackColor(int index, const QColor &color);   // 非交互版（自检 / setTrackColor 共用）
    void clearAllPointDots();            // 「清空所有点迹显示」：所有航迹都不再画点迹
    void setRadarPointMode(RadarPointMode mode);   // 切换雷达航迹点列：平滑 / 关联 / 预测
    RadarPointMode radarPointMode() const { return m_radarMode; }
    void setAllSourcesVisible(bool visible);   // 隐藏 / 显示所有数据源
    bool allSourcesHidden() const;             // 当前是否所有数据源都是隐藏态
    void resetAllFilters();              // 「重置所有筛选」：回到全部数据原样显示
    void showTrackDetail(int index);     // 「详细信息」：高亮并在管理面板里搜索
    void deleteTrackFromMap(int index);  // 「删除该航迹」
    void onEscape();
    // 当前地图显示集合（RadarView displayTracks）的航迹数
    int displayedTrackCount() const;
    void applyDisplay();                 // 显示集合 / 筛选 / 点迹变化后同步到图层和回放范围
    void saveUiState();                  // 把界面状态（显隐/面板/相机/绘图）写进设置
    void restoreUiState();               // 启动时恢复上面这些状态
    void scheduleSaveUiState();          // 合并写入：连续改动只写一次

signals:
    void importFinished(bool ok, const QString &message);
    void persistFinished(bool ok, const QString &message);

private slots:
    void onIconClicked(int index);
    void onTrackContextMenu(int index, const QPoint &globalPos);
    void onFlagContextMenu(const QString &flagId, const QPoint &globalPos);

protected:
    void closeEvent(QCloseEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    void buildMenuBar();
    QStackedWidget *buildEditorArea();
    void loadPersisted();
    void saveCamera();                   // 相机视角写设置
    void updateUndoToast();
    void placeBackAllButton();
    void resetReplayRange();
    void refreshSources();                  // 状态栏数据源项与航迹数
    void reloadBatchLabels();               // 从数据库读取数据源显示名
    void updateHideAllSourcesAction();      // 按当前显隐状态切换「隐藏/显示所有数据源」文字
    QString sourceLabel(TrackSource s, const QString &fileName) const;   // 数据源显示名（重命名后用它）
    void toggleSource(const QString &key);
    void applyAdsbVisibility();
    void syncReplayToLayer();
    void onTracksImported(int beforeCount, bool firstData);
    const Point3dDataset *activeDiagDetDataset() const;
    void syncDiagDetDataset();
    void loadDetPointsForDiagnostics();

    ActivityBar   *m_activityBar = nullptr;
    SidePanel     *m_sidePanel = nullptr;
    QSplitter     *m_splitter = nullptr;
    QStackedWidget *m_editor = nullptr;
    GlobeWidget   *m_globe = nullptr;
    int m_panel = -1;
    QHash<int, int> m_panelWidth;         // 每个面板各自记住宽度（默认 280，随字号缩放）
    double m_uiScale = 1.0;               // 上次应用的字号缩放比例

    TrackStore     m_store;
    ReplayController *m_replay = nullptr;
    AppStatusBar  *m_statusBar = nullptr;   // 底部状态栏（逐项对应 RadarView StatusBar.vue）
    SourceStrip   *m_sourceStrip = nullptr; // 数据源条：横排 chip，紧贴状态栏上方
    bool           m_adsbVisible = true;    // ADS-B 整体显隐（雷达按文件单独控制）
    QPushButton   *m_backAllBtn = nullptr;   // 单独显示时地图顶部的「← 返回全部」

    ManageState *m_manage = nullptr;
    ManagePanel *m_managePanel = nullptr;
    TrackFilterState m_filter;
    FilterPanel *m_filterPanel = nullptr;
    FlagStore *m_flags = nullptr;
    RadarSiteStore *m_radarSites = nullptr;
    RulerState *m_ruler = nullptr;
    SpatialLasso *m_lasso = nullptr;
    FlagPanel *m_flagPanel = nullptr;
    SettingsPanel *m_settingsPanel = nullptr;
    DiagnosticsPanel *m_diagPanel = nullptr;
    trackdiag::TrackDiagBundle m_diagBundle;
    QPointer<Point3dWindow> m_diagDetWin;
    ui::UndoToast *m_undoToast = nullptr;

    DrawingController *m_drawCtrl = nullptr;
    MapSourceController *m_mapSources = nullptr;
    Point3dCoordinator *m_point3d = nullptr;
    TrackImportCoordinator *m_importCtrl = nullptr;

    QSet<QString> m_isoKeys;                // 单独显示 / 多选对比的航迹集合（按 key 记，删除航迹后下标会变）
    QSet<QString> m_dotKeys;                // 显示点迹的航迹
    QSet<QString> m_overviewDotKeys;        // 进入单条查看前的点迹选择
    bool m_haveOverviewDots = false;
    QAction *m_labelAction = nullptr;
    QAction *m_batchColorAction = nullptr;       // 视图 → 按航迹批号分色
    QAction *m_radarPpiAction = nullptr;         // 视图 → 显示雷达 PPI 网格
    QAction *m_hideAllSourcesAction = nullptr;   // 视图 → 隐藏所有数据源 / 显示所有数据源（文字随状态变）
    QAction *m_resetFiltersAction = nullptr;     // 视图 → 重置所有筛选
    QAction *m_clearDotsAction = nullptr;        // 视图 → 清空所有点迹显示
    RadarPointMode m_radarMode = RadarPointMode::Smooth;
    QPushButton *m_pointModeBtns[3] = { nullptr, nullptr, nullptr };
    QAction *m_pointModeActions[3] = { nullptr, nullptr, nullptr };
    QPushButton *m_filterOverlayBtn = nullptr;
    QAction *m_filterOverlayAction = nullptr;
    QAction *m_filterOverlaySub[3] = { nullptr, nullptr, nullptr };
    QPointer<TrackAnalysisWindow> m_analysisWin;
    QHash<QString, QString> m_batchLabels;       // 来源::文件名 → 数据源显示名（批量数据管理里重命名）

    QTimer *m_uiSaveTimer = nullptr;     // UI 状态合并保存
    bool m_restoringState = false;       // 恢复过程中不要回写
};

#endif // MAINWINDOW_H
