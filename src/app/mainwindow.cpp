#include "mainwindow.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <functional>

#include "apppaths.h"
#include "appstatusbar.h"
#include "drawingcontroller.h"
#include "geocalc.h"
#include "globewidget.h"
#include "managepanel.h"
#include "mapsourcecontroller.h"
#include "maptools.h"
#include "point3dcoordinator.h"
#include "radarsite.h"
#include "radarsitedialog.h"
#include "replaycontroller.h"
#include "settingspanel.h"
#include "spatiallasso.h"
#include "theme.h"
#include "trackdb.h"
#include "trackimportcoordinator.h"
#include "trackanalysiswindow.h"
#include "tracklayer.h"
#include "trackpointdialog.h"
#include "uiwidgets.h"

namespace {

void centerGlobeOnTracks(GlobeWidget *globe, const TrackStore &store)
{
    if (store.size() <= 0)
        return;
    double sx = 0.0, sy = 0.0, sz = 0.0;
    for (const Track &t : store.tracks()) {
        const TrackPoint &p = t.points.first();
        const double la = qDegreesToRadians(p.lat), lo = qDegreesToRadians(p.lon);
        sx += std::cos(la) * std::cos(lo);
        sy += std::cos(la) * std::sin(lo);
        sz += std::sin(la);
    }
    globe->lookAt(qRadiansToDegrees(std::atan2(sy, sx)),
                  qRadiansToDegrees(std::atan2(sz, std::sqrt(sx * sx + sy * sy))), 6000.0);
}

} // namespace

// ---------------------------------------------------------------
//  主窗口
// ---------------------------------------------------------------
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("RadarView"));

    QWidget *central = new QWidget(this);
    central->setObjectName(QStringLiteral("central"));
    QVBoxLayout *v = new QVBoxLayout(central);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    QWidget *row = new QWidget(central);
    QHBoxLayout *h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);
    v->addWidget(row, 1);

    m_activityBar = new ActivityBar(row);
    m_splitter = new QSplitter(Qt::Horizontal, row);
    m_sidePanel = new SidePanel();
    m_editor = buildEditorArea();
    m_splitter->addWidget(m_sidePanel);
    m_splitter->addWidget(m_editor);
    m_splitter->setCollapsible(0, false);
    m_splitter->setCollapsible(1, false);
    m_splitter->setHandleWidth(4);
    setThemedStyle(m_splitter, QStringLiteral("QSplitter::handle { background: transparent; }"
                                              "QSplitter::handle:hover { background: var(--accent-primary); }"));
    h->addWidget(m_activityBar);
    h->addWidget(m_splitter, 1);
    setCentralWidget(central);
    setThemedStyle(central, QStringLiteral("#central { background: var(--editor-bg); }"));

    m_globe->setTrackStore(&m_store);
    m_flags = new FlagStore(this);
    m_radarSites = new RadarSiteStore(this);
    m_ruler = new RulerState(this);
    m_lasso = new SpatialLasso(this);
    m_globe->setMapTools(m_flags, m_ruler);
    m_globe->setRadarSites(m_radarSites);
    m_globe->setSpatialLasso(m_lasso);

    m_drawCtrl = new DrawingController(m_globe, m_lasso, m_ruler, m_flags, this, this);
    m_mapSources = new MapSourceController(m_globe, this, this);
    m_point3d = new Point3dCoordinator(&m_store, m_globe, this);
    m_importCtrl = new TrackImportCoordinator(&m_store, m_globe, this, this);

    m_replay = new ReplayController(this);
    m_manage = new ManageState(&m_store, this);

    // 侧栏面板（顺序同 PanelId）
    m_managePanel = new ManagePanel(m_manage);
    m_managePanel->rowColor = [this](const ManageRow &r) { return m_store.fileColor(r.source, r.fileName); };
    m_filterPanel = new FilterPanel(&m_filter);
    m_filterPanel->setSpatialLasso(m_lasso);
    m_flagPanel = new FlagPanel(m_flags, m_ruler);
    SettingsPanel::Host host;
    host.store = &m_store;
    host.globe = m_globe;
    host.manage = m_manage;
    host.replay = m_replay;
    m_settingsPanel = new SettingsPanel(host, this);
    connect(m_settingsPanel, &SettingsPanel::toggleLabelsRequested, this, &MainWindow::toggleLabels);
    connect(m_settingsPanel, &SettingsPanel::resetViewRequested, this, [this]() { m_globe->resetView(); });
    connect(m_settingsPanel, &SettingsPanel::clearDisplayRequested, this, [this]() { m_manage->clearVisible(); });
    connect(m_settingsPanel, &SettingsPanel::openManageRequested, this, [this]() { activatePanel(PanelId::Manage); });
    connect(m_settingsPanel, &SettingsPanel::colorModeChanged, this, [this](int m) {
        setTrackColorMode(m == 1 ? TrackLayer::TrackColorMode::ByTrack : TrackLayer::TrackColorMode::ByFile);
    });
    connect(m_manage, &ManageState::dataChanged, m_settingsPanel, &SettingsPanel::refresh);
    for (int i = 0; i < int(PanelId::Count); ++i) {
        QWidget *w = i == int(PanelId::Manage) ? static_cast<QWidget *>(m_managePanel)
                   : i == int(PanelId::Flags) ? static_cast<QWidget *>(m_flagPanel)
                   : i == int(PanelId::TimeFilter) ? static_cast<QWidget *>(m_filterPanel)
                   : i == int(PanelId::Settings) ? static_cast<QWidget *>(m_settingsPanel) : new QWidget;
        m_sidePanel->addPanel(w);
    }
    m_sidePanel->hide();
    connect(m_sidePanel, &SidePanel::closeRequested, this, &MainWindow::closeSidebar);
    connect(m_activityBar, &ActivityBar::iconClicked, this, &MainWindow::onIconClicked);
    connect(m_splitter, &QSplitter::splitterMoved, this, [this]() {
        if (m_panel >= 0)
            m_panelWidth[m_panel] = m_splitter->sizes().value(0);
        scheduleSaveUiState();
    });

    // 状态栏：常驻的自绘控件（不用 QStatusBar，它的 showMessage() 会把左侧控件整体隐藏）
    m_statusBar = new AppStatusBar(m_replay, central);
    // 数据源条：横向一排 chip，放在状态栏正上方（展开时压缩地图高度，不盖住地图）
    m_sourceStrip = new SourceStrip(central);
    m_sourceStrip->applyTheme(m_statusBar->theme());
    m_sourceStrip->hide();
    v->addWidget(m_sourceStrip);
    v->addWidget(m_statusBar);
    Theme::instance()->setId(m_statusBar->theme().id);
    connect(m_statusBar, &AppStatusBar::themeChanged, Theme::instance(), &Theme::setId);
    connect(m_globe, &GlobeWidget::cameraChanged, this, &MainWindow::saveCamera);
    connect(m_globe, &GlobeWidget::viewStatusChanged, m_statusBar,
            [this](double h, double lon, double lat, int fps, bool hasCoord) {
                m_statusBar->setViewStatus(h, lon, lat, fps, hasCoord);
                const RadarSite *site = m_radarSites ? m_radarSites->activeSite() : nullptr;
                double aLat = 0.0, aLon = 0.0;
                if (!site || !site->anchorLatLon(&aLat, &aLon)) {
                    m_statusBar->setRadarPolarText(QString(), QString(), false);
                    return;
                }
                if (!hasCoord) {
                    m_statusBar->setRadarPolarText(QStringLiteral("极 --"),
                                                   QStringLiteral("参考雷达站: %1 (%2)").arg(site->name, site->anchorLabel()),
                                                   true);
                    return;
                }
                const double rKm = geocalc::vincentyKm(aLat, aLon, lat, lon);
                const double az = geocalc::initialBearing(aLat, aLon, lat, lon);
                const QString text = QStringLiteral("极 %1km %2°").arg(rKm, 0, 'f', 1).arg(az, 0, 'f', 1);
                QStringList tips;
                tips << QStringLiteral("参考雷达站: %1（%2: %3, %4）")
                            .arg(site->name, site->anchorLabel())
                            .arg(aLat, 0, 'f', 4).arg(aLon, 0, 'f', 4);
                if (site->hasRx) {
                    const double rRx = geocalc::vincentyKm(site->rxLat, site->rxLon, lat, lon);
                    const double azRx = geocalc::initialBearing(site->rxLat, site->rxLon, lat, lon);
                    tips << QStringLiteral("相对接收站 Rx: %1 km / %2°").arg(rRx, 0, 'f', 2).arg(azRx, 0, 'f', 2);
                }
                if (site->hasTx) {
                    const double rTx = geocalc::vincentyKm(site->txLat, site->txLon, lat, lon);
                    const double azTx = geocalc::initialBearing(site->txLat, site->txLon, lat, lon);
                    tips << QStringLiteral("相对发射站 Tx: %1 km / %2°").arg(rTx, 0, 'f', 2).arg(azTx, 0, 'f', 2);
                }
                if (site->hasRx && site->hasTx) {
                    const double rRx = geocalc::vincentyKm(site->rxLat, site->rxLon, lat, lon);
                    const double rTx = geocalc::vincentyKm(site->txLat, site->txLon, lat, lon);
                    tips << QStringLiteral("双基地半程距离 (R_tx+R_rx)/2: %1 km").arg(0.5 * (rTx + rRx), 0, 'f', 2);
                }
                m_statusBar->setRadarPolarText(text, tips.join(QLatin1Char('\n')), true);
            });
    connect(m_statusBar, &AppStatusBar::sourceToggled, this, &MainWindow::toggleSource);
    connect(m_sourceStrip, &SourceStrip::sourceToggled, this, &MainWindow::toggleSource);
    connect(m_statusBar, &AppStatusBar::sourceStripToggleRequested, this, [this]() {
        setSourceStripOpen(m_sourceStrip->isHidden());
    });
    connect(m_statusBar, &AppStatusBar::themeChanged, this, [this](const QString &) {
        m_sourceStrip->applyTheme(m_statusBar->theme());
    });
    connect(m_replay, &ReplayController::timeChanged, this, &MainWindow::syncReplayToLayer);
    connect(m_replay, &ReplayController::stateChanged, this, &MainWindow::syncReplayToLayer);
    connect(m_replay, &ReplayController::trailSecondsChanged, this, [this](double sec) {
        m_globe->trackLayer()->setTrailSeconds(sec);
        m_settingsPanel->syncTrailSeconds(sec);
        m_globe->update();
    });
    m_globe->trackLayer()->setTrailSeconds(m_replay->trailSeconds());
    m_settingsPanel->syncTrailSeconds(m_replay->trailSeconds());
    connect(m_statusBar, &AppStatusBar::stepRequested, this, &MainWindow::stepReplay);

    // 协调器信号接入状态栏与主窗口联动
    connect(m_drawCtrl, &DrawingController::errorMessage, m_statusBar, &AppStatusBar::setError);
    connect(m_drawCtrl, &DrawingController::uiStateChanged, this, &MainWindow::scheduleSaveUiState);
    connect(m_point3d, &Point3dCoordinator::isolateTrackRequested, this, &MainWindow::isolateTrack);
    connect(m_point3d, &Point3dCoordinator::replayRangeInvalidated, this, &MainWindow::resetReplayRange);
    connect(m_point3d, &Point3dCoordinator::masterSeekRequested, m_replay, &ReplayController::seekTime);
    connect(m_importCtrl, &TrackImportCoordinator::loadingStateChanged, m_statusBar, &AppStatusBar::setLoading);
    connect(m_importCtrl, &TrackImportCoordinator::persistingStateChanged, m_statusBar, &AppStatusBar::setPersisting);
    connect(m_importCtrl, &TrackImportCoordinator::errorMessage, this, [this](const QString &msg, int clearAfterMs) {
        m_statusBar->setError(msg);
        if (clearAfterMs > 0 && !msg.isEmpty()) {
            QTimer::singleShot(clearAfterMs, this, [this, msg]() {
                if (m_statusBar->errorText() == msg)
                    m_statusBar->setError(QString());
            });
        }
    });
    connect(m_importCtrl, &TrackImportCoordinator::tracksImported, this, &MainWindow::onTracksImported);
    connect(m_importCtrl, &TrackImportCoordinator::importFinished, this, &MainWindow::importFinished);
    connect(m_importCtrl, &TrackImportCoordinator::persistFinished, this, &MainWindow::persistFinished);
    connect(m_importCtrl, &TrackImportCoordinator::dbStale, m_manage, &ManageState::markStale);
    connect(m_importCtrl, &TrackImportCoordinator::radarPointModeSelected, this, &MainWindow::setRadarPointMode);
    connect(m_importCtrl, &TrackImportCoordinator::radarSiteDiscovered, this, [this](const RadarSite &site) {
        if (m_radarSites)
            m_radarSites->upsertFromSysPara(site);
    });
    connect(m_radarSites, &RadarSiteStore::changed, this, [this]() {
        if (m_radarPpiAction) {
            QSignalBlocker blk(m_radarPpiAction);
            m_radarPpiAction->setChecked(m_radarSites->ppiVisible());
        }
        if (!m_radarSites->activeSite())
            m_statusBar->setRadarPolarText(QString(), QString(), false);
        m_globe->update();
    });

    buildMenuBar();
    m_drawCtrl->buildToolBar(this);
    setThemedStyle(menuBar(), ui::menuBarQss());

    // 地图交互：单击航迹单独显示，Ctrl+单击多选对比隔离，单击空白处返回全部；右键菜单；双击放旗标
    connect(m_globe, &GlobeWidget::trackClicked, this, [this](int index) {
        if (index >= 0)
            isolateTrack(index);
        else if (!m_isoKeys.isEmpty())
            isolateTrack(-1);
    });
    connect(m_globe, &GlobeWidget::trackMultiSelectClicked, this, [this](int index) {
        if (index >= 0)
            toggleIsolatedTrack(index);
    });
    connect(m_globe, &GlobeWidget::trackContextMenuRequested, this, &MainWindow::onTrackContextMenu);
    // 地图悬停某条航迹 → 所有点迹窗口里的同一条航迹同步高亮
    connect(m_globe, &GlobeWidget::trackHovered, m_point3d, &Point3dCoordinator::onMapHoverTrack);
    connect(m_globe, &GlobeWidget::flagContextMenuRequested, this, &MainWindow::onFlagContextMenu);
    m_backAllBtn = new QPushButton(tr("← 返回全部"), m_globe);
    m_backAllBtn->setToolTip(tr("返回查看全部航迹（Esc）"));
    m_backAllBtn->setCursor(Qt::PointingHandCursor);
    setThemedStyle(m_backAllBtn, QStringLiteral(
        "QPushButton { background: var(--accent-primary); color: var(--accent-fg); border: 1px solid var(--accent-primary);"
        " border-radius: 4px; padding: 5px 14px; font-family: %1; font-size: 12px; font-weight: 600; }"
        "QPushButton:hover { background: var(--accent-hover); border-color: var(--accent-hover); }")
        .arg(ui::uiFamilies()));
    m_backAllBtn->hide();
    connect(m_backAllBtn, &QPushButton::clicked, this, [this]() { isolateTrack(-1); });
    m_globe->installEventFilter(this);

    // 管理面板 / 筛选 → 地图显示集合
    connect(m_manage, &ManageState::aboutToChangeStore, m_globe, &GlobeWidget::resetInteraction);
    connect(m_manage, &ManageState::storeChanged, this, [this]() {
        refreshSources();
        m_filterPanel->setDataRange(m_store.minTime(), m_store.maxTime());
        applyDisplay();
        if (m_analysisWin)
            m_analysisWin->refreshTracks();
    });
    connect(m_manage, &ManageState::displayChanged, this, &MainWindow::applyDisplay);
    connect(m_managePanel, &ManagePanel::viewPointsRequested, this, [this](const QString &key) {
        showTrackPoints(m_store.indexOf(key));
    });
    // 管理面板里点「空间查询 ✕」→ 清掉套索与空间查询条件（与筛选面板的「清除套索」同一条路径）
    connect(m_managePanel, &ManagePanel::spatialQueryClearRequested, this, [this]() {
        m_filterPanel->clickSpatialClear();
    });
    // 批量数据管理里重命名了数据源：状态栏 chip / 管理系统的批次下拉都要换成新名字
    connect(m_managePanel, &ManagePanel::batchLabelsChanged, this, [this]() {
        reloadBatchLabels();
        refreshSources();
        m_managePanel->refresh();
        applyDisplay();
    });
    connect(m_filterPanel, &FilterPanel::changed, this, &MainWindow::applyDisplay);
    // 套索闭合 → 写入筛选状态并立即生效（只显示经过该区域的航迹）
    connect(m_lasso, &SpatialLasso::closed, this, [this]() {
        m_filter.spatialPolygon = m_lasso->vertices();
        m_filter.spatialActive = m_lasso->vertices().size() >= 3;
        m_filter.save();
        m_filterPanel->refreshUi();
        applyDisplay();
    });
    m_undoToast = new ui::UndoToast(central);
    connect(m_undoToast, &ui::UndoToast::undoClicked, m_manage, &ManageState::undoDelete);
    connect(m_manage, &ManageState::undoChanged, this, &MainWindow::updateUndoToast);

    // 快捷键（App.vue keydown）
    auto shortcut = [this](const QKeySequence &seq, std::function<void()> fn) {
        QAction *a = new QAction(this);
        a->setShortcut(seq);
        a->setShortcutContext(Qt::WindowShortcut);
        connect(a, &QAction::triggered, this, fn);
        addAction(a);
    };
    shortcut(Qt::Key_Space, [this]() { m_replay->togglePlay(); });
    shortcut(Qt::Key_Left, [this]() { stepReplay(-1); });
    shortcut(Qt::Key_Right, [this]() { stepReplay(+1); });
    shortcut(QKeySequence(Qt::SHIFT + Qt::Key_Left), [this]() { stepReplayByMs(-10000); });
    shortcut(QKeySequence(Qt::SHIFT + Qt::Key_Right), [this]() { stepReplayByMs(10000); });
    shortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_R), [this]() { m_ruler->toggle(); });

    // 全局字号变化：侧栏宽度按比例跟随，地图顶部按钮重新居中
    m_uiScale = ui::fontScale();
    connect(UiScale::instance(), &UiScale::changed, this, [this]() {
        const double f = ui::fontScale() / m_uiScale;
        m_uiScale = ui::fontScale();
        for (auto it = m_panelWidth.begin(); it != m_panelWidth.end(); ++it)
            it.value() = int(it.value() * f);
        if (m_panel >= 0 && !m_sidePanel->isHidden()) {
            const int total = m_splitter->width();
            const int minW = m_sidePanel->minimumWidth();
            const int w = qBound(minW, int(m_splitter->sizes().value(0) * f), qMax(minW, total - 200));
            m_splitter->setSizes(QList<int>() << w << total - w);
            m_panelWidth[m_panel] = w;
        }
        m_backAllBtn->adjustSize();
        placeBackAllButton();
    });

    // UI 状态合并保存：拖分栏、连续切显隐时不会每次都写盘
    m_uiSaveTimer = new QTimer(this);
    m_uiSaveTimer->setSingleShot(true);
    m_uiSaveTimer->setInterval(400);
    connect(m_uiSaveTimer, &QTimer::timeout, this, &MainWindow::saveUiState);
    // 兜底：不经过 closeEvent 的退出（QApplication::quit 等）也要把状态写完
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() {
        m_point3d->closeAllAndSave();
        saveUiState();
        m_drawCtrl->saveAuto();
        app::flushSettings();
    });

    // 先恢复窗口几何，侧栏宽度换算才有正确的总宽度
    const QByteArray geo = app::settings().value(QStringLiteral("window.geometry")).toByteArray();
    if (geo.isEmpty() || !restoreGeometry(geo))
        resize(1280, 800);
    loadPersisted();
}

MainWindow::~MainWindow()
{
    m_importCtrl->waitAndShutdown();
    trackdb::close();
}

// 启动：打开数据库并加载已保存的航迹（RadarView load_persisted_tracks），恢复各项设置
void MainWindow::loadPersisted()
{
    m_restoringState = true;
    m_drawCtrl->setRestoringState(true);
    app::Settings &s = app::settings();
    m_importCtrl->loadFileColors();

    QString err;
    if (trackdb::open(&err)) {
        const QVector<Track> saved = trackdb::loadAll(&err);
        if (!saved.isEmpty()) {
            m_store.addTracks(saved);
            m_importCtrl->saveFileColors();
        }
        reloadBatchLabels();        // 批量数据管理里改过的数据源显示名
    } else {
        m_statusBar->setError(QStringLiteral("数据库打开失败: %1").arg(err));
    }
    m_manage->loadSettings();
    m_filter.load();
    m_flags->load();
    m_radarSites->load();
    if (m_radarPpiAction)
        m_radarPpiAction->setChecked(m_radarSites->ppiVisible());
    m_ruler->load();
    m_globe->setShowLabels(s.value(QStringLiteral("display.show_labels"), false).toBool());
    if (m_labelAction)
        m_labelAction->setChecked(m_globe->showLabels());
    // 筛选面板控件按已恢复的筛选状态重建
    FilterPanel *fresh = new FilterPanel(&m_filter);
    connect(fresh, &FilterPanel::changed, this, &MainWindow::applyDisplay);
    fresh->setSpatialLasso(m_lasso);
    m_sidePanel->replacePanel(int(PanelId::TimeFilter), fresh);   // 旧面板由 replacePanel 延后销毁
    m_filterPanel = fresh;
    m_filterPanel->setDataRange(m_store.minTime(), m_store.maxTime());
    m_managePanel->refresh();

    // ---- 显示/隐藏状态：雷达点列模式、数据源整体显隐、状态栏里点掉的雷达文件、点迹、单独显示 ----
    {
        RadarPointMode savedMode = RadarPointMode::Smooth;
        radarPointModeFromKey(s.value(QStringLiteral("display.radar_point_mode"),
                                      QStringLiteral("smooth")).toString(), &savedMode);
        setRadarPointMode(savedMode);
    }
    m_adsbVisible = s.value(QStringLiteral("display.adsb_visible"), true).toBool();
    {
        TrackLayer *layer = m_globe->trackLayer();
        for (const QJsonValue &v : QJsonDocument::fromJson(
                 s.value(QStringLiteral("display.hidden_groups")).toString().toUtf8()).array()) {
            const QString key = v.toString();
            if (!key.isEmpty())
                layer->setGroupVisible(key, false);
        }
        for (const QJsonValue &v : QJsonDocument::fromJson(
                 s.value(QStringLiteral("display.dot_keys")).toString().toUtf8()).array()) {
            const QString key = v.toString();
            if (!key.isEmpty() && m_store.indexOf(key) >= 0)
                m_dotKeys.insert(key);
        }
    }
    {
        const int colorMode = s.value(QStringLiteral("display.color_mode"), 0).toInt();
        setTrackColorMode(colorMode == 1 ? TrackLayer::TrackColorMode::ByTrack : TrackLayer::TrackColorMode::ByFile);
    }
    {
        const QJsonArray isoArr = QJsonDocument::fromJson(
            s.value(QStringLiteral("track.isolated_keys")).toString().toUtf8()).array();
        for (const QJsonValue &v : isoArr) {
            const QString k = v.toString();
            if (!k.isEmpty() && m_store.indexOf(k) >= 0)
                m_isoKeys.insert(k);
        }
        if (m_isoKeys.isEmpty()) {
            const QString isoKey = s.value(QStringLiteral("track.isolated_key")).toString();
            if (!isoKey.isEmpty() && m_store.indexOf(isoKey) >= 0)
                m_isoKeys.insert(isoKey);
        }
    }

    applyAdsbVisibility();
    refreshSources();
    applyDisplay();
    centerGlobeOnTracks(m_globe, m_store);
    // 上次退出时的视角优先于「自动定位到数据中心」
    restoreUiState();

    // 恢复上次打开的点迹窗口（延到事件循环开始后，避免构造期弹窗/读文件）
    QTimer::singleShot(0, this, [this]() { m_point3d->restoreSessions(); });
    m_restoringState = false;
    m_drawCtrl->setRestoringState(false);
}

// ---------------------------------------------------------------
//  界面状态持久化：显隐 / 点迹 / 单独显示 / 侧栏 / 绘图栏 / 相机 / 绘图内容
// ---------------------------------------------------------------
void MainWindow::scheduleSaveUiState()
{
    if (m_restoringState || !m_uiSaveTimer)
        return;
    m_uiSaveTimer->start();
}

void MainWindow::saveUiState()
{
    if (m_restoringState)
        return;
    app::setValue(QStringLiteral("display.adsb_visible"), m_adsbVisible);
    app::setValue(QStringLiteral("display.show_labels"), m_globe->showLabels());
    app::setValue(QStringLiteral("display.radar_point_mode"), radarPointModeKey(m_radarMode));

    // 状态栏里被点掉的雷达文件（ADS-B 是整体开关，单独存在上面的键里）
    TrackLayer *layer = m_globe->trackLayer();
    QSet<QString> groups;
    for (const Track &t : m_store.tracks())
        groups.insert(trackSourceName(t.source) + QStringLiteral("::") + t.fileName);
    QStringList sorted = groups.values();
    sorted.sort();
    QJsonArray hidden;
    for (const QString &key : sorted) {
        if (key.startsWith(QLatin1String("ADS-B::")))
            continue;
        if (!layer->isGroupVisible(key))
            hidden.append(key);
    }
    app::setValue(QStringLiteral("display.hidden_groups"),
                  QString::fromUtf8(QJsonDocument(hidden).toJson(QJsonDocument::Compact)));

    QStringList dots = m_dotKeys.values();
    dots.sort();
    app::setValue(QStringLiteral("display.dot_keys"),
                  QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(dots)).toJson(QJsonDocument::Compact)));

    app::setValue(QStringLiteral("display.color_mode"), int(trackColorMode()));
    const QString singleIso = m_isoKeys.size() == 1 ? *m_isoKeys.constBegin() : QString();
    app::setValue(QStringLiteral("track.isolated_key"), singleIso);
    QStringList isoList = m_isoKeys.values();
    isoList.sort();
    app::setValue(QStringLiteral("track.isolated_keys"),
                  QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(isoList)).toJson(QJsonDocument::Compact)));

    app::setValue(QStringLiteral("panel.active"), m_panel);
    QJsonObject widths;
    for (auto it = m_panelWidth.constBegin(); it != m_panelWidth.constEnd(); ++it)
        widths.insert(QString::number(it.key()), it.value());
    app::setValue(QStringLiteral("panel.widths"),
                  QString::fromUtf8(QJsonDocument(widths).toJson(QJsonDocument::Compact)));
    app::setValue(QStringLiteral("panel.drawbar_visible"), drawingBarVisible());
    // 用 isHidden() 而不是 isVisible()：窗口还没显示（自检）时 isVisible() 恒为 false
    app::setValue(QStringLiteral("statusbar.sources_open"), m_sourceStrip && !m_sourceStrip->isHidden());
    app::setValue(QStringLiteral("window.geometry"), saveGeometry());
}

void MainWindow::restoreUiState()
{
    app::Settings &s = app::settings();
    // 相机视角
    const QJsonObject cam = QJsonDocument::fromJson(
        s.value(QStringLiteral("camera.state")).toString().toUtf8()).object();
    if (cam.contains(QStringLiteral("lon")) && cam.contains(QStringLiteral("lat"))) {
        const double alt = cam.value(QStringLiteral("alt")).toDouble(6000.0);
        m_globe->setCameraView(cam.value(QStringLiteral("lon")).toDouble(),
                               cam.value(QStringLiteral("lat")).toDouble(),
                               alt > 0.0 ? alt : 6000.0);
    }
    // 绘图颜色 + 绘图内容
    const QColor dc(s.value(QStringLiteral("drawing.color")).toString());
    if (dc.isValid())
        m_drawCtrl->setDrawColor(dc);
    m_drawCtrl->loadAuto();
    // 绘图工具条是否展开
    setDrawingBarVisible(s.value(QStringLiteral("panel.drawbar_visible"), false).toBool());
    // 数据源条是否展开
    setSourceStripOpen(s.value(QStringLiteral("statusbar.sources_open"), false).toBool());
    // 侧栏：先恢复各面板宽度，再打开上次的面板
    const QJsonObject widths = QJsonDocument::fromJson(
        s.value(QStringLiteral("panel.widths")).toString().toUtf8()).object();
    for (auto it = widths.begin(); it != widths.end(); ++it) {
        const int id = it.key().toInt();
        if (id >= 0 && id < int(PanelId::Count) && it.value().toInt() > 0)
            m_panelWidth[id] = it.value().toInt();
    }
    const int panel = s.value(QStringLiteral("panel.active"), -1).toInt();
    if (panel >= 0 && panel < int(PanelId::Count))
        activatePanel(PanelId(panel));
}

void MainWindow::saveCamera()
{
    if (m_restoringState)
        return;
    double lon = 0.0, lat = 0.0, alt = 0.0;
    m_globe->cameraView(&lon, &lat, &alt);
    QJsonObject o;
    o.insert(QStringLiteral("lon"), lon);
    o.insert(QStringLiteral("lat"), lat);
    o.insert(QStringLiteral("alt"), alt);
    app::setValue(QStringLiteral("camera.state"),
                  QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    m_point3d->closeAllAndSave();
    saveUiState();
    m_drawCtrl->saveAuto();
    app::flushSettings();
    QMainWindow::closeEvent(e);
}

// ---------------------------------------------------------------
//  子控制器委托入口（保持 MainWindow 公开接口 100% 不变）
// ---------------------------------------------------------------
bool MainWindow::openTiles(const QString &path) { return m_mapSources->openTiles(path); }
void MainWindow::loadDefaultTiles() { m_mapSources->loadDefaultTiles(); }
void MainWindow::setMapSource(const QString &id) { m_mapSources->setMapSource(id); }
bool MainWindow::openOnlineMap(const QString &token, bool useAmap) { return m_mapSources->openOnlineMap(token, useAmap); }
void MainWindow::syncSourceActions() { m_mapSources->syncSourceActions(); }

void MainWindow::importFile(TrackSource kind, const QString &path, const QString &displayName, const QColor &color)
{
    m_importCtrl->importFile(kind, path, displayName, color);
}
bool MainWindow::isImporting() const { return m_importCtrl->isImporting(); }
bool MainWindow::isPersisting() const { return m_importCtrl->isPersisting(); }

DrawingManager *MainWindow::drawing() const { return m_drawCtrl->drawing(); }
QUndoStack *MainWindow::undoStack() const { return m_drawCtrl->undoStack(); }
void MainWindow::setDrawTool(DrawTool t) { m_drawCtrl->setDrawTool(t); }
void MainWindow::setDrawingBarVisible(bool on) { m_drawCtrl->setBarVisible(on); }
bool MainWindow::drawingBarVisible() const { return m_drawCtrl->isBarVisible(); }

void MainWindow::showPoint3dWindow() { m_point3d->showNewWindow(); }
QList<Point3dWindow *> MainWindow::point3dWindows() const { return m_point3d->windows(); }

void MainWindow::onTracksImported(int beforeCount, bool firstData)
{
    // 管理面板有可见集合时，新导入的航迹也加入（RadarView revealImportedTracks）
    if (!m_isoKeys.isEmpty())
        isolateTrack(-1);
    if (!m_manage->visibleKeys().isEmpty()) {
        QStringList keys;
        for (int i = beforeCount; i < m_store.size(); ++i)
            keys << m_store.tracks()[i].key();
        m_manage->showAll(keys);
    }
    applyAdsbVisibility();
    refreshSources();
    m_filterPanel->setDataRange(m_store.minTime(), m_store.maxTime());
    applyDisplay();
    // 第一次导入时把视角移到数据中心
    if (firstData && m_store.size() > beforeCount)
        centerGlobeOnTracks(m_globe, m_store);
    if (m_analysisWin)
        m_analysisWin->refreshTracks();
    m_globe->update();
}

// ---------------------------------------------------------------
//  航迹显示集合 / 回放范围 / 数据源显隐
// ---------------------------------------------------------------
int MainWindow::isolatedTrack() const
{
    if (m_isoKeys.size() != 1)
        return -1;
    return m_store.indexOf(*m_isoKeys.constBegin());
}

QSet<int> MainWindow::isolatedTracks() const
{
    QSet<int> out;
    for (const QString &k : qAsConst(m_isoKeys)) {
        const int idx = m_store.indexOf(k);
        if (idx >= 0)
            out.insert(idx);
    }
    return out;
}

// 回放范围跟随地图显示集合（RadarView useReplay(displayTracks)），时间筛选时取窗口内的部分
void MainWindow::resetReplayRange()
{
    qint64 lo = 0, hi = 0;
    bool any = false;
    TrackLayer *layer = m_globe->trackLayer();
    const bool timeClip = m_manage->visibleKeys().isEmpty() && isolatedTracks().isEmpty() && m_filter.timeActive;
    for (int i = 0; i < m_store.size(); ++i) {
        if (!layer->isTrackShownIgnoringGroups(i))
            continue;
        const Track &tr = m_store.tracks()[i];
        const QVector<TrackPoint> &pts = tr.pointsForMode(m_radarMode);
        if (pts.isEmpty())
            continue;
        qint64 a = pts.first().t, b = pts.last().t;
        if (timeClip) {
            a = qMax(a, m_filter.timeMin);
            b = qMin(b, m_filter.timeMax);
        }
        if (!any || a < lo)
            lo = a;
        if (!any || b > hi)
            hi = b;
        any = true;
    }
    // 点迹窗口有自己的独立回放，不再并入主窗口的时间轴
    m_replay->setRange(any ? lo : 0, any ? hi : 0);
}

// 地图显示集合（App.vue displayTracks）：管理面板可见集合 > 单独显示 > 筛选结果；软删除的已从内存移除
void MainWindow::applyDisplay()
{
    TrackLayer *layer = m_globe->trackLayer();
    const QSet<QString> &vis = m_manage->visibleKeys();
    // 清理已不存在于内存中的孤立键
    for (auto it = m_isoKeys.begin(); it != m_isoKeys.end();) {
        if (m_store.indexOf(*it) < 0)
            it = m_isoKeys.erase(it);
        else
            ++it;
    }
    const QSet<int> isoSet = isolatedTracks();
    if (isoSet.isEmpty()) {
        if (m_haveOverviewDots) {
            m_dotKeys = m_overviewDotKeys;
            m_overviewDotKeys.clear();
            m_haveOverviewDots = false;
        }
    }

    // 空间套索 → 航迹管理系统的查询范围（查询条件，不是可见集合）
    {
        const bool spatial = m_filter.hasSpatialFilter();
        QStringList spatialKeys;
        if (spatial) {
            spatialKeys.reserve(m_store.size());
            for (int i = 0; i < m_store.size(); ++i) {
                const Track &t = m_store.tracks()[i];
                if (lassogeo::trackIntersectsPolygon(t, m_filter.spatialPolygon))
                    spatialKeys << t.key();
            }
        }
        m_manage->setSpatialQuery(spatialKeys, spatial);   // 无变化时内部直接返回
    }
    QVector<bool> mask;
    if (!vis.isEmpty()) {
        mask.resize(m_store.size());
        for (int i = 0; i < m_store.size(); ++i)
            mask[i] = vis.contains(m_store.tracks()[i].key());
        layer->setIsolatedTracks(QSet<int>());
        layer->setTimeFilter(false, 0, 0);
    } else if (!isoSet.isEmpty()) {
        layer->setIsolatedTracks(isoSet);
        layer->setTimeFilter(false, 0, 0);
    } else {
        layer->setIsolatedTracks(QSet<int>());
        if (m_filter.timeActive || m_filter.hasPointCountFilter() || m_filter.hasSpatialFilter()) {
            mask.resize(m_store.size());
            for (int i = 0; i < m_store.size(); ++i)
                mask[i] = m_filter.accepts(m_store.tracks()[i]);
        }
        layer->setTimeFilter(m_filter.timeActive, m_filter.timeMin, m_filter.timeMax);
    }
    layer->setTrackMask(mask);
    // 单独显示 / 多选对比提示按钮只在隔离真正生效时出现
    const bool isoShown = vis.isEmpty() && !isoSet.isEmpty();
    m_backAllBtn->setVisible(isoShown);
    if (isoShown) {
        if (isoSet.size() == 1) {
            const Track &t = m_store.tracks()[*isoSet.constBegin()];
            m_backAllBtn->setText(tr("← 返回全部（单独显示：%1）").arg(t.flightNo.isEmpty() ? t.id : t.flightNo));
        } else {
            m_backAllBtn->setText(tr("← 返回全部（多选对比：%1 条，Ctrl+点击增减）").arg(isoSet.size()));
        }
        m_backAllBtn->adjustSize();
        placeBackAllButton();
        m_backAllBtn->raise();
    }
    // 点迹：删掉已不存在的航迹
    QVector<int> dots;
    for (auto it = m_dotKeys.begin(); it != m_dotKeys.end();) {
        const int idx = m_store.indexOf(*it);
        if (idx < 0) {
            it = m_dotKeys.erase(it);
        } else {
            dots << idx;
            ++it;
        }
    }
    std::sort(dots.begin(), dots.end());
    layer->setPointDotTracks(dots);

    m_point3d->syncLinkedSelection(vis, isoSet);
    resetReplayRange();
    m_globe->update();
}

int MainWindow::displayedTrackCount() const
{
    int n = 0;
    for (int i = 0; i < m_store.size(); ++i)
        if (m_globe->trackLayer()->isTrackShownIgnoringGroups(i))
            ++n;
    return n;
}

void MainWindow::isolateTrack(int index)
{
    if (index >= m_store.size())
        index = -1;
    QSet<QString> next;
    if (index >= 0)
        next.insert(m_store.tracks()[index].key());
    if (next == m_isoKeys)
        return;
    if (!next.isEmpty() && m_isoKeys.isEmpty()) {
        m_overviewDotKeys = m_dotKeys;
        m_haveOverviewDots = true;
    } else if (next.isEmpty() && m_haveOverviewDots) {
        m_dotKeys = m_overviewDotKeys;
        m_overviewDotKeys.clear();
        m_haveOverviewDots = false;
    }
    m_isoKeys = next;
    applyDisplay();
    scheduleSaveUiState();
}

void MainWindow::toggleIsolatedTrack(int index)
{
    if (index < 0 || index >= m_store.size())
        return;
    const QString key = m_store.tracks()[index].key();
    const bool wasEmpty = m_isoKeys.isEmpty();
    if (m_isoKeys.contains(key))
        m_isoKeys.remove(key);
    else
        m_isoKeys.insert(key);
    if (!m_isoKeys.isEmpty() && wasEmpty) {
        m_overviewDotKeys = m_dotKeys;
        m_haveOverviewDots = true;
    } else if (m_isoKeys.isEmpty() && m_haveOverviewDots) {
        m_dotKeys = m_overviewDotKeys;
        m_overviewDotKeys.clear();
        m_haveOverviewDots = false;
    }
    applyDisplay();
    scheduleSaveUiState();
}

void MainWindow::zoomToTrack(int index)
{
    if (index < 0 || index >= m_store.size())
        return;
    const QVector<TrackPoint> &pts = m_store.tracks()[index].pointsForMode(m_radarMode);
    if (pts.isEmpty())
        return;
    double minLon = pts.first().lon, maxLon = minLon;
    double minLat = pts.first().lat, maxLat = minLat;
    for (const TrackPoint &p : pts) {
        minLon = qMin(minLon, p.lon);
        maxLon = qMax(maxLon, p.lon);
        minLat = qMin(minLat, p.lat);
        maxLat = qMax(maxLat, p.lat);
    }
    m_globe->fitGeoBounds(minLon, maxLon, minLat, maxLat);
}

void MainWindow::setTrackColorMode(TrackLayer::TrackColorMode mode)
{
    m_globe->trackLayer()->setColorMode(mode);
    if (m_batchColorAction) {
        QSignalBlocker blk(m_batchColorAction);
        m_batchColorAction->setChecked(mode == TrackLayer::TrackColorMode::ByTrack);
    }
    if (m_settingsPanel)
        m_settingsPanel->syncColorMode(int(mode));
    m_globe->update();
    scheduleSaveUiState();
}

TrackLayer::TrackColorMode MainWindow::trackColorMode() const
{
    return m_globe->trackLayer()->colorMode();
}

void MainWindow::stepReplay(int dir)
{
    if (m_replay->end() <= m_replay->start() || dir == 0)
        return;
    QVector<qint64> times;
    TrackLayer *layer = m_globe->trackLayer();
    const QSet<int> iso = isolatedTracks();
    // 优先使用隔离/多选航迹的采样时间戳；否则采集当前显示航迹的去重时间戳
    for (int i = 0; i < m_store.size(); ++i) {
        if (!iso.isEmpty() ? !iso.contains(i) : !layer->isTrackShown(i))
            continue;
        const QVector<TrackPoint> &pts = m_store.tracks()[i].pointsForMode(m_radarMode);
        for (const TrackPoint &p : pts)
            if (p.t >= m_replay->start() && p.t <= m_replay->end())
                times.append(p.t);
        if (times.size() > 100000)
            break;
    }
    if (times.isEmpty()) {
        m_replay->stepByMs(dir > 0 ? 1000 : -1000);
        return;
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    m_replay->stepToTimestamp(times, dir);
}

void MainWindow::stepReplayByMs(qint64 deltaMs)
{
    m_replay->stepByMs(deltaMs);
}

// 与 RadarView statusSources 相同：ADS-B 一项（整体），雷达 / 原始量测按导入文件各一项（按首次出现顺序）
void MainWindow::refreshSources()
{
    QVector<AppStatusBar::SourceItem> items;
    AppStatusBar::SourceItem adsb;
    adsb.key = QStringLiteral("adsb");
    adsb.label = QStringLiteral("ADS-B");
    adsb.color = trackSourceColor(TrackSource::Adsb);
    adsb.visible = m_adsbVisible;
    QVector<AppStatusBar::SourceItem> radar, raw;
    TrackLayer *layer = m_globe->trackLayer();
    for (const Track &t : m_store.tracks()) {
        if (t.source == TrackSource::Adsb) {
            ++adsb.count;
            continue;
        }
        QVector<AppStatusBar::SourceItem> &list = t.source == TrackSource::Radar ? radar : raw;
        const QString key = trackSourceName(t.source) + QStringLiteral("::") + t.fileName;
        int i = 0;
        while (i < list.size() && list[i].key != key)
            ++i;
        if (i == list.size()) {
            AppStatusBar::SourceItem it;
            it.key = key;
            it.label = sourceLabel(t.source, t.fileName);
            it.color = m_store.fileColor(t.source, t.fileName);
            it.visible = layer->isGroupVisible(key);
            list.append(it);
        }
        ++list[i].count;
    }
    items << adsb << radar << raw;
    m_statusBar->setSources(items);
    m_sourceStrip->setItems(items);
    m_statusBar->setTrackCount(m_store.size());
    updateHideAllSourcesAction();
    if (m_filterPanel)
        m_filterPanel->setShowRawSource(!raw.isEmpty());
    if (m_settingsPanel)
        m_settingsPanel->refresh();
}

// 数据源显示名：批量数据管理里可以重命名；file_name 仍是唯一标识
QString MainWindow::sourceLabel(TrackSource s, const QString &fileName) const
{
    const QString key = trackSourceName(s) + QStringLiteral("::") + fileName;
    const auto it = m_batchLabels.constFind(key);
    return it == m_batchLabels.constEnd() || it.value().isEmpty() ? fileName : it.value();
}

void MainWindow::reloadBatchLabels()
{
    m_batchLabels = trackdb::batchDisplayNames();
    updateHideAllSourcesAction();
}

// 「隐藏所有数据源」按钮的文字由真实显隐状态推导：全部隐藏时变成「显示所有数据源」
bool MainWindow::allSourcesHidden() const
{
    if (m_store.size() == 0)
        return false;
    TrackLayer *layer = m_globe->trackLayer();
    for (const Track &t : m_store.tracks()) {
        if (t.source == TrackSource::Adsb) {
            if (m_adsbVisible)
                return false;           // 还有可见的 ADS-B 数据
            continue;
        }
        if (layer->isGroupVisible(trackSourceName(t.source) + QStringLiteral("::") + t.fileName))
            return false;
    }
    return true;
}

void MainWindow::updateHideAllSourcesAction()
{
    if (!m_hideAllSourcesAction)
        return;
    m_hideAllSourcesAction->setEnabled(m_store.size() > 0);
    m_hideAllSourcesAction->setText(allSourcesHidden() ? tr("显示所有数据源") : tr("隐藏所有数据源"));
}

void MainWindow::setAllSourcesVisible(bool visible)
{
    m_adsbVisible = visible;
    TrackLayer *layer = m_globe->trackLayer();
    QSet<QString> groups;
    for (const Track &t : m_store.tracks())
        groups.insert(trackSourceName(t.source) + QStringLiteral("::") + t.fileName);
    for (const QString &key : qAsConst(groups))
        layer->setGroupVisible(key, visible);
    refreshSources();                   // chip 状态 + 「隐藏/显示所有数据源」文字
    m_globe->update();
    scheduleSaveUiState();
}

// 「清空所有点迹显示」：所有航迹都不再画小圆点
void MainWindow::clearAllPointDots()
{
    m_dotKeys.clear();
    m_overviewDotKeys.clear();
    m_haveOverviewDots = false;
    applyDisplay();
    scheduleSaveUiState();
}

// 切换雷达航迹显示点列（平滑 / 关联 / 预测）：同一份导入数据无需重复导入
void MainWindow::setRadarPointMode(RadarPointMode mode)
{
    const int idx = int(mode);
    for (int i = 0; i < 3; ++i) {
        if (m_pointModeBtns[i]) {
            QSignalBlocker blk(m_pointModeBtns[i]);
            m_pointModeBtns[i]->setChecked(i == idx);
        }
        if (m_pointModeActions[i]) {
            QSignalBlocker blk(m_pointModeActions[i]);
            m_pointModeActions[i]->setChecked(i == idx);
        }
    }
    if (m_importCtrl)
        m_importCtrl->setDefaultPointMode(mode);
    if (m_radarMode == mode && m_globe->trackLayer()->radarPointMode() == mode)
        return;
    m_radarMode = mode;
    m_globe->resetInteraction();
    m_globe->trackLayer()->setRadarPointMode(mode);
    resetReplayRange();
    m_globe->update();
    scheduleSaveUiState();
}

// 「重置所有筛选」：长度 / 时间 / 空间套索 / 单独显示 / 管理系统可见集合与查询条件，
// 并恢复显示被隐藏的数据源，让画面回到全部数据原样显示
void MainWindow::resetAllFilters()
{
    if (m_filterPanel) {
        m_filterPanel->clickClear();                                // 时间
        for (TrackSource s : { TrackSource::Adsb, TrackSource::Radar, TrackSource::RadarRaw })
            m_filterPanel->setPointCount(s, false, -1, -1);         // 航迹点长度
        m_filterPanel->clickSpatialClear();                         // 空间套索
        m_filterPanel->refreshUi();
    }
    m_isoKeys.clear();                      // 地图单独显示 / 多选对比
    m_globe->clearFocusPoint();
    m_manage->clearVisible();               // 管理系统可见集合（单独显示）
    m_manage->clearHighlights();
    if (m_managePanel)
        m_managePanel->clickResetFilters(); // 管理系统的查询条件 + 分页
    setAllSourcesVisible(true);             // 数据源显隐也恢复到全部显示
    applyDisplay();
    scheduleSaveUiState();
}

// 展开 / 收起状态栏上方的数据源条（展开时地图高度相应压缩，不遮挡地图）
void MainWindow::setSourceStripOpen(bool open)
{
    if (!m_sourceStrip)
        return;
    m_sourceStrip->setVisible(open);
    m_statusBar->setSourceStripOpen(open);
    if (open)
        refreshSources();          // 每次展开都按当前航迹重建 chip
    scheduleSaveUiState();         // 折叠/展开状态写入数据库
}

void MainWindow::toggleSource(const QString &key)
{
    if (key == QLatin1String("adsb")) {
        m_adsbVisible = !m_adsbVisible;
        applyAdsbVisibility();
    } else {
        TrackLayer *layer = m_globe->trackLayer();
        layer->setGroupVisible(key, !layer->isGroupVisible(key));
    }
    refreshSources();
    m_globe->update();
    scheduleSaveUiState();
}

// ADS-B 是整体开关：每个 ADS-B 文件组都跟随它（新导入的文件也一样）
void MainWindow::applyAdsbVisibility()
{
    TrackLayer *layer = m_globe->trackLayer();
    for (const Track &t : m_store.tracks())
        if (t.source == TrackSource::Adsb)
            layer->setGroupVisible(trackSourceName(t.source) + QStringLiteral("::") + t.fileName, m_adsbVisible);
}

void MainWindow::placeBackAllButton()
{
    m_backAllBtn->move((m_globe->width() - m_backAllBtn->width()) / 2, 8);
}

bool MainWindow::eventFilter(QObject *obj, QEvent *e)
{
    if (obj == m_globe && e->type() == QEvent::Resize)
        placeBackAllButton();
    return QMainWindow::eventFilter(obj, e);
}

QDialog *MainWindow::showTrackPoints(int index)
{
    if (index < 0 || index >= m_store.size())
        return nullptr;
    TrackPointDialog *dlg = new TrackPointDialog(m_store.tracks()[index], this, m_radarMode);
    connect(dlg, &TrackPointDialog::pointActivated, this,
            [this](const QString &trackKey, qint64 timeMs, double lonDeg, double latDeg, const QString &label) {
                const int idx = m_store.indexOf(trackKey);
                if (idx >= 0 && !pointDotsShown(idx))
                    togglePointDots(idx);
                double curLon = 0.0, curLat = 0.0, curAlt = 600.0;
                m_globe->cameraView(&curLon, &curLat, &curAlt);
                m_globe->lookAt(lonDeg, latDeg, qMin(curAlt, 300.0));
                m_globe->setFocusPoint(lonDeg, latDeg, label);
                if (m_replay->end() > m_replay->start()
                        && timeMs >= m_replay->start() && timeMs <= m_replay->end())
                    m_replay->seekTime(timeMs);
            });
    dlg->show();
    return dlg;
}

TrackAnalysisWindow *MainWindow::showTrackAnalysis(int index, int tabIndex)
{
    if (!m_analysisWin) {
        m_analysisWin = new TrackAnalysisWindow(&m_store, nullptr);
        m_analysisWin->setAttribute(Qt::WA_DeleteOnClose);
        connect(m_analysisWin, &TrackAnalysisWindow::replaySeekRequested, m_replay, &ReplayController::seekTime);
        connect(m_analysisWin, &TrackAnalysisWindow::timeLocateRequested, this,
                [this](const QString &trackKey, qint64, double lonDeg, double latDeg, const QString &label) {
                    const int idx = m_store.indexOf(trackKey);
                    if (idx >= 0 && !pointDotsShown(idx))
                        togglePointDots(idx);
                    m_globe->setFocusPoint(lonDeg, latDeg, label);
                });
        connect(m_analysisWin, &TrackAnalysisWindow::compareOverlayChanged, m_globe, &GlobeWidget::setCompareLinks);
    }
    m_analysisWin->setReplayTime(m_replay->current(), m_replay->isActive());
    if (index >= 0) {
        m_analysisWin->openForTrack(index, tabIndex);
    } else {
        // 若多选隔离正好 2 条航迹且打开误差对比页，直接带入该两航迹
        const QSet<int> iso = isolatedTracks();
        if (tabIndex == 1 && iso.size() == 2) {
            auto it = iso.constBegin();
            const int a = *it;
            ++it;
            const int b = *it;
            m_analysisWin->openCompare(a, b);
        } else {
            m_analysisWin->openForTrack(isolatedTrack(), tabIndex);
        }
    }
    return m_analysisWin.data();
}

TrackAnalysisWindow *MainWindow::analysisWindow() const
{
    return m_analysisWin.data();
}

void MainWindow::setFilterOverlayEnabled(bool on)
{
    GlobeOverlay::FilterOverlayState st = m_globe->filterOverlay();
    st.enabled = on;
    m_globe->setFilterOverlay(st);
    if (m_filterOverlayBtn) {
        QSignalBlocker blk(m_filterOverlayBtn);
        m_filterOverlayBtn->setChecked(on);
    }
    if (m_filterOverlayAction) {
        QSignalBlocker blk(m_filterOverlayAction);
        m_filterOverlayAction->setChecked(on);
    }
}

bool MainWindow::filterOverlayEnabled() const
{
    return m_globe->filterOverlay().enabled;
}

void MainWindow::showRadarSiteDialog()
{
    RadarSiteDialog dlg(m_radarSites, this);
    connect(&dlg, &RadarSiteDialog::flyToSiteRequested, this, [this](double lon, double lat, double altKm) {
        m_globe->lookAt(lon, lat, altKm);
    });
    dlg.exec();
}

bool MainWindow::pointDotsShown(int index) const
{
    return index >= 0 && index < m_store.size() && m_dotKeys.contains(m_store.tracks()[index].key());
}

void MainWindow::togglePointDots(int index)
{
    if (index < 0 || index >= m_store.size())
        return;
    const QString key = m_store.tracks()[index].key();
    if (!m_dotKeys.remove(key))
        m_dotKeys.insert(key);
    applyDisplay();
    scheduleSaveUiState();
}

// 右键「设置颜色」：颜色按数据源（导入文件）独立保存，改这一条的同时该文件的全部航迹一起改；
// 地图图层、状态栏 chip、管理系统来源圆点、已打开的点迹三维窗口同步更新
void MainWindow::setTrackColor(int index)
{
    if (index < 0 || index >= m_store.size())
        return;
    const Track &t = m_store.tracks()[index];
    const QColor cur = m_store.fileColor(t.source, t.fileName);
    const QColor c = QColorDialog::getColor(
        cur, this, tr("设置颜色：%1（该数据源的全部航迹）").arg(t.fileName));
    if (!c.isValid() || c == cur)
        return;
    applyTrackColor(index, c);
}

void MainWindow::applyTrackColor(int index, const QColor &c)
{
    if (index < 0 || index >= m_store.size() || !c.isValid())
        return;
    const Track &t = m_store.tracks()[index];
    const QString key = trackSourceName(t.source) + QStringLiteral("::") + t.fileName;
    m_importCtrl->setCustomFileColor(key, c);
    refreshSources();                       // 状态栏数据源 chip 的颜色
    if (m_managePanel)
        m_managePanel->repaintRows();
    m_globe->update();
    m_point3d->refreshOverlayColors();
}

// 「详细信息」：高亮该航迹并打开管理面板，按来源 + ICAO 搜索（App.vue onShowTrackDetail）
void MainWindow::showTrackDetail(int index)
{
    if (index < 0 || index >= m_store.size())
        return;
    const Track &t = m_store.tracks()[index];
    m_manage->addHighlight(t.id);
    m_manage->filter.source = trackSourceKey(t.source);
    m_manage->filter.searchText = t.id;
    m_manage->page = 1;
    m_manage->saveSettings();
    if (activePanel() != int(PanelId::Manage))
        activatePanel(PanelId::Manage);
    m_managePanel->refresh();
}

void MainWindow::deleteTrackFromMap(int index)
{
    if (index < 0 || index >= m_store.size())
        return;
    const Track t = m_store.tracks()[index];
    const QString src = trackSourceName(t.source);
    if (!ui::confirm(this, QStringLiteral("确定隐藏航迹 %1 (%2)？\n数据保留在数据库中，可通过撤销恢复。").arg(t.id, src),
                     QStringLiteral("隐藏航迹"), true))
        return;
    m_manage->softDelete(QStringList() << t.key(), QStringLiteral("航迹 %1 (%2)").arg(t.id, src));
}

void MainWindow::updateUndoToast()
{
    const QVector<ManageState::UndoEntry> &st = m_manage->undoStack();
    m_undoToast->setState(st.isEmpty() ? QString() : st.last().label, st.size());
}

// 地图右键菜单（CesiumMap.vue）：点迹开关、详细信息、查看点迹数据、删除
void MainWindow::onTrackContextMenu(int index, const QPoint &globalPos)
{
    if (index < 0 || index >= m_store.size())
        return;
    ui::ContextMenu menu(this);
    if (!pointDotsShown(index))
        menu.addItem(LucideIcon::Dot, QStringLiteral("显示所有对应点迹"), [this, index]() { togglePointDots(index); });
    else
        menu.addItem(LucideIcon::Circle, QStringLiteral("隐藏所有对应点迹"), [this, index]() { togglePointDots(index); });
    menu.addItem(LucideIcon::Maximize2, QStringLiteral("缩放至该航迹"), [this, index]() { zoomToTrack(index); });
    // 航迹颜色按「数据源（导入文件）」独立设置：改这一条的同时该文件的所有航迹一起改
    menu.addItem(LucideIcon::Palette, QStringLiteral("设置颜色（本条及该数据源）..."),
                 [this, index]() { setTrackColor(index); });
    menu.addItem(LucideIcon::FileText, QStringLiteral("详细信息"), [this, index]() { showTrackDetail(index); });
    menu.addItem(LucideIcon::ClipboardList, QStringLiteral("查看点迹数据"), [this, index]() { showTrackPoints(index); });
    menu.addItem(LucideIcon::ChartColumn, QStringLiteral("时序曲线分析..."), [this, index]() { showTrackAnalysis(index, 0); });
    menu.addItem(LucideIcon::Layers, QStringLiteral("航迹误差对比..."), [this, index]() { showTrackAnalysis(index, 1); });
    menu.addItem(LucideIcon::Trash2, QStringLiteral("删除该航迹"), [this, index]() { deleteTrackFromMap(index); }, true);
    menu.exec(globalPos);
}

void MainWindow::onFlagContextMenu(const QString &flagId, const QPoint &globalPos)
{
    const int i = m_flags->indexOf(flagId);
    if (i < 0)
        return;
    ui::ContextMenu menu(this);
    menu.addItem(LucideIcon::Pencil, QStringLiteral("重命名"), [this, flagId]() {
        const int k = m_flags->indexOf(flagId);
        QString name;
        if (k >= 0 && ui::prompt(this, QStringLiteral("请输入新名称："), m_flags->flags()[k].label, &name)
                && !name.trimmed().isEmpty())
            m_flags->renameFlag(flagId, name);
    });
    menu.addItem(LucideIcon::Circle, QStringLiteral("绘制距离圆"),
                 [this, flagId]() { m_drawCtrl->drawDistanceCircle(flagId); });
    menu.addItem(LucideIcon::Trash2, QStringLiteral("删除"), [this, flagId]() {
        const int k = m_flags->indexOf(flagId);
        if (k >= 0 && ui::confirm(this, QStringLiteral("确定要删除旗标「%1」吗？").arg(m_flags->flags()[k].label),
                                  QStringLiteral("删除旗标")))
            m_flags->removeFlag(flagId);
    }, true);
    menu.exec(globalPos);
}

void MainWindow::syncReplayToLayer()
{
    m_globe->trackLayer()->setReplay(m_replay->isActive(), m_replay->current());
    m_globe->update();
    if (m_replay->isActive())
        m_point3d->onMasterTimeChanged(m_replay->current());
    if (m_analysisWin)
        m_analysisWin->setReplayTime(m_replay->current(), m_replay->isActive());
}

void MainWindow::toggleLabels()
{
    m_globe->setShowLabels(!m_globe->showLabels());
    app::setValue(QStringLiteral("display.show_labels"), m_globe->showLabels());
    if (m_labelAction)
        m_labelAction->setChecked(m_globe->showLabels());
}

// Esc：绘图先取消当前笔画，再退出绘图工具；然后套索、标尺、单独显示
void MainWindow::onEscape()
{
    // 选中了图形：Esc 先取消选中，再按一次才退出工具
    if (m_globe->selectedFeature() > 0) {
        m_globe->setSelectedFeature(0);
        return;
    }
    if (m_globe->drawTool() != DrawTool::None) {
        if (!m_globe->cancelDrawing())      // 没有未完成的笔画时才退出工具
            setDrawTool(DrawTool::None);
        return;
    }
    if (m_lasso && m_lasso->isActive()) {
        m_lasso->clearAll();
        m_filterPanel->refreshUi();
    } else if (m_filter.hasSpatialFilter()) {
        m_filter.spatialActive = false;
        m_filter.spatialPolygon.clear();
        if (m_lasso)
            m_lasso->clearAll();
        m_filter.save();
        m_filterPanel->refreshUi();
        applyDisplay();
    } else if (m_ruler->isActive()) {
        m_ruler->deactivate();
    } else if (m_globe->hasFocusPoint()) {
        m_globe->clearFocusPoint();
    } else if (!m_isoKeys.isEmpty()) {
        isolateTrack(-1);
    }
}

// ---------------------------------------------------------------
//  菜单栏 / 编辑区 / 侧栏切换
// ---------------------------------------------------------------
void MainWindow::buildMenuBar()
{
    QMenuBar *mb = menuBar();
    auto item = [this](QMenu *m, const QString &text, const QKeySequence &seq, std::function<void()> fn) {
        QAction *a = m->addAction(text);
        if (!seq.isEmpty()) {
            a->setShortcut(seq);
            a->setShortcutContext(Qt::WindowShortcut);
        }
        connect(a, &QAction::triggered, this, fn);
        return a;
    };

    QMenu *mFile = mb->addMenu(tr("文件"));
    auto pick = [this](TrackSource kind) {
        const bool csv = kind == TrackSource::Adsb;
        const QString path = QFileDialog::getOpenFileName(
            this, csv ? tr("导入 ADS-B 数据") : tr("导入雷达数据"), QString(),
            csv ? tr("ADS-B 数据 (*.csv);;所有文件 (*)") : tr("MATLAB 数据 (*.mat);;所有文件 (*)"));
        if (!path.isEmpty())
            importFile(kind, path);
    };
    QAction *aImportAdsb = item(mFile, tr("导入 ADS-B 数据..."), QKeySequence(Qt::CTRL + Qt::Key_O),
                                [pick]() { pick(TrackSource::Adsb); });
    QAction *aImportRadar = item(mFile, tr("导入雷达数据..."), QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_O),
                                 [pick]() { pick(TrackSource::Radar); });
    m_importCtrl->setImportActions({ aImportAdsb, aImportRadar });
    mFile->addSeparator();
    item(mFile, tr("打开瓦片 (.mbtiles)..."), QKeySequence(), [this]() { m_mapSources->openTileFile(); });
    QMenu *mSource = mFile->addMenu(tr("底图源"));
    m_mapSources->populateSourceMenu(mSource);
    mFile->addSeparator();
    QAction *aExit = item(mFile, tr("退出"), QKeySequence(Qt::ALT + Qt::Key_F4), []() { qApp->quit(); });
    aExit->setShortcutContext(Qt::WidgetShortcut);   // Alt+F4 由系统处理，这里只显示

    QMenu *mEdit = mb->addMenu(tr("编辑"));
    item(mEdit, tr("清除选中"), QKeySequence(Qt::Key_Escape), [this]() { onEscape(); });
    // Delete：只删「选择」工具选中的图形，没选中时动作是禁用的（不抢其他控件的 Delete）
    QAction *deleteDrawAction = item(mEdit, tr("删除选中图形"), QKeySequence(Qt::Key_Delete), [this]() {
        if (m_globe->selectedFeature() > 0)
            m_globe->deleteSelected();
    });
    deleteDrawAction->setEnabled(false);
    m_drawCtrl->setDeleteAction(deleteDrawAction);
    mEdit->addSeparator();
    item(mEdit, tr("首选项设置"), QKeySequence(Qt::CTRL + Qt::Key_Comma), [this]() { activatePanel(PanelId::Settings); });

    QMenu *mSettings = mb->addMenu(tr("设置"));
    mSettings->setObjectName(QStringLiteral("displaySettingsMenu"));
    setThemedStyle(mSettings, ui::menuBarQss() + QStringLiteral(
        "QLabel, QSpinBox, QDoubleSpinBox { font-family: %1; font-size: 12px; }"
        "QLabel { color: var(--menu-fg); background: transparent; }"
        "QSpinBox, QDoubleSpinBox { color: var(--input-fg); background: var(--input-bg);"
        " border: 1px solid var(--input-border); border-radius: 4px; padding: 3px 6px; min-width: 80px; }"
        "QSpinBox:focus, QDoubleSpinBox:focus { border-color: var(--accent-primary); }").arg(ui::uiFamilies()));
    auto settingRow = [mSettings](const QString &label, QWidget *control) {
        QWidget *row = new QWidget(mSettings);
        QHBoxLayout *layout = new QHBoxLayout(row);
        ui::bindMargins(layout, 12, 6, 12, 6);
        layout->addWidget(new QLabel(label, row));
        layout->addStretch();
        layout->addWidget(control);
        QWidgetAction *action = new QWidgetAction(mSettings);
        action->setDefaultWidget(row);
        mSettings->addAction(action);
    };
    QSpinBox *fontSize = new QSpinBox(mSettings);
    fontSize->setLocale(QLocale::c());
    fontSize->setObjectName(QStringLiteral("menuFontSize"));
    fontSize->setRange(ui::kFontMinPx, ui::kFontMaxPx);
    fontSize->setSuffix(QStringLiteral(" px"));
    fontSize->setValue(m_settingsPanel->fontValue());
    settingRow(tr("界面字号"), fontSize);
    connect(fontSize, QOverload<int>::of(&QSpinBox::valueChanged), m_settingsPanel, &SettingsPanel::setFontValue);
    connect(m_settingsPanel, &SettingsPanel::fontValueChanged, fontSize, &QSpinBox::setValue);
    QDoubleSpinBox *lineWidth = new QDoubleSpinBox(mSettings);
    lineWidth->setLocale(QLocale::c());
    lineWidth->setObjectName(QStringLiteral("menuLineWidth"));
    lineWidth->setDecimals(1);
    lineWidth->setRange(0.5, 8.0);
    lineWidth->setSingleStep(0.5);
    lineWidth->setSuffix(QStringLiteral(" px"));
    lineWidth->setValue(m_settingsPanel->lineWidthValue());
    settingRow(tr("所有航迹线宽"), lineWidth);
    connect(lineWidth, QOverload<double>::of(&QDoubleSpinBox::valueChanged), m_settingsPanel, &SettingsPanel::setLineWidthValue);
    connect(m_settingsPanel, &SettingsPanel::lineWidthValueChanged, lineWidth, &QDoubleSpinBox::setValue);
    QDoubleSpinBox *pointScale = new QDoubleSpinBox(mSettings);
    pointScale->setLocale(QLocale::c());
    pointScale->setObjectName(QStringLiteral("menuPointScale"));
    pointScale->setDecimals(1);
    pointScale->setRange(0.2, 5.0);
    pointScale->setSingleStep(0.1);
    pointScale->setSuffix(tr(" 倍"));
    pointScale->setValue(m_settingsPanel->pointScaleValue());
    settingRow(tr("点迹大小"), pointScale);
    connect(pointScale, QOverload<double>::of(&QDoubleSpinBox::valueChanged), m_settingsPanel, &SettingsPanel::setPointScaleValue);
    connect(m_settingsPanel, &SettingsPanel::pointScaleValueChanged, pointScale, &QDoubleSpinBox::setValue);
    mSettings->addSeparator();
    item(mSettings, tr("更多显示设置…"), QKeySequence(), [this]() {
        if (activePanel() != int(PanelId::Settings))
            activatePanel(PanelId::Settings);
    });

    QMenu *mView = mb->addMenu(tr("视图"));
    item(mView, tr("航迹管理系统"), QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_M), [this]() { activatePanel(PanelId::Manage); });
    item(mView, tr("旗标面板"), QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_F), [this]() { activatePanel(PanelId::Flags); });
    item(mView, tr("时间过滤"), QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_E), [this]() { activatePanel(PanelId::TimeFilter); });
    item(mView, tr("空间套索筛选"), QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_S), [this]() { m_filterPanel->clickSpatialToggle(); });
    mView->addSeparator();
    item(mView, tr("重置地图视角"), QKeySequence(Qt::CTRL + Qt::Key_R), [this]() { m_globe->resetView(); });
    m_labelAction = item(mView, tr("切换标签显示"), QKeySequence(Qt::CTRL + Qt::Key_T), [this]() { toggleLabels(); });
    m_labelAction->setCheckable(true);
    m_batchColorAction = item(mView, tr("按航迹批号分色"), QKeySequence(), [this]() {
        const bool byTrack = trackColorMode() != TrackLayer::TrackColorMode::ByTrack;
        setTrackColorMode(byTrack ? TrackLayer::TrackColorMode::ByTrack : TrackLayer::TrackColorMode::ByFile);
    });
    m_batchColorAction->setCheckable(true);
    m_batchColorAction->setToolTip(tr("开启后每条航迹按批号独立配色，便于区分同文件交叉航迹；关闭则恢复按数据源文件配色"));
    m_radarPpiAction = item(mView, tr("显示雷达 PPI 网格"), QKeySequence(), [this]() {
        if (m_radarSites)
            m_radarSites->setPpiVisible(!m_radarSites->ppiVisible());
    });
    m_radarPpiAction->setCheckable(true);
    m_radarPpiAction->setChecked(true);
    m_radarPpiAction->setToolTip(tr("显示/隐藏雷达收发站址、探测扇区、同心距离环与方位射线"));
    item(mView, tr("放大"), QKeySequence::ZoomIn, [this]() { m_globe->zoomBy(1.0); });
    item(mView, tr("缩小"), QKeySequence::ZoomOut, [this]() { m_globe->zoomBy(-1.0); });
    mView->addSeparator();
    // 整体显示开关：文字由当前真实显隐状态决定（全部隐藏时变成「显示所有数据源」）
    m_hideAllSourcesAction = item(mView, tr("隐藏所有数据源"), QKeySequence(),
                                  [this]() { setAllSourcesVisible(allSourcesHidden()); });
    m_hideAllSourcesAction->setToolTip(tr("隐藏全部数据源（ADS-B 与每个导入文件）；全部隐藏后此处变成「显示所有数据源」"));
    m_resetFiltersAction = item(mView, tr("重置所有筛选"), QKeySequence(),
                                [this]() { resetAllFilters(); });
    m_resetFiltersAction->setToolTip(tr("清除长度 / 时间 / 空间套索 / 单独显示 / 管理系统可见集合与查询条件，"
                                        "并恢复显示被隐藏的数据源，让画面回到全部数据原样显示"));
    m_clearDotsAction = item(mView, tr("清空所有点迹显示"), QKeySequence(), [this]() { clearAllPointDots(); });
    m_clearDotsAction->setToolTip(tr("取消所有航迹的「显示所有对应点迹」"));
    QMenu *mPointMode = mView->addMenu(tr("雷达点源"));
    {
        QActionGroup *grp = new QActionGroup(this);
        grp->setExclusive(true);
        const RadarPointMode modes[3] = { RadarPointMode::Smooth, RadarPointMode::Associated, RadarPointMode::Predicted };
        const char *const labels[3] = { "平滑点（smoothPointList）", "关联点（asscPointList）", "预测点（predictRes）" };
        for (int i = 0; i < 3; ++i) {
            QAction *a = mPointMode->addAction(QString::fromUtf8(labels[i]));
            a->setCheckable(true);
            a->setChecked(modes[i] == m_radarMode);
            grp->addAction(a);
            m_pointModeActions[i] = a;
            const RadarPointMode mode = modes[i];
            connect(a, &QAction::triggered, this, [this, mode]() { setRadarPointMode(mode); });
        }
    }
    QMenu *mFilterOv = mView->addMenu(tr("滤波叠加对比"));
    {
        m_filterOverlayAction = mFilterOv->addAction(tr("启用平滑/关联/预测同屏叠加"));
        m_filterOverlayAction->setCheckable(true);
        connect(m_filterOverlayAction, &QAction::toggled, this, &MainWindow::setFilterOverlayEnabled);
        mFilterOv->addSeparator();
        const char *const subNames[3] = { "显示关联点（◇）", "显示预测点与折线（×）", "显示预测→关联新息残差线" };
        for (int i = 0; i < 3; ++i) {
            QAction *sa = mFilterOv->addAction(QString::fromUtf8(subNames[i]));
            sa->setCheckable(true);
            sa->setChecked(true);
            m_filterOverlaySub[i] = sa;
            connect(sa, &QAction::toggled, this, [this, i](bool on) {
                GlobeOverlay::FilterOverlayState st = m_globe->filterOverlay();
                if (i == 0) st.showAssociated = on;
                else if (i == 1) st.showPredicted = on;
                else st.showResiduals = on;
                m_globe->setFilterOverlay(st);
            });
        }
    }
    mView->addSeparator();
    item(mView, tr("新建点迹三维窗口"), QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_P),
         [this]() { showPoint3dWindow(); });

    // 「窗口」菜单：列出已打开的点迹窗口，可激活或全部关闭
    QMenu *mWindows = mb->addMenu(tr("窗口"));
    connect(mWindows, &QMenu::aboutToShow, this, [this, mWindows]() {
        m_point3d->populateWindowMenu(mWindows);
    });

    QMenu *mTools = mb->addMenu(tr("工具"));
    item(mTools, tr("时序曲线分析..."), QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_A),
         [this]() { showTrackAnalysis(isolatedTrack(), 0); });
    item(mTools, tr("两航迹定量误差对比..."), QKeySequence(),
         [this]() { showTrackAnalysis(-1, 1); });
    item(mTools, tr("雷达站址与 PPI 网格管理..."), QKeySequence(),
         [this]() { showRadarSiteDialog(); });
    mTools->addSeparator();
    item(mTools, tr("旗标管理"), QKeySequence(), [this]() { activatePanel(PanelId::Flags); });
    item(mTools, tr("清除所有旗标"), QKeySequence(), [this]() {
        if (ui::confirm(this, QStringLiteral("确定要清除地图上所有旗标吗？此操作不可撤销。"), QStringLiteral("清除旗标")))
            m_flags->clearAll();
    });

    QMenu *mHelp = mb->addMenu(tr("帮助"));
    item(mHelp, tr("关于 RadarView"), QKeySequence(), [this]() {
        QMessageBox::about(this, tr("关于 RadarView"), tr("RadarView（Qt 版）\n雷达 / ADS-B 航迹导入、显示与回放"));
    });
    item(mHelp, tr("关于 Qt"), QKeySequence(), []() { QApplication::aboutQt(); });

    // 「绘图」入口：紧挨「帮助」右边（标题栏最后一个位置），点一下展开/收起绘图工具条。
    // 用普通 QAction 挂在菜单栏上（QMenuBar 不显示 QWidgetAction 里的控件），点它就是按钮。
    QAction *drawToggle = mb->addAction(tr("绘图"));
    drawToggle->setObjectName(QStringLiteral("drawToggleAction"));
    drawToggle->setCheckable(true);
    drawToggle->setToolTip(tr("显示 / 隐藏绘图工具栏"));
    connect(drawToggle, &QAction::triggered, this, &MainWindow::setDrawingBarVisible);
    m_drawCtrl->setToggleAction(drawToggle);

    // 菜单栏右上角：雷达点源分段切换（平滑 / 关联 / 预测），一次导入即可一键切换
    QWidget *corner = new QWidget(mb);
    corner->setObjectName(QStringLiteral("pointModeCorner"));
    QHBoxLayout *cl = new QHBoxLayout(corner);
    ui::bindMargins(cl, 4, 2, 8, 2);
    ui::bindSpacing(cl, 6);
    QLabel *lbl = new QLabel(tr("雷达点源"), corner);
    lbl->setObjectName(QStringLiteral("pointModeLabel"));
    cl->addWidget(lbl);
    QWidget *seg = new QWidget(corner);
    seg->setObjectName(QStringLiteral("mainPointModeSeg"));
    seg->setProperty("segmented", true);
    seg->setAttribute(Qt::WA_StyledBackground);
    QHBoxLayout *sl = new QHBoxLayout(seg);
    ui::bindMargins(sl, 2, 2, 2, 2);
    ui::bindSpacing(sl, 2);
    QButtonGroup *btnGrp = new QButtonGroup(seg);
    btnGrp->setExclusive(true);
    const RadarPointMode modes[3] = { RadarPointMode::Smooth, RadarPointMode::Associated, RadarPointMode::Predicted };
    const char *const names[3] = { "平滑", "关联", "预测" };
    const char *const tips[3] = {
        "显示雷达平滑点列（smoothPointList / outputPointList）",
        "显示雷达关联点列（asscPointList）",
        "显示雷达预测点列（predictRes）"
    };
    for (int i = 0; i < 3; ++i) {
        QPushButton *b = new QPushButton(QString::fromUtf8(names[i]), seg);
        b->setObjectName(QStringLiteral("pointModeBtn_%1").arg(i));
        b->setCheckable(true);
        b->setChecked(modes[i] == m_radarMode);
        b->setCursor(Qt::PointingHandCursor);
        b->setFocusPolicy(Qt::NoFocus);
        b->setToolTip(QString::fromUtf8(tips[i]));
        btnGrp->addButton(b, i);
        sl->addWidget(b);
        m_pointModeBtns[i] = b;
        const RadarPointMode mode = modes[i];
        connect(b, &QPushButton::clicked, this, [this, mode]() { setRadarPointMode(mode); });
    }
    cl->addWidget(seg);
    m_filterOverlayBtn = new QPushButton(tr("叠加对比"), corner);
    m_filterOverlayBtn->setObjectName(QStringLiteral("filterOverlayBtn"));
    m_filterOverlayBtn->setCheckable(true);
    m_filterOverlayBtn->setCursor(Qt::PointingHandCursor);
    m_filterOverlayBtn->setFocusPolicy(Qt::NoFocus);
    m_filterOverlayBtn->setToolTip(tr("同屏叠加雷达关联点(◇)、预测点(×)与预测→关联新息残差连线"));
    connect(m_filterOverlayBtn, &QPushButton::toggled, this, &MainWindow::setFilterOverlayEnabled);
    cl->addWidget(m_filterOverlayBtn);
    setThemedStyle(corner, ui::segmentedQss() + QStringLiteral(
        "#pointModeCorner { background: transparent; }"
        "#pointModeLabel { color: var(--text-secondary); background: transparent; font-family: %1; font-size: 11px; }"
        "QWidget[segmented=\"true\"] QPushButton { padding: 2px 8px; font-size: 11px; }"
        "#filterOverlayBtn { padding: 2px 8px; font-size: 11px; border-radius: 4px;"
        " border: 1px solid var(--border-primary); background: var(--button-bg); color: var(--button-fg); }"
        "#filterOverlayBtn:hover { background: var(--button-hover); color: var(--text-primary); }"
        "#filterOverlayBtn:checked { background: var(--accent-primary); color: var(--accent-fg); border-color: var(--accent-primary); font-weight: 600; }")
        .arg(ui::uiFamilies()));
    mb->setCornerWidget(corner, Qt::TopRightCorner);
}

QStackedWidget *MainWindow::buildEditorArea()
{
    QStackedWidget *stack = new QStackedWidget;
    stack->setObjectName("editorArea");
    m_globe = new GlobeWidget(stack);
    stack->addWidget(m_globe);
    return stack;
}

void MainWindow::onIconClicked(int index)
{
    activatePanel(PanelId(index));
}

int MainWindow::activePanel() const
{
    return m_sidePanel->isVisible() || (!isVisible() && !m_sidePanel->isHidden()) ? m_panel : -1;
}

// useActivityBar.activate：点当前面板关闭侧栏，否则切换并恢复该面板记住的宽度
void MainWindow::activatePanel(PanelId id)
{
    const int i = int(id);
    if (m_panel == i && !m_sidePanel->isHidden()) {
        closeSidebar();
        return;
    }
    if (m_panel >= 0 && !m_sidePanel->isHidden())
        m_panelWidth[m_panel] = m_splitter->sizes().value(0);
    m_panel = i;
    m_sidePanel->showPanel(i, appshell::panelTitle(id));
    m_sidePanel->show();
    const int w = qBound(ui::sz(appshell::kSidebarMin),
                         m_panelWidth.value(i, ui::sz(appshell::kSidebarDefault)),
                         ui::sz(appshell::kSidebarMax));
    const int total = qMax(m_splitter->width(), w + 200);
    m_splitter->setSizes(QList<int>() << w << total - w);
    m_activityBar->highlight(i);
    if (id == PanelId::Manage)
        m_managePanel->refresh();
    scheduleSaveUiState();
}

void MainWindow::closeSidebar()
{
    if (m_panel >= 0 && !m_sidePanel->isHidden())
        m_panelWidth[m_panel] = m_splitter->sizes().value(0);
    m_sidePanel->hide();
    m_panel = -1;
    m_activityBar->highlight(-1);
    scheduleSaveUiState();
}
