#ifndef POINT3DWINDOW_H
#define POINT3DWINDOW_H

#include <QElapsedTimer>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <QWidget>

#include <functional>

#include "point3dtypes.h"

class QAction;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSlider;
class QStackedWidget;
class QTimer;

class Point3dAdvancedPanel;
class Point3dCanvas;
class Point3dImportJob;
class Point3dSidePanel;

// 点迹三维可视化独立窗口（可多开；每个窗口承载一组点迹数据）
class Point3dWindow : public QWidget
{
    Q_OBJECT
public:
    explicit Point3dWindow(QWidget *parent = nullptr);
    ~Point3dWindow() override;

    void setNumber(int n);
    int number() const { return m_number; }

    // 窗口自带的回放：只回放本窗口的点迹/航迹，按帧间隔逐帧推进（不插值、不做时间换算）
    bool isPlaying() const { return m_playing; }
    void togglePlay();                 // 到头就从头播
    void setSpeed(double speed);       // 倍速（帧间隔 / 倍速 = 每帧停留的真实毫秒）
    double speed() const { return m_speed; }
    void seekToFrame(int index);       // 拖动定位：吸附到整帧
    int currentFrame() const;
    int frameCount() const { return m_data.frames.size(); }
    int frameHoldMs() const;           // 当前帧应停留的真实毫秒（自检用）
    // 可选「同步主时钟」锁（默认关闭，不破坏窗口原有独立回放）
    bool isSyncMasterClock() const { return m_syncMasterClock; }
    void setSyncMasterClock(bool on);
    void syncToMasterTime(qint64 masterTimeMs);

    // 航迹颜色提供者：与主窗口地图上的文件颜色保持一致
    void setTrackColorProvider(const std::function<QColor(TrackSource, const QString &)> &provider);
    // 数据源颜色在右键菜单里改了：按新的文件颜色重载叠加航迹（保持地图 ↔ 点迹窗口实时一致）
    void refreshOverlayColors();
    // 从数据库选择航迹（按 BatchNo 与主窗口联动）
    void chooseTracksFromDb();

    // 多窗口现场：保存 / 恢复本窗口的数据、航迹选择与几何
    QJsonObject sessionJson() const;
    void restoreSession(const QJsonObject &obj);

    Point3dCanvas *canvas() const { return m_canvas; }
    const Point3dDataset *dataset() const { return &m_data; }
    qint64 minTime() const { return m_data.minTime; }
    qint64 maxTime() const { return m_data.maxTime; }
    bool isLoading() const { return m_job != nullptr; }
    void cancelImport();
    void loadDetectionFiles(const QStringList &paths);
    QStringList importedFiles() const { return m_files; }

    void setHighlightedBatch(int batchNo);
    int highlightedBatch() const;

    // 区域筛选：每个三轴视图各存一套（切换视图时原样恢复）
    Point3dRegion regionForView(Point3dViewMode mode) const;
    bool regionAppliedForView(Point3dViewMode mode) const;
    // 自检/脚本用：设置当前视图的区域并按需应用
    bool setRegionForCurrentView(const Point3dRegion &r, bool apply);
    // 诊断面板一键缩放：把当前视图缩放到指定局部 (R, V, Az, Frame) 盒并切到末端帧
    void focusLocalBox(double rMin, double rMax, double vMin, double vMax,
                       double azMin, double azMax, int frameStart, int frameEnd);
    void setViewMode(Point3dViewMode mode);   // 同步工具栏下拉（会触发视图切换逻辑）

    // 地图联动：地图单独显示 / 管理面板筛选出的航迹 → 本窗口里有对应叠加航迹就单独显示它们；
    // 一条都对不上时本窗口保持原样（refs 为空则恢复显示全部叠加航迹）
    void setLinkedSelection(const QVector<LinkedTrackRef> &refs);
    // 地图悬停某条航迹 → 本窗口里对应的叠加航迹同时高亮；batchNo < 0 清除
    void setLinkedHighlight(int batchNo, qint64 t0, qint64 t1);
    QSet<int> isolatedBatches() const;
    void clearIsolation();
    void setCurrentFrame(int index);

signals:
    void closed();
    void dataChanged();
    void trackFocusChanged(int batchNo, TrackSource source, qint64 t0, qint64 t1);
    void trackActivated(int batchNo, TrackSource source, qint64 t0, qint64 t1);
    void isolationCleared();
    void frameTimeActivated(qint64 timeMs);   // 开启「同步主时钟」且用户手动切帧时发出

protected:
    void closeEvent(QCloseEvent *e) override;

private:
    void buildUi();
    void setSidePanelPage(int page, bool show);   // 0=设置 1=高级
    void syncViewMenuChecks(Point3dViewMode mode);
    void recomputeContinuityNow(const ContinuityConfig &cfg);
    void importDetections();
    void onImportDone();
    void exportCanvasImage();
    void setLoadingUi(bool loading, int percent);
    void updateStatus();
    void saveSettings();
    void setPlaying(bool on);
    void onPlayTick();
    void setFrameInternal(int index);
    void syncPlaybackUi();               // 把当前帧同步到进度条/时间标签
    void updatePlaybackUi();             // 播放/暂停按钮文字
    void reloadOverlaysFromKeys();
    void autoLoadMatchingTracks();
    void applyLinkedSelection();
    void toggleTrackHighlight(int batchNo, TrackSource source, qint64 t0, qint64 t1);
    void toggleTrackIsolation(int batchNo, TrackSource source, qint64 t0, qint64 t1);
    void updateBackAllButton();
    void startPendingLoad();

    Point3dCanvas *m_canvas = nullptr;
    QWidget *m_sidePanel = nullptr;      // 左侧停靠区（QStackedWidget：设置页 / 高级页）
    QStackedWidget *m_sideStack = nullptr;
    Point3dSidePanel *m_sideSettings = nullptr;
    Point3dAdvancedPanel *m_sideAdvanced = nullptr;
    QPushButton *m_settingsBtn = nullptr;   // 顶部「设置」按钮：切换设置侧栏
    QPushButton *m_advancedBtn = nullptr;   // 顶部「高级」按钮：切换高级参数侧栏
    QPushButton *m_backAllBtn = nullptr;    // 顶部「返回全部」按钮：仅在单独显示时出现
    QAction *m_viewActions[3] = { nullptr, nullptr, nullptr };
    QAction *m_importAction = nullptr;   // 文件 → 导入点迹（导入中禁用）
    QWidget *m_statusBar = nullptr;
    QLabel *m_status = nullptr;
    QProgressBar *m_progress = nullptr;

    // 本窗口自带的回放（独立于主窗口时间轴）
    QPushButton *m_playBtn = nullptr;
    QSlider *m_seekSlider = nullptr;
    QComboBox *m_speedCombo = nullptr;
    QPushButton *m_syncClockBtn = nullptr;
    QLabel *m_timeLabel = nullptr;
    QTimer *m_playTimer = nullptr;
    QElapsedTimer m_frameClock;          // 当前帧已经显示的真实时长
    double m_speed = 100.0;
    bool m_playing = false;
    bool m_resumeAfterSeek = false;
    bool m_syncing = false;
    bool m_syncMasterClock = false;
    bool m_applyingMasterTime = false;

    Point3dDataset m_data;
    Point3dImportJob *m_job = nullptr;
    QStringList m_pendingFiles;
    QStringList m_files;
    QStringList m_trackKeys;             // 已选航迹的 Track::key()（自动匹配或从数据库选择）
    QSet<QString> m_hiddenTrackKeys;     // 侧栏列表中取消勾选的航迹键
    bool m_manualTracksChosen = false;   // 用户手动在「从库选择…」确认过，不再自动覆盖
    int m_pinnedBatch = -1;              // 单击高亮锁定的批号（-1 = 未锁定）
    int m_skippedTracks = 0;             // 与点迹时间窗对不上、被忽略的航迹条数（一致性校验结果）
    QVector<LinkedTrackRef> m_linkedSelection;   // 地图当前的选中/筛选（重载数据后重新套用）
    std::function<QColor(TrackSource, const QString &)> m_colorProvider;
    int m_number = 1;
    bool m_restoring = false;
};

#endif // POINT3DWINDOW_H
