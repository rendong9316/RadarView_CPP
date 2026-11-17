#include "point3dwindow.h"

#include "apppaths.h"
#include "point3dcanvas.h"
#include "point3dimportjob.h"
#include "point3dparsers.h"
#include "replaycontroller.h"
#include "theme.h"
#include "trackdb.h"
#include "trackpicker.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QShortcut>
#include <QSlider>
#include <QSpinBox>
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
        "QWidget#point3dToolbar { background: var(--bg-secondary);"
        " border-bottom: 1px solid var(--border-primary); }"
        "QWidget#point3dStatus { background: var(--bg-secondary);"
        " border-top: 1px solid var(--border-primary); }"
        "QLabel { color: var(--text-secondary); font-family: %1; font-size: 12px; }"
        "QPushButton { color: var(--text-secondary); background: var(--button-bg);"
        " border: 1px solid var(--border-primary); border-radius: 3px; padding: 4px 10px;"
        " font-family: %1; font-size: 12px; }"
        "QPushButton:hover { background: var(--button-hover); color: var(--text-primary); }"
        "QPushButton:checked { background: var(--accent-primary); color: #ffffff;"
        " border-color: var(--accent-primary); }"
        "QComboBox, QSpinBox { color: var(--text-primary); background: var(--input-bg);"
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

// 接入主窗口的全局回放时钟：窗口内所有播放操作都直接驱动它
void Point3dWindow::attachReplay(ReplayController *replay)
{
    if (!replay)
        return;
    if (m_replay == replay)
        return;
    if (m_replay)
        disconnect(m_replay, nullptr, this, nullptr);
    m_replay = replay;
    connect(replay, &ReplayController::timeChanged, this, &Point3dWindow::onReplayTime);
    connect(replay, &ReplayController::stateChanged, this, &Point3dWindow::updatePlaybackUi);

    m_playBtn->setEnabled(true);
    m_seekSlider->setEnabled(true);
    m_speedCombo->setEnabled(true);

    m_restoring = true;
    m_speedCombo->setEditText(QString::number(replay->speed(), 'g', 6));
    m_restoring = false;

    onReplayTime(replay->current());
    updatePlaybackUi();
}

qint64 Point3dWindow::currentTimeMs() const
{
    return m_replay ? m_replay->current() : m_data.maxTime;
}

int Point3dWindow::nearestFrame(qint64 ms) const
{
    const QVector<DetPointFrame> &frames = m_data.frames;
    if (frames.isEmpty())
        return 0;
    int lo = 0, hi = frames.size();
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (frames.at(mid).timeMs < ms)
            lo = mid + 1;
        else
            hi = mid;
    }
    int best = qBound(0, lo, frames.size() - 1);
    if (best > 0 && qAbs(frames.at(best - 1).timeMs - ms)
                        <= qAbs(frames.at(best).timeMs - ms))
        best -= 1;
    return best;
}

void Point3dWindow::onReplayTime(qint64 ms)
{
    m_outOfRange = false;
    if (!m_data.frames.isEmpty()) {
        if (ms < m_data.minTime || ms > m_data.maxTime)
            m_outOfRange = true;
        else
            m_canvas->setCurrentFrame(nearestFrame(ms));
    }

    m_syncing = true;
    if (m_replay && m_replay->end() > m_replay->start()) {
        m_seekSlider->setValue(int(std::lround(m_replay->progress() * 1000.0)));
        m_timeLabel->setText(formatBeijingTime(ms));
    } else {
        m_seekSlider->setValue(0);
        m_timeLabel->setText(QStringLiteral("--"));
    }
    m_syncing = false;
    updateStatus();
}

void Point3dWindow::seekFromSlider(int value)
{
    if (m_syncing || !m_replay)
        return;
    m_replay->seek(qBound(0.0, value / 1000.0, 1.0));
}

void Point3dWindow::updatePlaybackUi()
{
    if (!m_replay)
        return;
    m_playBtn->setText(m_replay->isPlaying() ? QStringLiteral("暂停")
                                             : QStringLiteral("播放"));
}

void Point3dWindow::setTrackColorProvider(
    const std::function<QColor(TrackSource, const QString &)> &provider)
{
    m_colorProvider = provider;
}

void Point3dWindow::reloadOverlaysFromKeys()
{
    m_data.tracks.clear();
    if (!m_trackKeys.isEmpty() && !m_data.frames.isEmpty() && trackdb::isOpen()) {
        const QVector<Track> tracks = trackdb::loadTracks(m_trackKeys);
        for (const Track &t : tracks) {
            TrackOverlay ov;
            const QColor color = m_colorProvider ? m_colorProvider(t.source, t.fileName)
                                                 : QColor();
            if (overlayFromTrack(t, m_data.frames, &ov, color))
                m_data.tracks.append(ov);
        }
    }
    m_canvas->refreshLimits();
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

    QJsonArray arr;
    for (const QString &key : qAsConst(m_trackKeys))
        arr.append(key);
    app::setValue(QStringLiteral("point3d.selected_tracks"),
                  QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));

    reloadOverlaysFromKeys();
    updateStatus();
}

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

    double yaw = 42.0, pitch = 23.0, zoom = 1.0;
    m_canvas->camera(&yaw, &pitch, &zoom);
    QJsonObject cam;
    cam.insert(QStringLiteral("yaw"), yaw);
    cam.insert(QStringLiteral("pitch"), pitch);
    cam.insert(QStringLiteral("zoom"), zoom);
    o.insert(QStringLiteral("camera"), cam);

    o.insert(QStringLiteral("view_mode"), m_viewCombo->currentData().toInt());
    o.insert(QStringLiteral("window_length"), m_canvas->windowLength());
    o.insert(QStringLiteral("geometry"),
             QString::fromLatin1(saveGeometry().toBase64()));
    o.insert(QStringLiteral("number"), m_number);
    return o;
}

void Point3dWindow::restoreSession(const QJsonObject &obj)
{
    m_restoring = true;
    if (obj.contains(QStringLiteral("number")))
        setNumber(obj.value(QStringLiteral("number")).toInt(m_number));

    const QByteArray geo = QByteArray::fromBase64(
        obj.value(QStringLiteral("geometry")).toString().toLatin1());
    if (!geo.isEmpty())
        restoreGeometry(geo);

    const int mode = obj.value(QStringLiteral("view_mode")).toInt(
        int(Point3dViewMode::RVF));
    const int viewIdx = m_viewCombo->findData(mode);
    if (viewIdx >= 0)
        m_viewCombo->setCurrentIndex(viewIdx);
    m_canvas->setViewMode(Point3dViewMode(m_viewCombo->currentData().toInt()));

    const int len = obj.value(QStringLiteral("window_length")).toInt(0);
    int presetIdx = -1;
    for (int i = 0; i < m_windowCombo->count(); ++i)
        if (m_windowCombo->itemData(i).toInt() == len && len > 0) {
            presetIdx = i;
            break;
        }
    if (len == 0) {
        presetIdx = m_windowCombo->count() - 2;
    } else if (presetIdx < 0) {
        presetIdx = m_windowCombo->count() - 1;
        m_windowSpin->setValue(len);
        m_windowSpin->setVisible(true);
    }
    m_windowCombo->setCurrentIndex(presetIdx);
    m_canvas->setWindowLength(len);

    const QJsonObject cam = obj.value(QStringLiteral("camera")).toObject();
    if (!cam.isEmpty())
        m_canvas->setCamera(cam.value(QStringLiteral("yaw")).toDouble(42.0),
                            cam.value(QStringLiteral("pitch")).toDouble(23.0),
                            cam.value(QStringLiteral("zoom")).toDouble(1.0));

    m_files.clear();
    for (const QJsonValue &v : obj.value(QStringLiteral("files")).toArray())
        m_files << v.toString();
    m_trackKeys.clear();
    for (const QJsonValue &v : obj.value(QStringLiteral("tracks")).toArray())
        m_trackKeys << v.toString();

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

void Point3dWindow::buildUi()
{
    m_restoring = true;

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- 工具栏 ----
    m_toolbar = new QWidget(this);
    m_toolbar->setObjectName(QStringLiteral("point3dToolbar"));
    QHBoxLayout *bar = new QHBoxLayout(m_toolbar);
    bar->setContentsMargins(ui::sz(8), ui::sz(6), ui::sz(8), ui::sz(6));
    bar->setSpacing(ui::sz(8));

    m_importBtn = new QPushButton(QStringLiteral("导入点迹…"), m_toolbar);
    m_importBtn->setToolTip(QStringLiteral("选择一个或多个 detPoint_*.mat 文件（可多选，单个也可以）"));
    bar->addWidget(m_importBtn);

    QPushButton *pickTracksBtn = new QPushButton(QStringLiteral("选择航迹…"), m_toolbar);
    pickTracksBtn->setToolTip(QStringLiteral("从地球侧数据库选择要叠加的航迹（与地图共用批次与颜色）"));
    bar->addWidget(pickTracksBtn);

    QLabel *viewLabel = new QLabel(QStringLiteral("视图"), m_toolbar);
    bar->addWidget(viewLabel);
    m_viewCombo = new QComboBox(m_toolbar);
    m_viewCombo->addItem(QStringLiteral("距离–速度–帧号"), int(Point3dViewMode::RVF));
    m_viewCombo->addItem(QStringLiteral("距离–方位–帧号"), int(Point3dViewMode::RAF));
    m_viewCombo->addItem(QStringLiteral("速度–方位–帧号"), int(Point3dViewMode::VAF));
    m_viewCombo->setToolTip(QStringLiteral("三维视图的 X/Y 轴组合，Z 轴恒为帧号"));
    bar->addWidget(m_viewCombo);

    QLabel *windowLabel = new QLabel(QStringLiteral("窗口"), m_toolbar);
    bar->addWidget(windowLabel);
    m_windowCombo = new QComboBox(m_toolbar);
    const int presets[] = { 20, 40, 70, 150 };
    for (int v : presets)
        m_windowCombo->addItem(QStringLiteral("%1 帧").arg(v), v);
    m_windowCombo->addItem(QStringLiteral("全部"), 0);
    m_windowCombo->addItem(QStringLiteral("自定义…"), -1);
    m_windowCombo->setToolTip(QStringLiteral("同时显示最近多少帧的点迹（「全部」= 整个数据集）"));
    bar->addWidget(m_windowCombo);
    m_windowSpin = new QSpinBox(m_toolbar);
    m_windowSpin->setRange(1, 100000);
    m_windowSpin->setValue(70);
    m_windowSpin->setVisible(false);
    m_windowSpin->setToolTip(QStringLiteral("自定义滑动窗口帧数"));
    bar->addWidget(m_windowSpin);

    QLabel *sourceLabel = new QLabel(QStringLiteral("点源"), m_toolbar);
    bar->addWidget(sourceLabel);
    m_sourceCombo = new QComboBox(m_toolbar);
    m_sourceCombo->addItem(QStringLiteral("自动"), int(Point3dTrackSource::Auto));
    m_sourceCombo->addItem(QStringLiteral("平滑"), int(Point3dTrackSource::Smooth));
    m_sourceCombo->addItem(QStringLiteral("关联"), int(Point3dTrackSource::Associated));
    m_sourceCombo->addItem(QStringLiteral("预测"), int(Point3dTrackSource::Predicted));
    m_sourceCombo->setToolTip(QStringLiteral(
        "数据库航迹来源：自动=全部；平滑=Radar 批次；关联=RadarRaw 批次；\n"
        "预测点列未入库，选择「预测」时不显示航迹"));
    bar->addWidget(m_sourceCombo);

    bar->addSpacing(ui::sz(6));
    m_pointBtn = new QPushButton(QStringLiteral("点迹 [P]"), m_toolbar);
    m_pointBtn->setCheckable(true);
    m_pointBtn->setChecked(true);
    m_pointBtn->setToolTip(QStringLiteral("显示 / 隐藏点迹（P）"));
    bar->addWidget(m_pointBtn);
    m_trackBtn = new QPushButton(QStringLiteral("航迹 [T]"), m_toolbar);
    m_trackBtn->setCheckable(true);
    m_trackBtn->setChecked(true);
    m_trackBtn->setToolTip(QStringLiteral("显示 / 隐藏航迹（T）"));
    bar->addWidget(m_trackBtn);
    m_labelBtn = new QPushButton(QStringLiteral("标签 [L]"), m_toolbar);
    m_labelBtn->setCheckable(true);
    m_labelBtn->setChecked(true);
    m_labelBtn->setToolTip(QStringLiteral("显示 / 隐藏航迹编号（L）"));
    bar->addWidget(m_labelBtn);

    QPushButton *advBtn = new QPushButton(QStringLiteral("高级参数…"), m_toolbar);
    advBtn->setToolTip(QStringLiteral("MATLAB 工具的全部可调参数（变量名、连续性、帧数等）"));
    bar->addWidget(advBtn);

    QPushButton *exportBtn = new QPushButton(QStringLiteral("导出 PNG…"), m_toolbar);
    exportBtn->setToolTip(QStringLiteral("把当前画面保存为 PNG（对应 ExportFinalFrame）"));
    bar->addWidget(exportBtn);

    bar->addSpacing(ui::sz(6));
    m_playBtn = new QPushButton(QStringLiteral("播放"), m_toolbar);
    m_playBtn->setToolTip(QStringLiteral("播放 / 暂停全局时间轴（空格）"));
    bar->addWidget(m_playBtn);

    m_seekSlider = new QSlider(Qt::Horizontal, m_toolbar);
    m_seekSlider->setRange(0, 1000);
    m_seekSlider->setMinimumWidth(ui::sz(160));
    m_seekSlider->setToolTip(QStringLiteral("拖动跳转全局时间轴（与主窗口进度条同步）"));
    bar->addWidget(m_seekSlider, 1);

    m_timeLabel = new QLabel(QStringLiteral("--"), m_toolbar);
    m_timeLabel->setMinimumWidth(ui::sz(150));
    bar->addWidget(m_timeLabel);

    QLabel *speedLabel = new QLabel(QStringLiteral("倍速"), m_toolbar);
    bar->addWidget(speedLabel);
    m_speedCombo = new QComboBox(m_toolbar);
    m_speedCombo->setEditable(true);
    for (int s : { 1, 2, 5, 10, 20, 50, 100, 300, 500, 2000 })
        m_speedCombo->addItem(QString::number(s));
    m_speedCombo->setToolTip(QStringLiteral("回放倍速（作用于全局时间轴，与主窗口共用）"));
    bar->addWidget(m_speedCombo);

    m_progress = new QProgressBar(m_toolbar);
    m_progress->setRange(0, 100);
    m_progress->setTextVisible(false);
    m_progress->setFixedWidth(ui::sz(140));
    m_progress->setVisible(false);
    bar->addWidget(m_progress);
    root->addWidget(m_toolbar);

    // ---- 画布 ----
    m_canvas = new Point3dCanvas(this);
    root->addWidget(m_canvas, 1);

    // ---- 底部状态 ----
    m_statusBar = new QWidget(this);
    m_statusBar->setObjectName(QStringLiteral("point3dStatus"));
    QHBoxLayout *statusLay = new QHBoxLayout(m_statusBar);
    statusLay->setContentsMargins(ui::sz(10), ui::sz(3), ui::sz(10), ui::sz(3));
    m_status = new QLabel(m_statusBar);
    m_status->setText(QStringLiteral("未导入点迹数据"));
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    statusLay->addWidget(m_status, 1);
    root->addWidget(m_statusBar);

    setThemedStyle(this, toolbarQss());

    // ---- 连接 ----
    connect(m_importBtn, &QPushButton::clicked, this, &Point3dWindow::importDetections);
    connect(pickTracksBtn, &QPushButton::clicked, this, &Point3dWindow::chooseTracksFromDb);
    connect(advBtn, &QPushButton::clicked, this, &Point3dWindow::openAdvancedSettings);
    connect(exportBtn, &QPushButton::clicked, this, &Point3dWindow::exportCanvasImage);
    connect(m_sourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        const Point3dTrackSource src = Point3dTrackSource(m_sourceCombo->currentData().toInt());
        m_canvas->setTrackSource(src);
        if (!m_restoring) {
            const char *key = src == Point3dTrackSource::Auto ? "auto"
                            : src == Point3dTrackSource::Smooth ? "smooth"
                            : src == Point3dTrackSource::Predicted ? "predicted" : "associated";
            app::setValue(QStringLiteral("point3d.track_source"), QString::fromLatin1(key));
        }
        updateStatus();
    });
    connect(m_viewCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &Point3dWindow::onViewModeChanged);
    connect(m_windowCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &Point3dWindow::onWindowLengthChanged);
    connect(m_windowSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int v) {
                if (!m_restoring)
                    applyWindowLength(v);
            });
    connect(m_pointBtn, &QPushButton::toggled, this, [this](bool on) {
        m_canvas->setShowDetections(on);
        if (!m_restoring) {
            app::setValue(QStringLiteral("point3d.show_detections"), on);
            updateStatus();
        }
    });
    connect(m_trackBtn, &QPushButton::toggled, this, [this](bool on) {
        m_canvas->setShowTracks(on);
        if (!m_restoring) {
            app::setValue(QStringLiteral("point3d.show_tracks"), on);
            updateStatus();
        }
    });
    connect(m_labelBtn, &QPushButton::toggled, this, [this](bool on) {
        m_canvas->setShowLabels(on);
        if (!m_restoring)
            app::setValue(QStringLiteral("point3d.show_labels"), on);
    });

    connect(m_canvas, &Point3dCanvas::hoverChanged, this, &Point3dWindow::updateStatus);
    connect(m_canvas, &Point3dCanvas::statsChanged, this, &Point3dWindow::updateStatus);
    connect(m_canvas, &Point3dCanvas::cameraChanged, this, &Point3dWindow::saveSettings);
    connect(m_canvas, &Point3dCanvas::trackFocusChanged,
            this, &Point3dWindow::trackFocusChanged);
    connect(m_canvas, &Point3dCanvas::trackActivated,
            this, &Point3dWindow::trackActivated);

    // 播放控制：全部驱动主窗口的 ReplayController（全局共用时间轴）
    connect(m_playBtn, &QPushButton::clicked, this, [this]() {
        if (m_replay)
            m_replay->togglePlay();
    });
    connect(m_seekSlider, &QSlider::sliderMoved, this, &Point3dWindow::seekFromSlider);
    connect(m_speedCombo, &QComboBox::currentTextChanged, this, [this](const QString &text) {
        if (m_restoring || !m_replay)
            return;
        bool ok = false;
        const double v = text.toDouble(&ok);
        if (ok && v > 0.0)
            m_replay->setSpeed(v);
    });

    // 快捷键：P / T / L（窗口内生效）
    auto bindKey = [this](Qt::Key key, const std::function<void()> &fn) {
        QShortcut *sc = new QShortcut(QKeySequence(key), this);
        sc->setContext(Qt::WidgetWithChildrenShortcut);
        connect(sc, &QShortcut::activated, this, fn);
    };
    bindKey(Qt::Key_P, [this]() { m_pointBtn->toggle(); });
    bindKey(Qt::Key_T, [this]() { m_trackBtn->toggle(); });
    bindKey(Qt::Key_L, [this]() { m_labelBtn->toggle(); });
    bindKey(Qt::Key_Space, [this]() {
        if (m_replay)
            m_replay->togglePlay();
    });

    // 未接入全局时钟前先禁用播放控制（由 attachReplay 打开）
    m_playBtn->setEnabled(false);
    m_seekSlider->setEnabled(false);
    m_speedCombo->setEnabled(false);

    // ---- 恢复全局默认参数 ----
    const QString view = setting("point3d.view_mode", QStringLiteral("rvf")).toString();
    const int viewIdx = view == QLatin1String("raf") ? 1 : view == QLatin1String("vaf") ? 2 : 0;
    m_viewCombo->setCurrentIndex(viewIdx);
    m_canvas->setViewMode(Point3dViewMode(m_viewCombo->currentData().toInt()));

    m_pointBtn->setChecked(setting("point3d.show_detections", true).toBool());
    m_trackBtn->setChecked(setting("point3d.show_tracks", true).toBool());
    m_labelBtn->setChecked(setting("point3d.show_labels", true).toBool());
    m_canvas->setShowDetections(m_pointBtn->isChecked());
    m_canvas->setShowTracks(m_trackBtn->isChecked());
    m_canvas->setShowLabels(m_labelBtn->isChecked());
    const QString src = setting("point3d.track_source", QStringLiteral("associated")).toString();
    Point3dTrackSource trackSource = Point3dTrackSource::Associated;
    if (src == QLatin1String("auto"))
        trackSource = Point3dTrackSource::Auto;
    else if (src == QLatin1String("smooth"))
        trackSource = Point3dTrackSource::Smooth;
    else if (src == QLatin1String("predicted"))
        trackSource = Point3dTrackSource::Predicted;
    m_sourceCombo->setCurrentIndex(qMax(0, m_sourceCombo->findData(int(trackSource))));
    m_canvas->setTrackSource(trackSource);
    m_canvas->setTrackLineWidth(setting("point3d.track_line_width", 2.0).toDouble());

    const double camYaw = setting("point3d.camera_yaw", 42.0).toDouble();
    const double camPitch = setting("point3d.camera_pitch", 23.0).toDouble();
    const double camZoom = setting("point3d.camera_zoom", 1.0).toDouble();
    m_canvas->setCamera(camYaw, camPitch, camZoom);

    // 滑动窗口
    const int len = setting("point3d.window_length", 0).toInt();
    int presetIdx = -1;
    for (int i = 0; i < m_windowCombo->count(); ++i)
        if (m_windowCombo->itemData(i).toInt() == len && len > 0) {
            presetIdx = i;
            break;
        }
    if (len == 0) {
        presetIdx = m_windowCombo->count() - 2;          // 全部
    } else if (presetIdx < 0) {
        presetIdx = m_windowCombo->count() - 1;          // 自定义…
        m_windowSpin->setValue(len);
        m_windowSpin->setVisible(true);
    }
    m_windowCombo->setCurrentIndex(presetIdx);
    m_canvas->setWindowLength(len);

    // 恢复上次从数据库选择的航迹 key（数据导入后重新映射 Z 值）
    const QString keysJson = setting("point3d.selected_tracks", QString()).toString();
    for (const QJsonValue &v : QJsonDocument::fromJson(keysJson.toUtf8()).array())
        m_trackKeys << v.toString();

    m_restoring = false;
    updateStatus();
}

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
    m_canvas->setDataset(&m_data);
    m_canvas->setHighlightedBatch(-1);
    m_files = m_pendingFiles;
    // 重新导入点迹后，已选数据库航迹需要按新帧重新映射 Z 值
    reloadOverlaysFromKeys();
    if (m_replay)
        onReplayTime(m_replay->current());

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

void Point3dWindow::onViewModeChanged()
{
    const int mode = m_viewCombo->currentData().toInt();
    m_canvas->setViewMode(Point3dViewMode(mode));
    if (m_restoring)
        return;
    const char *key = mode == int(Point3dViewMode::RAF) ? "raf"
                    : mode == int(Point3dViewMode::VAF) ? "vaf" : "rvf";
    app::setValue(QStringLiteral("point3d.view_mode"), QString::fromLatin1(key));
    updateStatus();
}

void Point3dWindow::onWindowLengthChanged()
{
    const int data = m_windowCombo->currentData().toInt();
    if (data == -1) {
        m_windowSpin->setVisible(true);
        if (!m_restoring)
            applyWindowLength(m_windowSpin->value());
        return;
    }
    m_windowSpin->setVisible(false);
    if (!m_restoring)
        applyWindowLength(data);
}

void Point3dWindow::applyWindowLength(int len)
{
    m_canvas->setWindowLength(len);
    app::setValue(QStringLiteral("point3d.window_length"), len);
    updateStatus();
}

void Point3dWindow::setLoadingUi(bool loading, int percent)
{
    m_progress->setVisible(loading);
    m_progress->setValue(qBound(0, percent, 100));
    m_importBtn->setEnabled(!loading);
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
    if (m_outOfRange)
        text += QStringLiteral(" | 当前时间不在本数据范围内");
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

int Point3dWindow::currentFrame() const
{
    return m_canvas->currentFrame();
}

void Point3dWindow::setCurrentFrame(int index)
{
    m_canvas->setCurrentFrame(index);
    updateStatus();
}

void Point3dWindow::cancelImport()
{
    if (m_job)
        m_job->requestCancel();
}

void Point3dWindow::openAdvancedSettings()
{
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("点迹可视化高级参数"));
    dlg.setModal(true);
    QFormLayout *form = new QFormLayout(&dlg);

    auto line = [&dlg](const QString &text) {
        QLineEdit *e = new QLineEdit(&dlg);
        e->setText(text);
        return e;
    };
    auto spin = [&dlg](int lo, int hi, int value) {
        QSpinBox *s = new QSpinBox(&dlg);
        s->setRange(lo, hi);
        s->setValue(value);
        return s;
    };
    auto dspin = [&dlg](double lo, double hi, int decimals, double value, double step = 0.1) {
        QDoubleSpinBox *s = new QDoubleSpinBox(&dlg);
        s->setRange(lo, hi);
        s->setDecimals(decimals);
        s->setSingleStep(step);
        s->setValue(value);
        return s;
    };

    QLineEdit *detVar = line(setting("point3d.det_variable", QStringLiteral("DetPoint")).toString());
    QLineEdit *sysVar = line(setting("point3d.syspara_variable", QStringLiteral("sysPara")).toString());
    QComboBox *azField = new QComboBox(&dlg);
    azField->setEditable(true);
    azField->addItems(QStringList() << QStringLiteral("az") << QStringLiteral("theta"));
    azField->setCurrentText(setting("point3d.azimuth_field", QStringLiteral("az")).toString());
    QLineEdit *trackVar = line(setting("point3d.track_variable", QStringLiteral("trackList")).toString());
    QLineEdit *pattern = line(setting("point3d.file_pattern", QStringLiteral("detPoint_*.mat")).toString());

    QSpinBox *maxFrames = spin(0, 1000000, setting("point3d.max_frames", 0).toInt());
    maxFrames->setSpecialValueText(QStringLiteral("全部"));
    QSpinBox *minTrackLen = spin(1, 1000, setting("point3d.min_track_length", 4).toInt());
    QDoubleSpinBox *rangeScale = dspin(0.001, 1000.0, 3,
                                       setting("point3d.track_range_scale", 1.0).toDouble(), 0.1);
    QDoubleSpinBox *lineWidth = dspin(0.5, 12.0, 1,
                                      setting("point3d.track_line_width", 2.0).toDouble(), 0.1);
    QSpinBox *gap = spin(1, 20, setting("point3d.continuity_frame_gap", 3).toInt());
    QDoubleSpinBox *rangeC = dspin(0.1, 100000.0, 1,
                                   setting("point3d.continuity_range_scale", 80.0).toDouble(), 1.0);
    QDoubleSpinBox *dopplerC = dspin(0.1, 1000.0, 1,
                                     setting("point3d.continuity_doppler_scale", 10.0).toDouble(), 0.5);
    QDoubleSpinBox *azimuthC = dspin(0.1, 180.0, 1,
                                     setting("point3d.continuity_azimuth_scale", 6.0).toDouble(), 0.5);
    QDoubleSpinBox *threshold = dspin(0.0, 1.0, 2,
                                      setting("point3d.strong_threshold", 0.70).toDouble(), 0.05);
    QCheckBox *autoReload = new QCheckBox(QStringLiteral("启动时恢复上次的点迹窗口与数据"), &dlg);
    autoReload->setChecked(setting("point3d.auto_reload", true).toBool());

    form->addRow(QStringLiteral("DetPoint 变量名"), detVar);
    form->addRow(QStringLiteral("sysPara 变量名"), sysVar);
    form->addRow(QStringLiteral("方位字段"), azField);
    form->addRow(QStringLiteral("航迹变量名（trackList）"), trackVar);
    form->addRow(QStringLiteral("文件匹配模式"), pattern);
    form->addRow(QStringLiteral("最多帧数（0=全部）"), maxFrames);
    form->addRow(QStringLiteral("最小航迹长度（点）"), minTrackLen);
    form->addRow(QStringLiteral("航迹距离缩放"), rangeScale);
    form->addRow(QStringLiteral("航迹线宽"), lineWidth);
    form->addRow(QStringLiteral("连续性：帧间隔"), gap);
    form->addRow(QStringLiteral("连续性：距离尺度"), rangeC);
    form->addRow(QStringLiteral("连续性：速度尺度"), dopplerC);
    form->addRow(QStringLiteral("连续性：方位尺度"), azimuthC);
    form->addRow(QStringLiteral("强连续阈值"), threshold);
    form->addRow(autoReload);

    QLabel *note = new QLabel(QStringLiteral(
        "说明：播放间隔 PauseTime 由工具栏「倍速」统一控制（与主窗口共用全局时间轴）；\n"
        "变量名 / 方位字段 / 文件匹配模式在下次导入时生效，其余参数立即生效；\n"
        "航迹变量名仅用于 MAT 解析（当前航迹来自数据库，按批次叠加）。"), &dlg);
    note->setWordWrap(true);
    form->addRow(note);

    QDialogButtonBox *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(box);
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    setThemedStyle(&dlg, QStringLiteral(
        "QDialog { background: var(--bg-secondary); }"
        "QLabel { color: var(--text-primary); font-family: %1; font-size: 12px; }"
        "QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox { color: var(--text-primary);"
        " background: var(--input-bg); border: 1px solid var(--input-border);"
        " border-radius: 3px; padding: 3px 6px; font-family: %1; font-size: 12px; }"
        "QPushButton { color: var(--text-secondary); background: var(--button-bg);"
        " border: 1px solid var(--border-primary); border-radius: 3px;"
        " padding: 4px 12px; font-family: %1; font-size: 12px; }"
        "QPushButton:hover { background: var(--button-hover); }").arg(ui::uiFamilies()));

    if (dlg.exec() != QDialog::Accepted)
        return;

    app::setValue(QStringLiteral("point3d.det_variable"), detVar->text().trimmed());
    app::setValue(QStringLiteral("point3d.syspara_variable"), sysVar->text().trimmed());
    app::setValue(QStringLiteral("point3d.azimuth_field"), azField->currentText().trimmed());
    app::setValue(QStringLiteral("point3d.track_variable"), trackVar->text().trimmed());
    app::setValue(QStringLiteral("point3d.file_pattern"), pattern->text().trimmed());
    app::setValue(QStringLiteral("point3d.max_frames"), maxFrames->value());
    app::setValue(QStringLiteral("point3d.min_track_length"), minTrackLen->value());
    app::setValue(QStringLiteral("point3d.track_range_scale"), rangeScale->value());
    app::setValue(QStringLiteral("point3d.track_line_width"), lineWidth->value());
    app::setValue(QStringLiteral("point3d.continuity_frame_gap"), gap->value());
    app::setValue(QStringLiteral("point3d.continuity_range_scale"), rangeC->value());
    app::setValue(QStringLiteral("point3d.continuity_doppler_scale"), dopplerC->value());
    app::setValue(QStringLiteral("point3d.continuity_azimuth_scale"), azimuthC->value());
    app::setValue(QStringLiteral("point3d.strong_threshold"), threshold->value());
    app::setValue(QStringLiteral("point3d.auto_reload"), autoReload->isChecked());

    m_canvas->setTrackLineWidth(lineWidth->value());
    if (!m_data.frames.isEmpty()) {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        ContinuityConfig cfg;
        cfg.frameGap = gap->value();
        cfg.rangeScale = rangeC->value();
        cfg.dopplerScale = dopplerC->value();
        cfg.azimuthScale = azimuthC->value();
        computeContinuity(m_data.frames, cfg);
        QApplication::restoreOverrideCursor();
    }
    updateStatus();
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
    double yaw = 0, pitch = 0, zoom = 0;
    m_canvas->camera(&yaw, &pitch, &zoom);
    app::setValue(QStringLiteral("point3d.camera_yaw"), yaw);
    app::setValue(QStringLiteral("point3d.camera_pitch"), pitch);
    app::setValue(QStringLiteral("point3d.camera_zoom"), zoom);
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
