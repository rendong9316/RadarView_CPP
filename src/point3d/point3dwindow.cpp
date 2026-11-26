#include "point3dwindow.h"

#include "apppaths.h"
#include "point3dadvancedpanel.h"
#include "point3dcanvas.h"
#include "point3dimportjob.h"
#include "point3dparsers.h"
#include "point3dsidepanel.h"
#include "theme.h"
#include "trackdb.h"
#include "trackpicker.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QShortcut>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <functional>

namespace {

QVariant setting(const char *key, const QVariant &def)
{
    return app::settings().value(QString::fromLatin1(key), def);
}

QString toolbarQss()
{
    return QStringLiteral(
        // 菜单栏（对齐主窗口）
        "QWidget#point3dNavBar { background: var(--bg-secondary);"
        " border-bottom: 1px solid var(--border-primary); }"
        "QMenuBar { background: transparent; color: var(--text-secondary);"
        " font-family: %1; font-size: 12px; border: none; }"
        "QMenuBar::item { padding: 4px 10px; background: transparent; }"
        "QMenuBar::item:selected { background: var(--button-hover); color: var(--text-primary); }"
        "QMenu { background: var(--dropdown-bg); color: var(--dropdown-fg);"
        " border: 1px solid var(--border-primary); font-family: %1; font-size: 12px; }"
        "QMenu::item { padding: 4px 18px; }"
        "QMenu::item:selected { background: var(--dropdown-hover); }"
        // 左侧「显示」侧栏
        "QStackedWidget#point3dSideStack { background: var(--bg-secondary);"
        " border-right: 1px solid var(--border-primary); }"
        "QSplitter::handle { background: var(--border-primary); }"
        "QScrollArea#point3dSide, QScrollArea#point3dAdvanced { background: var(--bg-secondary);"
        " border: none; }"
        "QWidget#point3dSidePage { background: var(--bg-secondary); }"
        "QGroupBox { color: var(--text-secondary); border: 1px solid var(--border-primary);"
        " border-radius: 4px; margin-top: 10px; font-family: %1; font-size: 12px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }"
        "QScrollBar:vertical { background: var(--bg-tertiary); width: 10px; margin: 0; border: none; }"
        "QScrollBar::handle:vertical { background: var(--border-primary); min-height: 40px;"
        " border-radius: 5px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0;"
        " border: none; background: none; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }"
        "QScrollBar:horizontal { background: var(--bg-tertiary); height: 10px; margin: 0;"
        " border: none; }"
        "QScrollBar::handle:horizontal { background: var(--border-primary); min-width: 40px;"
        " border-radius: 5px; }"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0;"
        " border: none; background: none; }"
        "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: none; }"
        // 底部状态栏
        "QWidget#point3dStatus { background: var(--bg-secondary);"
        " border-top: 1px solid var(--border-primary); }"
        "QLabel { color: var(--text-secondary); font-family: %1; font-size: 12px; }"
        "QCheckBox { color: var(--text-secondary); font-family: %1; font-size: 12px; }"
        "QPushButton { color: var(--text-secondary); background: var(--button-bg);"
        " border: 1px solid var(--border-primary); border-radius: 3px; padding: 4px 10px;"
        " font-family: %1; font-size: 12px; }"
        "QPushButton:hover { background: var(--button-hover); color: var(--text-primary); }"
        "QPushButton:checked { background: var(--accent-primary); color: #ffffff;"
        " border-color: var(--accent-primary); }"
        "QComboBox, QSpinBox, QDoubleSpinBox { color: var(--text-primary); background: var(--input-bg);"
        " border: 1px solid var(--input-border); border-radius: 3px; padding: 2px 6px;"
        " font-family: %1; font-size: 12px; }"
        "QSlider::groove:horizontal { height: 4px; background: var(--bg-tertiary);"
        " border-radius: 2px; }"
        "QSlider::sub-page:horizontal { background: var(--accent-primary); border-radius: 2px; }"
        "QSlider::handle:horizontal { width: 12px; height: 12px; margin: -4px 0;"
        " border-radius: 6px; background: var(--accent-primary); }"
        "QComboBox QAbstractItemView { background: var(--dropdown-bg); color: var(--dropdown-fg);"
        " selection-background-color: var(--dropdown-hover); }")
        .arg(ui::uiFamilies());
}

} // namespace

Point3dWindow::Point3dWindow(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlag(Qt::Window, true);
    setWindowTitle(QStringLiteral("点迹三维可视化"));
    buildUi();

    const QByteArray geo = setting("point3d.window_geometry", QByteArray()).toByteArray();
    if (geo.isEmpty() || !restoreGeometry(geo))
        resize(1200, 800);
}

Point3dWindow::~Point3dWindow()
{
    if (m_job) {
        m_job->requestCancel();
        m_job->wait();
    }
}

void Point3dWindow::setNumber(int n)
{
    m_number = qMax(1, n);
    setWindowTitle(m_number <= 1 ? QStringLiteral("点迹三维可视化")
                                 : QStringLiteral("点迹三维可视化 %1").arg(m_number));
}

// ---------------------------------------------------------------------------
//  本窗口自带的回放：独立于主窗口时间轴，只按本窗口点迹帧逐帧推进
//  - 不做插值：一帧停留一段时间后直接切到下一帧
//  - 停留时长 = 相邻两帧的时间差 / 倍速（1× 即按数据真实节奏回放）
//  - 进度条绑定帧序号（有级），拖动只能落在某一帧上
// ---------------------------------------------------------------------------
int Point3dWindow::currentFrame() const
{
    return m_canvas->currentFrame();
}

int Point3dWindow::frameHoldMs() const
{
    const int n = m_data.frames.size();
    if (n < 2)
        return 100;
    const int cur = qBound(0, m_canvas->currentFrame(), n - 1);
    const int next = qMin(cur + 1, n - 1);
    qint64 interval = m_data.frames.at(next).timeMs - m_data.frames.at(cur).timeMs;
    if (interval <= 0)
        interval = 100;                       // 时间戳异常时的兜底
    const double hold = double(interval) / qMax(0.01, m_speed);
    return int(qBound<qint64>(1, qint64(std::llround(hold)), 600000));
}

void Point3dWindow::setPlaying(bool on)
{
    if (on == m_playing)
        return;
    if (on) {
        if (m_data.frames.isEmpty())
            return;
        if (m_canvas->currentFrame() >= m_data.frames.size() - 1)
            setFrameInternal(0);              // 已在末尾：从头播
        m_frameClock.start();
        m_playTimer->start();
    } else {
        m_playTimer->stop();
    }
    m_playing = on;
    updatePlaybackUi();
}

void Point3dWindow::togglePlay()
{
    setPlaying(!m_playing);
}

void Point3dWindow::setSpeed(double speed)
{
    if (!(speed > 0.0) || !std::isfinite(speed))
        return;
    m_speed = speed;
    m_frameClock.restart();                   // 新倍速从当前帧重新计时
    app::setValue(QStringLiteral("point3d.play_speed"), m_speed);
}

void Point3dWindow::onPlayTick()
{
    if (!m_playing || m_data.frames.isEmpty())
        return;
    const int n = m_data.frames.size();
    const int cur = m_canvas->currentFrame();
    if (cur >= n - 1) {
        setPlaying(false);
        return;
    }
    if (m_frameClock.elapsed() < frameHoldMs())
        return;
    setFrameInternal(cur + 1);
    if (m_canvas->currentFrame() >= n - 1) {
        setPlaying(false);                    // 最后一帧显示完就停
    } else {
        m_frameClock.restart();
    }
}

void Point3dWindow::seekToFrame(int index)
{
    if (m_data.frames.isEmpty())
        return;
    setFrameInternal(qBound(0, index, m_data.frames.size() - 1));
    m_frameClock.restart();                   // 拖拽后重新计时，避免刚拖完就跳帧
}

void Point3dWindow::setFrameInternal(int index)
{
    m_canvas->setCurrentFrame(index);
    syncPlaybackUi();
    updateStatus();
}

void Point3dWindow::syncPlaybackUi()
{
    const int n = m_data.frames.size();
    const int cur = n > 0 ? qBound(0, m_canvas->currentFrame(), n - 1) : 0;
    m_syncing = true;
    m_seekSlider->setRange(0, qMax(0, n - 1));
    m_seekSlider->setPageStep(1);
    m_seekSlider->setSingleStep(1);
    m_seekSlider->setValue(cur);
    m_seekSlider->setEnabled(n > 1);
    if (n > 0)
        m_timeLabel->setText(QStringLiteral("%1  帧 %2/%3")
                                 .arg(formatBeijingTime(m_data.frames.at(cur).timeMs))
                                 .arg(cur + 1).arg(n));
    else
        m_timeLabel->setText(QStringLiteral("--"));
    m_syncing = false;
}

void Point3dWindow::updatePlaybackUi()
{
    m_playBtn->setText(m_playing ? QStringLiteral("暂停") : QStringLiteral("播放"));
    m_playBtn->setEnabled(!m_data.frames.isEmpty());
}

// ---------------------------------------------------------------------------
//  航迹叠加与地图 / 数据库联动
// ---------------------------------------------------------------------------
void Point3dWindow::setTrackColorProvider(
    const std::function<QColor(TrackSource, const QString &)> &provider)
{
    m_colorProvider = provider;
}

void Point3dWindow::refreshOverlayColors()
{
    reloadOverlaysFromKeys();       // 颜色在 overlayFromTrack 里由提供者取当前文件色
    updateStatus();
}

void Point3dWindow::reloadOverlaysFromKeys()
{
    m_data.tracks.clear();
    m_skippedTracks = 0;
    if (!m_trackKeys.isEmpty() && !m_data.frames.isEmpty() && trackdb::isOpen()) {
        const double rangeScale = setting("point3d.track_range_scale", 1.0).toDouble();
        const int minLen = qMax(1, setting("point3d.min_track_length", 4).toInt());
        const QVector<Track> tracks = trackdb::loadTracks(m_trackKeys);
        for (const Track &t : tracks) {
            TrackOverlay ov;
            const QColor color = m_colorProvider ? m_colorProvider(t.source, t.fileName)
                                                 : QColor();
            if (!overlayFromTrack(t, m_data.frames, &ov, color, rangeScale)) {
                ++m_skippedTracks;       // 与点迹时间窗对不上（一致性校验）
                continue;
            }
            if (qMax(ov.points.size(), ov.rawPoints.size()) < minLen)
                continue;                // 最小航迹长度：太短的航迹不画（对应 MATLAB MinTrackLength）
            m_data.tracks.append(ov);
        }
    }
    m_canvas->refreshLimits();
    applyLinkedSelection();              // 重载后重新套用地图当前的选中/筛选
}

// 地图侧选中/筛选 → 本窗口同步"单独显示"对应航迹
void Point3dWindow::setLinkedSelection(const QVector<LinkedTrackRef> &refs)
{
    m_linkedSelection = refs;
    applyLinkedSelection();
}

void Point3dWindow::applyLinkedSelection()
{
    if (m_linkedSelection.isEmpty()) {
        m_canvas->setIsolatedBatches(QSet<int>());
        updateStatus();
        return;
    }
    QSet<int> batches;
    for (const TrackOverlay &ov : qAsConst(m_data.tracks))
        for (const LinkedTrackRef &r : qAsConst(m_linkedSelection))
            if (point3dRefsMatch(ov.batchNo, ov.minTime, ov.maxTime, r.batchNo, r.t0, r.t1)) {
                batches.insert(ov.batchNo);
                break;
            }
    if (batches.isEmpty())
        return;                          // 地图选中的航迹本窗口没有：保持原样
    m_canvas->setIsolatedBatches(batches);
    updateStatus();
}

// 地图悬停某条航迹 → 本窗口里"同一条航迹"同步高亮
void Point3dWindow::setLinkedHighlight(int batchNo, qint64 t0, qint64 t1)
{
    int resolved = -1;
    if (batchNo > 0)
        for (const TrackOverlay &ov : qAsConst(m_data.tracks))
            if (point3dRefsMatch(ov.batchNo, ov.minTime, ov.maxTime, batchNo, t0, t1)) {
                resolved = ov.batchNo;
                break;
            }
    m_canvas->setHighlightedBatch(resolved);
    updateStatus();
}

QSet<int> Point3dWindow::isolatedBatches() const
{
    return m_canvas->isolatedBatches();
}

// ---------------------------------------------------------------------------
//  区域筛选与视图转发（委托给左侧设置栏 Point3dSidePanel）
// ---------------------------------------------------------------------------
Point3dRegion Point3dWindow::regionForView(Point3dViewMode mode) const
{
    return m_sideSettings->regionForView(mode);
}

bool Point3dWindow::regionAppliedForView(Point3dViewMode mode) const
{
    return m_sideSettings->regionAppliedForView(mode);
}

bool Point3dWindow::setRegionForCurrentView(const Point3dRegion &r, bool apply)
{
    return m_sideSettings->setRegionForCurrentView(r, apply);
}

void Point3dWindow::setViewMode(Point3dViewMode mode)
{
    m_sideSettings->setViewMode(mode);
}

void Point3dWindow::syncViewMenuChecks(Point3dViewMode mode)
{
    for (int i = 0; i < 3; ++i)
        if (m_viewActions[i])
            m_viewActions[i]->setChecked(i == int(mode));
}

void Point3dWindow::chooseTracksFromDb()
{
    if (!trackdb::isOpen()) {
        QMessageBox::information(this, QStringLiteral("选择航迹"),
                                 QStringLiteral("数据库未打开。请先在主窗口导入雷达航迹数据。"));
        return;
    }
    TrackPicker dlg(this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const QVector<DbTrackSummary> sel = dlg.selectedTracks();
    m_trackKeys.clear();
    for (const DbTrackSummary &s : sel)
        m_trackKeys << s.icao + QStringLiteral("::") + trackSourceName(s.source)
                    + QStringLiteral("::") + s.fileName;

    reloadOverlaysFromKeys();

    // 一致性校验：航迹必须与点迹在时间上对得上，否则单独提示
    if (m_skippedTracks > 0 && !app::testMode())
        QMessageBox::warning(
            this, QStringLiteral("时间不匹配"),
            QStringLiteral("%1 条航迹与点迹时间窗对不上，已忽略。\n"
                           "本窗口点迹：%2 ~ %3\n\n"
                           "请确认选的是与点迹同一批数据的航迹。")
                .arg(m_skippedTracks)
                .arg(formatBeijingTime(m_data.minTime), formatBeijingTime(m_data.maxTime)));

    // 用户明确选中的航迹不能被「点源」过滤静默藏起来：当前模式取不到点列就自动切另一列
    if (!m_data.tracks.isEmpty()) {
        bool admitted = false;
        for (const TrackOverlay &ov : qAsConst(m_data.tracks))
            if (point3dOverlayHasColumn(ov, m_canvas->trackSource())) {
                admitted = true;
                break;
            }
        if (!admitted)
            m_sideSettings->toggleTrackSource();
    } else if (!sel.isEmpty() && !app::testMode()) {
        // 选了航迹却一条也叠加不上：多半是时间窗不重叠或旧数据没有测量坐标
        QMessageBox::information(
            this, QStringLiteral("选择航迹"),
            QStringLiteral("所选 %1 条航迹都无法叠加到本窗口：\n"
                           "· 点迹与航迹时间窗不重叠，或\n"
                           "· 该航迹是升级前导入的，缺少测量坐标（prange/pvr/paz）。\n\n"
                           "请检查时间范围，或在地球侧重导该雷达 MAT。").arg(sel.size()));
    }
    updateStatus();
}

// ---------------------------------------------------------------------------
//  多窗口会话保存 / 恢复
// ---------------------------------------------------------------------------
QJsonObject Point3dWindow::sessionJson() const
{
    QJsonObject o;
    QJsonArray files;
    for (const QString &f : m_files)
        files.append(f);
    o.insert(QStringLiteral("files"), files);

    QJsonArray tracks;
    for (const QString &k : m_trackKeys)
        tracks.append(k);
    o.insert(QStringLiteral("tracks"), tracks);

    double yaw = 42.0, pitch = 23.0, zoom = 1.0, panX = 0.0, panY = 0.0;
    m_canvas->camera(&yaw, &pitch, &zoom, &panX, &panY);
    QJsonObject cam;
    cam.insert(QStringLiteral("yaw"), yaw);
    cam.insert(QStringLiteral("pitch"), pitch);
    cam.insert(QStringLiteral("zoom"), zoom);
    cam.insert(QStringLiteral("pan_x"), panX);
    cam.insert(QStringLiteral("pan_y"), panY);
    o.insert(QStringLiteral("camera"), cam);

    o.insert(QStringLiteral("view_mode"), int(m_sideSettings->viewMode()));
    o.insert(QStringLiteral("window_length"), m_canvas->windowLength());
    o.insert(QStringLiteral("regions"), m_sideSettings->regionsToJson());
    o.insert(QStringLiteral("geometry"),
             QString::fromLatin1(saveGeometry().toBase64()));
    o.insert(QStringLiteral("number"), m_number);
    return o;
}

void Point3dWindow::restoreSession(const QJsonObject &obj)
{
    m_restoring = true;
    m_sideSettings->setRestoring(true);

    if (obj.contains(QStringLiteral("number")))
        setNumber(obj.value(QStringLiteral("number")).toInt(m_number));

    const QByteArray geo = QByteArray::fromBase64(
        obj.value(QStringLiteral("geometry")).toString().toLatin1());
    if (!geo.isEmpty())
        restoreGeometry(geo);

    // 区域筛选（三个视图各一套）：先恢复状态，再切视图，切完统一套用到画布
    m_sideSettings->restoreRegionsFromJson(obj.value(QStringLiteral("regions")).toArray());

    const int mode = obj.value(QStringLiteral("view_mode")).toInt(
        int(Point3dViewMode::RVF));
    m_sideSettings->setViewMode(Point3dViewMode(mode));
    const Point3dViewMode resolvedMode = m_sideSettings->viewMode();
    m_canvas->setViewMode(resolvedMode);
    syncViewMenuChecks(resolvedMode);
    m_sideSettings->loadRegionForCurrentView();

    const int len = obj.value(QStringLiteral("window_length")).toInt(0);
    m_sideSettings->setWindowLength(len);

    const QJsonObject cam = obj.value(QStringLiteral("camera")).toObject();
    if (!cam.isEmpty())
        m_canvas->setCamera(cam.value(QStringLiteral("yaw")).toDouble(42.0),
                            cam.value(QStringLiteral("pitch")).toDouble(23.0),
                            cam.value(QStringLiteral("zoom")).toDouble(1.0),
                            cam.value(QStringLiteral("pan_x")).toDouble(0.0),
                            cam.value(QStringLiteral("pan_y")).toDouble(0.0));

    m_files.clear();
    for (const QJsonValue &v : obj.value(QStringLiteral("files")).toArray())
        m_files << v.toString();
    m_trackKeys.clear();
    for (const QJsonValue &v : obj.value(QStringLiteral("tracks")).toArray())
        m_trackKeys << v.toString();

    m_sideSettings->setRestoring(false);
    m_restoring = false;
    if (!m_files.isEmpty())
        startPendingLoad();
}

void Point3dWindow::startPendingLoad()
{
    QStringList existing;
    for (const QString &f : qAsConst(m_files))
        if (QFileInfo::exists(f))
            existing << f;
    if (existing.isEmpty()) {
        m_status->setText(QStringLiteral("上次打开的点迹文件已不存在"));
        m_files.clear();
        return;
    }
    loadDetectionFiles(existing);
}

// ---------------------------------------------------------------------------
//  主界面搭建与连接
// ---------------------------------------------------------------------------
void Point3dWindow::buildUi()
{
    m_restoring = true;

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- 菜单栏（对齐地图窗口：动作进菜单、参数进侧栏、回放放底部）----
    m_canvas = new Point3dCanvas(this);

    QMenuBar *mb = new QMenuBar(this);
    mb->setObjectName(QStringLiteral("point3dMenuBar"));
    QMenu *fileMenu = mb->addMenu(QStringLiteral("文件"));
    QAction *actImport = fileMenu->addAction(QStringLiteral("导入点迹…"));
    actImport->setShortcut(QKeySequence(QStringLiteral("Ctrl+O")));
    QAction *actPick = fileMenu->addAction(QStringLiteral("选择航迹…"));
    QAction *actExport = fileMenu->addAction(QStringLiteral("导出 PNG…"));
    fileMenu->addSeparator();
    QAction *actClose = fileMenu->addAction(QStringLiteral("关闭"));
    actClose->setShortcut(QKeySequence(QStringLiteral("Ctrl+W")));

    QMenu *viewMenu = mb->addMenu(QStringLiteral("视图"));
    {
        QActionGroup *group = new QActionGroup(this);
        const QString names[3] = { QStringLiteral("距离–速度–帧号"),
                                   QStringLiteral("距离–方位–帧号"),
                                   QStringLiteral("速度–方位–帧号") };
        for (int i = 0; i < 3; ++i) {
            m_viewActions[i] = viewMenu->addAction(names[i]);
            m_viewActions[i]->setCheckable(true);
            group->addAction(m_viewActions[i]);
            const Point3dViewMode mode = Point3dViewMode(i);
            connect(m_viewActions[i], &QAction::triggered, this,
                    [this, mode]() { setViewMode(mode); });
        }
    }
    viewMenu->addSeparator();
    QAction *actResetView = viewMenu->addAction(QStringLiteral("重置视角"));
    connect(actResetView, &QAction::triggered, m_canvas, &Point3dCanvas::resetCamera);

    // 顶部导航：菜单 + 「设置」「高级」按钮（点一下展开/收起对应的左侧侧栏）
    QWidget *navBar = new QWidget(this);
    navBar->setObjectName(QStringLiteral("point3dNavBar"));
    navBar->setAttribute(Qt::WA_StyledBackground, true);
    QHBoxLayout *nav = new QHBoxLayout(navBar);
    nav->setContentsMargins(ui::sz(6), 0, ui::sz(8), 0);
    nav->setSpacing(ui::sz(6));
    nav->addWidget(mb);
    m_settingsBtn = new QPushButton(QStringLiteral("设置"), navBar);
    m_settingsBtn->setCheckable(true);
    m_settingsBtn->setToolTip(QStringLiteral("展开 / 收起左侧「显示」设置侧栏"));
    m_advancedBtn = new QPushButton(QStringLiteral("高级"), navBar);
    m_advancedBtn->setCheckable(true);
    m_advancedBtn->setToolTip(QStringLiteral("展开 / 收起左侧「高级参数」侧栏"));
    nav->addWidget(m_settingsBtn);
    nav->addWidget(m_advancedBtn);
    nav->addStretch(1);
    root->addWidget(navBar);

    // ---- 主体：左侧停靠区（设置页 / 高级页）+ 画布 ----
    QHBoxLayout *body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    m_sideStack = new QStackedWidget(this);
    m_sideStack->setObjectName(QStringLiteral("point3dSideStack"));
    m_sideStack->setMinimumWidth(ui::sz(220));
    m_sideSettings = new Point3dSidePanel(m_canvas, &m_data, this);
    m_sideAdvanced = new Point3dAdvancedPanel(this);
    m_sideStack->addWidget(m_sideSettings);
    m_sideStack->addWidget(m_sideAdvanced);
    m_sidePanel = m_sideStack;
    // 侧栏和画布之间用分割器：侧栏宽度可以拖着改
    QSplitter *split = new QSplitter(Qt::Horizontal, this);
    split->setChildrenCollapsible(false);
    split->setHandleWidth(ui::sz(4));
    split->addWidget(m_sidePanel);
    split->addWidget(m_canvas);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    {
        const int sideW = qBound(ui::sz(220), app::settings().value(
                                     QStringLiteral("point3d.side_width"), ui::sz(300)).toInt(),
                                 ui::sz(900));
        split->setSizes(QList<int>() << sideW << qMax(int(ui::sz(200)), width() - sideW));
    }
    connect(split, &QSplitter::splitterMoved, this, [this](int, int) {
        if (!m_restoring)
            app::setValue(QStringLiteral("point3d.side_width"), m_sidePanel->width());
    });
    body->addWidget(split, 1);
    root->addLayout(body, 1);

    // ---- 底部：状态文字 + 回放条（对齐地图窗口的状态栏）----
    m_statusBar = new QWidget(this);
    m_statusBar->setObjectName(QStringLiteral("point3dStatus"));
    m_statusBar->setAttribute(Qt::WA_StyledBackground, true);
    QVBoxLayout *statusLay = new QVBoxLayout(m_statusBar);
    statusLay->setContentsMargins(ui::sz(10), ui::sz(3), ui::sz(10), ui::sz(4));
    statusLay->setSpacing(ui::sz(3));
    {
        QHBoxLayout *infoRow = new QHBoxLayout;
        infoRow->setSpacing(ui::sz(8));
        m_status = new QLabel(m_statusBar);
        m_status->setText(QStringLiteral("未导入点迹数据"));
        m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        m_progress = new QProgressBar(m_statusBar);
        m_progress->setRange(0, 100);
        m_progress->setTextVisible(false);
        m_progress->setFixedWidth(ui::sz(160));
        m_progress->setVisible(false);
        infoRow->addWidget(m_status, 1);
        infoRow->addWidget(m_progress);
        statusLay->addLayout(infoRow);

        QHBoxLayout *transport = new QHBoxLayout;
        transport->setSpacing(ui::sz(8));
        m_playBtn = new QPushButton(QStringLiteral("播放"), m_statusBar);
        m_playBtn->setToolTip(QStringLiteral("播放 / 暂停本窗口的回放（空格）。只回放本窗口的点迹与航迹"));
        m_playBtn->setFixedWidth(ui::sz(72));
        transport->addWidget(m_playBtn);
        m_seekSlider = new QSlider(Qt::Horizontal, m_statusBar);
        m_seekSlider->setRange(0, 0);
        m_seekSlider->setPageStep(1);
        m_seekSlider->setSingleStep(1);
        m_seekSlider->setToolTip(QStringLiteral("拖动跳转到某一帧（按帧吸附，不插值）"));
        transport->addWidget(m_seekSlider, 1);
        m_timeLabel = new QLabel(QStringLiteral("--"), m_statusBar);
        m_timeLabel->setMinimumWidth(ui::sz(190));
        transport->addWidget(m_timeLabel);
        transport->addWidget(new QLabel(QStringLiteral("倍速"), m_statusBar));
        m_speedCombo = new QComboBox(m_statusBar);
        m_speedCombo->setEditable(true);
        for (int s : { 1, 2, 5, 10, 20, 50, 100, 300, 500, 2000 })
            m_speedCombo->addItem(QString::number(s));
        m_speedCombo->setToolTip(QStringLiteral(
            "本窗口回放倍速：每帧停留 = 相邻帧时间差 / 倍速（1× 为数据真实节奏）"));
        m_speedCombo->setFixedWidth(ui::sz(100));
        transport->addWidget(m_speedCombo);
        statusLay->addLayout(transport);
    }
    root->addWidget(m_statusBar);

    setThemedStyle(this, toolbarQss());

    // ---- 连接 ----
    connect(actImport, &QAction::triggered, this, &Point3dWindow::importDetections);
    m_importAction = actImport;
    connect(actPick, &QAction::triggered, this, &Point3dWindow::chooseTracksFromDb);
    connect(actExport, &QAction::triggered, this, &Point3dWindow::exportCanvasImage);
    connect(actClose, &QAction::triggered, this, &QWidget::close);

    // 顶部「设置」「高级」：点一下展开对应的左侧侧栏，再点一下收起
    connect(m_settingsBtn, &QPushButton::clicked, this, [this]() {
        const bool show = m_sidePanel->isHidden() || m_sideStack->currentIndex() != 0;
        setSidePanelPage(0, show);
    });
    connect(m_advancedBtn, &QPushButton::clicked, this, [this]() {
        const bool show = m_sidePanel->isHidden() || m_sideStack->currentIndex() != 1;
        setSidePanelPage(1, show);
    });

    // 设置页 / 高级页信号
    connect(m_sideSettings, &Point3dSidePanel::viewModeChanged,
            this, &Point3dWindow::syncViewMenuChecks);
    connect(m_sideSettings, &Point3dSidePanel::statusChanged,
            this, &Point3dWindow::updateStatus);
    connect(m_sideSettings, &Point3dSidePanel::statusMessage,
            this, [this](const QString &text) { m_status->setText(text); });

    connect(m_sideAdvanced, &Point3dAdvancedPanel::continuityConfigChanged,
            this, &Point3dWindow::recomputeContinuityNow);
    connect(m_sideAdvanced, &Point3dAdvancedPanel::overlayParamsChanged, this, [this]() {
        reloadOverlaysFromKeys();
        updateStatus();
    });
    connect(m_sideAdvanced, &Point3dAdvancedPanel::strongThresholdChanged, this, [this](double v) {
        m_canvas->setStrongThreshold(v);
        updateStatus();
    });

    // 画布信号
    connect(m_canvas, &Point3dCanvas::hoverChanged, this, &Point3dWindow::updateStatus);
    connect(m_canvas, &Point3dCanvas::statsChanged, this, &Point3dWindow::updateStatus);
    connect(m_canvas, &Point3dCanvas::cameraChanged, this, &Point3dWindow::saveSettings);
    connect(m_canvas, &Point3dCanvas::trackFocusChanged,
            this, &Point3dWindow::trackFocusChanged);
    connect(m_canvas, &Point3dCanvas::trackActivated,
            this, &Point3dWindow::trackActivated);
    // 左键单击某条航迹：本窗口单独显示这一条，并通知主窗口让地图也单独显示它（双向联动）
    connect(m_canvas, &Point3dCanvas::trackClicked, this,
            [this](int batchNo, TrackSource source, qint64 t0, qint64 t1) {
                if (batchNo <= 0)
                    return;
                QSet<int> only;
                only.insert(batchNo);
                m_canvas->setIsolatedBatches(only);
                updateStatus();
                emit trackActivated(batchNo, source, t0, t1);
            });

    // 本窗口独立播放控制
    connect(m_playBtn, &QPushButton::clicked, this, &Point3dWindow::togglePlay);
    // 进度条：拖动暂停、松手恢复；每一格都是一帧
    connect(m_seekSlider, &QSlider::sliderPressed, this, [this]() {
        m_resumeAfterSeek = m_playing;
        setPlaying(false);
    });
    connect(m_seekSlider, &QSlider::sliderReleased, this, [this]() {
        if (m_resumeAfterSeek)
            setPlaying(true);
        m_resumeAfterSeek = false;
    });
    connect(m_seekSlider, &QSlider::valueChanged, this, [this](int v) {
        if (!m_syncing)
            seekToFrame(v);
    });
    connect(m_speedCombo, &QComboBox::currentTextChanged, this, [this](const QString &text) {
        if (m_restoring)
            return;
        bool ok = false;
        const double v = text.toDouble(&ok);
        if (ok && v > 0.0)
            setSpeed(v);
    });

    // 回放心跳：每 16ms 检查当前帧是否该换下一帧（按帧间隔 / 倍速）
    m_playTimer = new QTimer(this);
    m_playTimer->setInterval(16);
    m_playTimer->setTimerType(Qt::PreciseTimer);
    connect(m_playTimer, &QTimer::timeout, this, &Point3dWindow::onPlayTick);

    // 快捷键：P / T / L / 空格（窗口内生效）
    auto bindKey = [this](Qt::Key key, const std::function<void()> &fn) {
        QShortcut *sc = new QShortcut(QKeySequence(key), this);
        sc->setContext(Qt::WidgetWithChildrenShortcut);
        connect(sc, &QShortcut::activated, this, fn);
    };
    bindKey(Qt::Key_P, [this]() { m_sideSettings->toggleDetections(); });
    bindKey(Qt::Key_T, [this]() { m_sideSettings->toggleTracks(); });
    bindKey(Qt::Key_L, [this]() { m_sideSettings->toggleLabels(); });
    bindKey(Qt::Key_Space, [this]() { togglePlay(); });

    // 没有点迹数据前先禁用播放控制（导入完成后 syncPlaybackUi 打开）
    m_playBtn->setEnabled(false);
    m_seekSlider->setEnabled(false);
    m_speedCombo->setEnabled(true);

    // ---- 恢复全局默认参数 ----
    // 左侧侧栏：恢复可见性 + 当前页（0=设置，1=高级）
    setSidePanelPage(setting("point3d.side_page", 0).toInt(),
                     setting("point3d.side_panel", true).toBool());
    m_sideSettings->restoreDefaults();
    syncViewMenuChecks(m_sideSettings->viewMode());

    // 强连续阈值（只用于状态统计）
    m_canvas->setStrongThreshold(setting("point3d.strong_threshold", 0.70).toDouble());

    // 本窗口回放的倍速（只影响本窗口）
    m_speed = qBound(0.01, setting("point3d.play_speed", 100.0).toDouble(), 1.0e6);
    m_speedCombo->setEditText(QString::number(m_speed, 'g', 6));

    const double camYaw = setting("point3d.camera_yaw", 42.0).toDouble();
    const double camPitch = setting("point3d.camera_pitch", 23.0).toDouble();
    const double camZoom = setting("point3d.camera_zoom", 1.0).toDouble();
    const double camPanX = setting("point3d.camera_pan_x", 0.0).toDouble();
    const double camPanY = setting("point3d.camera_pan_y", 0.0).toDouble();
    m_canvas->setCamera(camYaw, camPitch, camZoom, camPanX, camPanY);

    // 航迹选择属于单个窗口：只随窗口会话（point3d.windows）保存，不写全局默认，
    // 否则新开的窗口会莫名继承别的窗口选过的航迹。
    m_restoring = false;
    syncPlaybackUi();
    updatePlaybackUi();
    updateStatus();
}

// ---------------------------------------------------------------------------
//  点迹导入与数据生命周期
// ---------------------------------------------------------------------------
void Point3dWindow::importDetections()
{
    QString dir = setting("point3d.last_dir", QDir::homePath()).toString();
    const QString pattern = setting("point3d.file_pattern", QStringLiteral("detPoint_*.mat")).toString();
    const QString filter = QStringLiteral("MATLAB 数据 (%1);;所有文件 (*)").arg(pattern);
    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("导入点迹数据"), dir, filter);
    if (files.isEmpty())
        return;
    app::setValue(QStringLiteral("point3d.last_dir"), QFileInfo(files.first()).absolutePath());
    loadDetectionFiles(files);
}

void Point3dWindow::loadDetectionFiles(const QStringList &paths)
{
    if (paths.isEmpty())
        return;
    if (m_job) {
        m_job->requestCancel();
        m_job->wait();
        m_job->deleteLater();
        m_job = nullptr;
    }

    QStringList files;
    QSet<QString> seen;
    for (const QString &p : paths) {
        const QString c = QFileInfo(p).canonicalFilePath();
        const QString key = c.isEmpty() ? p : c;
        if (!seen.contains(key)) {
            seen.insert(key);
            files << key;
        }
    }
    if (files.isEmpty())
        return;

    Point3dImportOptions opt;
    opt.detVariable = setting("point3d.det_variable", QStringLiteral("DetPoint")).toString();
    opt.sysVariable = setting("point3d.syspara_variable", QStringLiteral("sysPara")).toString();
    opt.azimuthField = setting("point3d.azimuth_field", QStringLiteral("az")).toString();
    opt.maxFrames = setting("point3d.max_frames", 0).toInt();
    opt.continuity.frameGap = setting("point3d.continuity_frame_gap", 3).toInt();
    opt.continuity.rangeScale = setting("point3d.continuity_range_scale", 80.0).toDouble();
    opt.continuity.dopplerScale = setting("point3d.continuity_doppler_scale", 10.0).toDouble();
    opt.continuity.azimuthScale = setting("point3d.continuity_azimuth_scale", 6.0).toDouble();

    m_pendingFiles = files;
    m_job = new Point3dImportJob(files, opt, this);
    connect(m_job, &Point3dImportJob::progressChanged, this,
            [this](int pct) { setLoadingUi(true, pct); });
    connect(m_job, &Point3dImportJob::importDone, this, &Point3dWindow::onImportDone);
    setLoadingUi(true, 0);
    m_job->start();
}

void Point3dWindow::onImportDone()
{
    Point3dImportJob *job = m_job;
    m_job = nullptr;
    if (!job)
        return;
    job->wait();
    setLoadingUi(false, 100);

    if (job->cancelled()) {
        m_status->setText(QStringLiteral("导入已取消"));
        job->deleteLater();
        return;
    }
    if (job->dataset().frames.isEmpty()) {
        const QString summary = job->errors().isEmpty()
            ? QStringLiteral("没有解析到有效点迹")
            : job->errors().first();
        m_status->setText(QStringLiteral("导入失败：%1").arg(summary));
        if (!app::testMode())
            QMessageBox::warning(this, QStringLiteral("导入失败"),
                                 job->errors().join(QLatin1Char('\n')));
        job->deleteLater();
        return;
    }

    m_data = job->dataset();
    setPlaying(false);
    m_canvas->setDataset(&m_data);
    m_canvas->setHighlightedBatch(-1);
    m_files = m_pendingFiles;
    // 重新导入点迹后，已选数据库航迹需要按新帧重新映射 Z 值
    reloadOverlaysFromKeys();
    // 数据变了：各视图的区域区间按新量程夹紧/复位，再套用到画布
    m_sideSettings->revalidateRegions();
    m_sideSettings->loadRegionForCurrentView();
    syncPlaybackUi();
    updatePlaybackUi();

    // 持久化文件清单
    QJsonArray arr;
    for (const QString &f : qAsConst(m_files))
        arr.append(f);
    app::setValue(QStringLiteral("point3d.files"),
                  QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));

    QString msg = QStringLiteral("已导入 %1 帧，%2 个点，耗时 %3 ms")
                      .arg(m_data.frames.size())
                      .arg([this]() {
                          int n = 0;
                          for (const DetPointFrame &f : qAsConst(m_data.frames))
                              n += f.points.size();
                          return n;
                      }())
                      .arg(job->elapsedMs());
    if (!job->errors().isEmpty())
        msg += QStringLiteral("；%1 个文件失败").arg(job->errors().size());
    m_status->setText(msg);
    job->deleteLater();
    emit dataChanged();
}

void Point3dWindow::setLoadingUi(bool loading, int percent)
{
    m_progress->setVisible(loading);
    m_progress->setValue(qBound(0, percent, 100));
    if (m_importAction)
        m_importAction->setEnabled(!loading);
    if (loading)
        m_status->setText(QStringLiteral("正在解析点迹数据… %1%").arg(qBound(0, percent, 100)));
}

void Point3dWindow::updateStatus()
{
    if (m_job)
        return;
    if (m_data.frames.isEmpty())
        return;                       // 保留导入完成/失败时的提示文字
    QString text = m_canvas->statusText();
    const QSet<int> isolated = m_canvas->isolatedBatches();
    if (!isolated.isEmpty())
        text += QStringLiteral(" | 按地图选择单独显示 %1 条").arg(isolated.size());
    if (m_skippedTracks > 0)
        text += QStringLiteral(" | %1 条航迹与点迹时间不匹配已忽略").arg(m_skippedTracks);
    if (m_sideSettings && m_sideSettings->regionAppliedForView(m_canvas->viewMode()))
        text += QStringLiteral(" | 区域筛选已应用");
    m_status->setText(text);
}

void Point3dWindow::setHighlightedBatch(int batchNo)
{
    m_canvas->setHighlightedBatch(batchNo);
    updateStatus();
}

int Point3dWindow::highlightedBatch() const
{
    return m_canvas->highlightedBatch();
}

void Point3dWindow::setCurrentFrame(int index)
{
    m_canvas->setCurrentFrame(index);
    syncPlaybackUi();
    updateStatus();
}

void Point3dWindow::cancelImport()
{
    if (m_job)
        m_job->requestCancel();
}

// 连续性参数改动后（防抖）按新参数重算当前数据，点的大小/透明度立刻跟着变
void Point3dWindow::recomputeContinuityNow(const ContinuityConfig &cfg)
{
    if (m_data.frames.isEmpty())
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    computeContinuity(m_data.frames, cfg);
    QApplication::restoreOverrideCursor();
    m_canvas->update();
    m_status->setText(QStringLiteral("连续性参数已按新设置重算"));
    updateStatus();
}

// 顶部「设置」「高级」：展开对应页；再点同一个按钮或 show=false 时收起
void Point3dWindow::setSidePanelPage(int page, bool show)
{
    m_sideStack->setCurrentIndex(qBound(0, page, 1));
    m_sidePanel->setVisible(show);
    const bool visible = !m_sidePanel->isHidden();
    const bool settingsActive = visible && m_sideStack->currentIndex() == 0;
    m_settingsBtn->setChecked(settingsActive);
    m_advancedBtn->setChecked(visible && m_sideStack->currentIndex() == 1);
    m_sideSettings->setPanelActive(settingsActive);
    if (!m_restoring) {
        if (!visible)
            m_canvas->setRegionBox(Point3dRegion());
        else if (settingsActive)
            m_sideSettings->loadRegionForCurrentView();
        app::setValue(QStringLiteral("point3d.side_panel"), visible);
        app::setValue(QStringLiteral("point3d.side_page"), m_sideStack->currentIndex());
    }
}

void Point3dWindow::exportCanvasImage()
{
    const QString dir = setting("point3d.last_dir", QDir::homePath()).toString();
    const QString def = QDir(dir).filePath(
        QStringLiteral("point3d_%1.png")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出当前画面"), def,
                                                QStringLiteral("PNG 图片 (*.png)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(QLatin1String(".png"), Qt::CaseInsensitive))
        path += QStringLiteral(".png");
    if (m_canvas->grab().save(path)) {
        app::setValue(QStringLiteral("point3d.export_file"), path);
        m_status->setText(QStringLiteral("已导出画面：%1").arg(path));
    } else if (!app::testMode()) {
        QMessageBox::warning(this, QStringLiteral("导出失败"),
                             QStringLiteral("无法写入：%1").arg(path));
    }
}

void Point3dWindow::saveSettings()
{
    if (m_restoring)
        return;
    double yaw = 0, pitch = 0, zoom = 0, panX = 0, panY = 0;
    m_canvas->camera(&yaw, &pitch, &zoom, &panX, &panY);
    app::setValue(QStringLiteral("point3d.camera_yaw"), yaw);
    app::setValue(QStringLiteral("point3d.camera_pitch"), pitch);
    app::setValue(QStringLiteral("point3d.camera_zoom"), zoom);
    app::setValue(QStringLiteral("point3d.camera_pan_x"), panX);
    app::setValue(QStringLiteral("point3d.camera_pan_y"), panY);
    app::setValue(QStringLiteral("point3d.window_geometry"), saveGeometry());
}

void Point3dWindow::closeEvent(QCloseEvent *e)
{
    if (m_job) {
        m_job->requestCancel();
        m_job->wait();
        m_job->deleteLater();
        m_job = nullptr;
    }
    saveSettings();
    app::flushSettings();
    emit closed();
    QWidget::closeEvent(e);
}
