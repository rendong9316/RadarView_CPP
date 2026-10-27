#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QAbstractButton>
#include <QColor>
#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QPushButton>
#include <QSet>
#include <QWidget>

#include "drawingmanager.h"
#include "lucide.h"
#include "sidepanels.h"
#include "track.h"
#include "settingspanel.h"

class GlobeWidget;
class QLabel;
class QSplitter;
class QStackedWidget;
class TrackImporter;
class ReplayController;
class AppStatusBar;
class QDialog;
class QToolBar;
class QToolButton;
class QActionGroup;
class QUndoStack;
class ManageState;
class ManagePanel;
class FlagStore;
class RulerState;
class SpatialLasso;
class DrawingManager;
class DbSaveJob;
namespace ui { class UndoToast; }

// 侧栏面板（RadarView useActivityBar.ts PanelId）：轨迹面板 / 图层控制已移除
enum class PanelId { Manage = 0, Flags, TimeFilter, Settings, Count };

// 左侧活动栏（ActivityBar.vue）：48px 宽，5 个面板图标 + 底部「设置」，当前项左侧 2px 强调色竖条
class ActivityBar : public QWidget
{
    Q_OBJECT
public:
    explicit ActivityBar(QWidget *parent = nullptr);
    void highlight(int index);       // -1 表示都不高亮
    int activeIndex() const { return m_active; }

signals:
    void iconClicked(int index);

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    QList<QAbstractButton *> m_buttons;
    int m_active = -1;
};

// 侧栏（SideBar.vue）：32px 标题栏 + 关闭按钮，内容区内边距 8px
class SidePanel : public QWidget
{
    Q_OBJECT
public:
    explicit SidePanel(QWidget *parent = nullptr);
    void addPanel(QWidget *w);                    // 放进滚动区后加入
    void replacePanel(int index, QWidget *w);     // 替换某个面板（旧的延后销毁）
    void showPanel(int index, const QString &title);
    int currentPanel() const;
    QString title() const;

signals:
    void closeRequested();

private:
    QLabel *m_title;
    QStackedWidget *m_stack;
};

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
    bool isImporting() const { return m_importer != nullptr; }
    bool isPersisting() const { return m_saveJob != nullptr || !m_saveQueue.isEmpty(); }
    TrackStore *trackStore() { return &m_store; }
    ReplayController *replay() const { return m_replay; }

    // 单独显示某条航迹（TrackStore 下标），-1 返回全部；回放范围随之切换
    void isolateTrack(int index);
    int isolatedTrack() const;
    // 打开「查看点迹数据」窗口（非模态），返回窗口指针
    QDialog *showTrackPoints(int index);
    AppStatusBar *appStatusBar() const { return m_statusBar; }

    // ---- 侧栏 / 功能入口（菜单、快捷键、自检共用）----
    void activatePanel(PanelId id);      // 已打开同一面板时关闭侧栏
    void closeSidebar();
    int activePanel() const;             // -1 表示侧栏关闭
    ManageState *manageState() const { return m_manage; }
    ManagePanel *managePanel() const { return m_managePanel; }
    FilterPanel *filterPanel() const { return m_filterPanel; }
    FlagPanel *flagPanel() const { return m_flagPanel; }
    FlagStore *flagStore() const { return m_flags; }
    RulerState *ruler() const { return m_ruler; }
    SpatialLasso *spatialLasso() const { return m_lasso; }
    DrawingManager *drawing() const { return m_draw; }
    QUndoStack *undoStack() const { return m_undo; }
    void setDrawTool(DrawTool t);            // 绘图工具栏切换（Esc 也会回到漫游）
    // 绘图工具栏平时不显示：左上角「绘图」按钮点开才展开（两行按钮）
    void setDrawingBarVisible(bool on);
    bool drawingBarVisible() const;
    ui::UndoToast *undoToast() const { return m_undoToast; }
    SettingsPanel *settingsPanel() const { return m_settingsPanel; }
    void toggleLabels();
    void togglePointDots(int index);     // 「显示/隐藏所有对应点迹」
    bool pointDotsShown(int index) const;
    void showTrackDetail(int index);     // 「详细信息」：高亮并在管理面板里搜索
    void deleteTrackFromMap(int index);  // 「删除该航迹」
    void onEscape();
    // 当前地图显示集合（RadarView displayTracks）的航迹数
    int displayedTrackCount() const;
    void applyDisplay();                 // 显示集合 / 筛选 / 点迹变化后同步到图层和回放范围

signals:
    void importFinished(bool ok, const QString &message);
    void persistFinished(bool ok, const QString &message);

private slots:
    void onIconClicked(int index);
    void openTileFile();
    void onImportDone();
    void onTrackContextMenu(int index, const QPoint &globalPos);
    void onFlagContextMenu(const QString &flagId, const QPoint &globalPos);

private:
    void buildMenuBar();
    void buildDrawToolBar();
    void updateDrawColorIcon();
    void onDrawText(double lat, double lon, const QPoint &globalPos);
    void onDrawFill(int featureId);
    void onDrawFeatureEdit(int featureId);   // 选择工具双击：改颜色/线宽/填充/半径/文本
    void saveDrawings();
    void importDrawings();
    void drawDistanceCircle(const QString &flagId);   // 旗标右键「绘制距离圆」
    QStackedWidget *buildEditorArea();
    void loadPersisted();
    void startNextSave();
    void saveFileColors();
    void updateUndoToast();

    ActivityBar   *m_activityBar = nullptr;
    SidePanel     *m_sidePanel = nullptr;
    QSplitter     *m_splitter = nullptr;
    QStackedWidget *m_editor = nullptr;
    GlobeWidget   *m_globe = nullptr;
    int m_panel = -1;
    QHash<int, int> m_panelWidth;         // 每个面板各自记住宽度（默认 280，随字号缩放）
    double m_uiScale = 1.0;               // 上次应用的字号缩放比例

    TrackStore     m_store;
    TrackImporter *m_importer = nullptr;
    QColor m_importColor;
    QHash<QString, QColor> m_customFileColors;
    QList<QAction *> m_importActions;
    ReplayController *m_replay = nullptr;
    AppStatusBar  *m_statusBar = nullptr;   // 底部状态栏（逐项对应 RadarView StatusBar.vue）
    bool           m_adsbVisible = true;    // ADS-B 整体显隐（雷达按文件单独控制）
    QPushButton   *m_backAllBtn = nullptr;   // 单独显示时地图顶部的「← 返回全部」

    ManageState *m_manage = nullptr;
    ManagePanel *m_managePanel = nullptr;
    TrackFilterState m_filter;
    FilterPanel *m_filterPanel = nullptr;
    FlagStore *m_flags = nullptr;
    RulerState *m_ruler = nullptr;
    SpatialLasso *m_lasso = nullptr;
    DrawingManager *m_draw = nullptr;
    QUndoStack *m_undo = nullptr;            // 绘图撤销栈（最多 200 步）
    QToolBar *m_drawBar = nullptr;           // 绘图工具条（一行）
    QAction *m_drawToggle = nullptr;         // 标题栏「帮助」右边的「绘图」入口
    QActionGroup *m_drawGroup = nullptr;
    QAction *m_drawColorAction = nullptr;
    QAction *m_undoAction = nullptr;
    QAction *m_redoAction = nullptr;
    QAction *m_deleteDrawAction = nullptr;   // Delete：删除选中的图形
    QColor m_drawColor = QColor(0xf5, 0x9e, 0x0b);
    FlagPanel *m_flagPanel = nullptr;
    SettingsPanel *m_settingsPanel = nullptr;
    ui::UndoToast *m_undoToast = nullptr;
    QString m_isoKey;                       // 单独显示的航迹（按 key 记，删除航迹后下标会变）
    QSet<QString> m_dotKeys;                // 显示点迹的航迹
    QSet<QString> m_overviewDotKeys;        // 进入单条查看前的点迹选择
    bool m_haveOverviewDots = false;
    QAction *m_labelAction = nullptr;
    QAction *m_srcOffline = nullptr;
    QAction *m_srcTdt = nullptr;
    QAction *m_srcAmap = nullptr;
    QAction *m_srcAmapOffline = nullptr;

    DbSaveJob *m_saveJob = nullptr;
    QList<DbSaveJob *> m_saveQueue;

    bool eventFilter(QObject *obj, QEvent *e) override;
    void placeBackAllButton();
    void resetReplayRange();
    void refreshSources();                  // 状态栏数据源项与航迹数
    void toggleSource(const QString &key);
    void applyAdsbVisibility();

    void syncReplayToLayer();
};

#endif // MAINWINDOW_H
