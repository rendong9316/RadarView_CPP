#include "point3dwindow.h"

#include "apppaths.h"
#include "point3dcanvas.h"
#include "point3dimportjob.h"
#include "point3dparsers.h"
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
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
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
        "QWidget#point3dSettings { background: var(--bg-secondary);"
        " border-bottom: 1px solid var(--border-primary); }"
        "QScrollArea#point3dToolbarScroll { background: var(--bg-secondary); border: none; }"
        "QScrollArea#point3dToolbarScroll > QWidget { background: var(--bg-secondary); }"
        "QScrollArea#point3dToolbarScroll > QWidget > QWidget { background: var(--bg-secondary); }"
        "QScrollBar:horizontal { background: var(--bg-tertiary); height: 10px; margin: 0; border: none; }"
        "QScrollBar::handle:horizontal { background: var(--border-primary); min-width: 40px;"
        " border-radius: 5px; }"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0;"
        " border: none; background: none; }"
        "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: none; }"
        "QWidget#point3dStatus { background: var(--bg-secondary);"
        " border-top: 1px solid var(--border-primary); }"
        "QLabel { color: var(--text-secondary); font-family: %1; font-size: 12px; }"
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

void Point3dWindow::setTrackColorProvider(
    const std::function<QColor(TrackSource, const QString &)> &provider)
{
    m_colorProvider = provider;
}

void Point3dWindow::reloadOverlaysFromKeys()
{
    m_data.tracks.clear();
    m_skippedTracks = 0;
    if (!m_trackKeys.isEmpty() && !m_data.frames.isEmpty() && trackdb::isOpen()) {
        const QVector<Track> tracks = trackdb::loadTracks(m_trackKeys);
        for (const Track &t : tracks) {
            TrackOverlay ov;
            const QColor color = m_colorProvider ? m_colorProvider(t.source, t.fileName)
                                                 : QColor();
            if (overlayFromTrack(t, m_data.frames, &ov, color))
                m_data.tracks.append(ov);
            else
                ++m_skippedTracks;       // 与点迹时间窗对不上（一致性校验）
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

    // 用户明确选中的航迹不能被「点源」过滤静默藏起来：当前模式取不到点列就自动切「自动」
    if (!m_data.tracks.isEmpty()) {
        bool admitted = false;
        for (const TrackOverlay &ov : qAsConst(m_data.tracks))
            if (point3dOverlayHasColumn(ov, m_canvas->trackSource())) {
                admitted = true;
                break;
            }
        if (!admitted)
            m_sourceBtn->toggle();      // 当前点列取不到点：切到另一列（平滑 <-> 关联）
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
                            cam.value(QStringLiteral("zoom")).toDouble(1.0),
                            cam.value(QStringLiteral("pan_x")).toDouble(0.0),
                            cam.value(QStringLiteral("pan_y")).toDouble(0.0));

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
    m_toolbar->setAttribute(Qt::WA_StyledBackground, true);
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

    // 拖拽方式：旋转视角 / 平移视图（平移时鼠标变手爪）
    QLabel *dragLabel = new QLabel(QStringLiteral("拖拽"), m_toolbar);
    bar->addWidget(dragLabel);
    m_panBtn = new QPushButton(QStringLiteral("平移 [H]"), m_toolbar);
    m_panBtn->setCheckable(true);              // 弹起 = 旋转视角，按下 = 平移视图
    m_panBtn->setToolTip(QStringLiteral(
        "左键拖拽方式：弹起=旋转视角；按下=平移视图（鼠标变手爪，按住左键拖动图表）\n"
        "空白处双击恢复默认视角"));
    bar->addWidget(m_panBtn);

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
    // 库里每条雷达航迹只保存两套点列（平滑 / 关联），用一个按钮来回切即可，不需要下拉框
    m_sourceBtn = new QPushButton(QStringLiteral("平滑"), m_toolbar);
    m_sourceBtn->setCheckable(true);           // 弹起 = 平滑，按下 = 关联
    m_sourceBtn->setToolTip(QStringLiteral(
        "切换叠加航迹的点列：平滑（smoothPointList）↔ 关联（asscPointList）"));
    bar->addWidget(m_sourceBtn);

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

    m_settingsBtn = new QPushButton(QStringLiteral("显示设置"), m_toolbar);
    m_settingsBtn->setCheckable(true);
    m_settingsBtn->setToolTip(QStringLiteral(
        "展开本窗口的显示设置栏：点迹尺寸 / 航迹线宽 / 标签大小"));
    bar->addWidget(m_settingsBtn);

    QPushButton *advBtn = new QPushButton(QStringLiteral("高级参数…"), m_toolbar);
    advBtn->setToolTip(QStringLiteral("MATLAB 工具的全部可调参数（变量名、连续性、帧数等）"));
    bar->addWidget(advBtn);

    QPushButton *exportBtn = new QPushButton(QStringLiteral("导出 PNG…"), m_toolbar);
    exportBtn->setToolTip(QStringLiteral("把当前画面保存为 PNG（对应 ExportFinalFrame）"));
    bar->addWidget(exportBtn);

    bar->addSpacing(ui::sz(6));
    m_playBtn = new QPushButton(QStringLiteral("播放"), m_toolbar);
    m_playBtn->setToolTip(QStringLiteral("播放 / 暂停本窗口的回放（空格）。只回放本窗口的点迹与航迹"));
    bar->addWidget(m_playBtn);

    // 进度条 = 帧序号（有级）：每一格就是一帧，拖动只能落到某一帧上
    m_seekSlider = new QSlider(Qt::Horizontal, m_toolbar);
    m_seekSlider->setRange(0, 0);
    m_seekSlider->setPageStep(1);
    m_seekSlider->setSingleStep(1);
    m_seekSlider->setMinimumWidth(ui::sz(160));
    m_seekSlider->setToolTip(QStringLiteral("拖动跳转到某一帧（按帧吸附，不插值）"));
    bar->addWidget(m_seekSlider, 1);

    m_timeLabel = new QLabel(QStringLiteral("--"), m_toolbar);
    m_timeLabel->setMinimumWidth(ui::sz(190));
    bar->addWidget(m_timeLabel);

    QLabel *speedLabel = new QLabel(QStringLiteral("倍速"), m_toolbar);
    bar->addWidget(speedLabel);
    m_speedCombo = new QComboBox(m_toolbar);
    m_speedCombo->setEditable(true);
    for (int s : { 1, 2, 5, 10, 20, 50, 100, 300, 500, 2000 })
        m_speedCombo->addItem(QString::number(s));
    m_speedCombo->setToolTip(QStringLiteral(
        "本窗口回放倍速：每帧停留 = 相邻帧时间差 / 倍速（1× 为数据真实节奏）"));
    bar->addWidget(m_speedCombo);

    m_progress = new QProgressBar(m_toolbar);
    m_progress->setRange(0, 100);
    m_progress->setTextVisible(false);
    m_progress->setFixedWidth(ui::sz(140));
    m_progress->setVisible(false);
    bar->addWidget(m_progress);

    // 工具栏控件很多，直接放进窗口会把最小宽度撑到屏幕外、窗口就没法自由缩放了；
    // 放进横向滚动区后窗口可以随便拉窄，放不下的按钮横向滚动即可
    bar->activate();
    QScrollArea *barScroll = new QScrollArea(this);
    barScroll->setObjectName(QStringLiteral("point3dToolbarScroll"));
    barScroll->setWidgetResizable(true);
    barScroll->setFrameShape(QFrame::NoFrame);
    barScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    barScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    barScroll->setWidget(m_toolbar);
    barScroll->setFixedHeight(m_toolbar->sizeHint().height() + ui::sz(12));
    root->addWidget(barScroll);

    // ---- 显示设置栏（可折叠，只影响本窗口）----
    m_settingsBar = new QWidget(this);
    m_settingsBar->setObjectName(QStringLiteral("point3dSettings"));
    m_settingsBar->setAttribute(Qt::WA_StyledBackground, true);
    QHBoxLayout *sbar = new QHBoxLayout(m_settingsBar);
    sbar->setContentsMargins(ui::sz(10), ui::sz(4), ui::sz(10), ui::sz(4));
    sbar->setSpacing(ui::sz(8));
    auto dspin = [this](double lo, double hi, double value) {
        QDoubleSpinBox *s = new QDoubleSpinBox(m_settingsBar);
        s->setRange(lo, hi);
        s->setDecimals(2);
        s->setSingleStep(0.1);
        s->setValue(value);
        s->setMinimumWidth(ui::sz(72));
        return s;
    };
    sbar->addWidget(new QLabel(QStringLiteral("点迹尺寸"), m_settingsBar));
    m_pointSizeSpin = dspin(0.2, 5.0, 1.0);
    m_pointSizeSpin->setToolTip(QStringLiteral("点迹点的大小倍数（0.2–5）"));
    sbar->addWidget(m_pointSizeSpin);
    sbar->addWidget(new QLabel(QStringLiteral("航迹线宽"), m_settingsBar));
    m_lineWidthSpin = dspin(0.5, 12.0, 2.0);
    m_lineWidthSpin->setToolTip(QStringLiteral("叠加航迹的线宽（0.5–12）"));
    sbar->addWidget(m_lineWidthSpin);
    sbar->addWidget(new QLabel(QStringLiteral("标签大小"), m_settingsBar));
    m_labelSizeSpin = dspin(0.5, 3.0, 1.0);
    m_labelSizeSpin->setToolTip(QStringLiteral("航迹编号（T20xxx）文字大小倍数（0.5–3）"));
    sbar->addWidget(m_labelSizeSpin);
    sbar->addStretch(1);
    m_settingsBar->setVisible(false);
    root->addWidget(m_settingsBar);

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
    connect(m_sourceBtn, &QPushButton::toggled, this, [this](bool associated) {
        m_sourceBtn->setText(associated ? QStringLiteral("关联") : QStringLiteral("平滑"));
        m_canvas->setTrackSource(associated ? Point3dTrackSource::Associated
                                            : Point3dTrackSource::Smooth);
        if (!m_restoring)
            app::setValue(QStringLiteral("point3d.track_source"),
                          associated ? QStringLiteral("associated") : QStringLiteral("smooth"));
        updateStatus();
    });
    connect(m_panBtn, &QPushButton::toggled, this, [this](bool pan) {
        m_canvas->setDragMode(pan ? Point3dCanvas::DragMode::Pan
                                  : Point3dCanvas::DragMode::Rotate);
        if (!m_restoring)
            app::setValue(QStringLiteral("point3d.drag_mode"),
                          pan ? QStringLiteral("pan") : QStringLiteral("rotate"));
    });
    // 显示设置栏：点迹尺寸 / 航迹线宽 / 标签大小
    connect(m_settingsBtn, &QPushButton::toggled, this, [this](bool open) {
        m_settingsBar->setVisible(open);
        if (!m_restoring)
            app::setValue(QStringLiteral("point3d.settings_open"), open);
    });
    connect(m_pointSizeSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double v) {
                m_canvas->setPointScale(v);
                if (!m_restoring)
                    app::setValue(QStringLiteral("point3d.point_scale"), v);
            });
    connect(m_lineWidthSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double v) {
                m_canvas->setTrackLineWidth(v);
                if (!m_restoring)
                    app::setValue(QStringLiteral("point3d.track_line_width"), v);
            });
    connect(m_labelSizeSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double v) {
                m_canvas->setLabelScale(v);
                if (!m_restoring)
                    app::setValue(QStringLiteral("point3d.label_scale"), v);
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

    // 快捷键：P / T / L / H / 空格（窗口内生效）
    auto bindKey = [this](Qt::Key key, const std::function<void()> &fn) {
        QShortcut *sc = new QShortcut(QKeySequence(key), this);
        sc->setContext(Qt::WidgetWithChildrenShortcut);
        connect(sc, &QShortcut::activated, this, fn);
    };
    bindKey(Qt::Key_P, [this]() { m_pointBtn->toggle(); });
    bindKey(Qt::Key_T, [this]() { m_trackBtn->toggle(); });
    bindKey(Qt::Key_L, [this]() { m_labelBtn->toggle(); });
    bindKey(Qt::Key_H, [this]() { m_panBtn->toggle(); });   // 手爪（平移）/ 旋转
    bindKey(Qt::Key_Space, [this]() { togglePlay(); });

    // 没有点迹数据前先禁用播放控制（导入完成后 syncPlaybackUi 打开）
    m_playBtn->setEnabled(false);
    m_seekSlider->setEnabled(false);
    m_speedCombo->setEnabled(true);

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
    // 点源（平滑/关联）与拖拽方式（旋转/平移）：按钮状态即设置值，按下时自动写回
    m_sourceBtn->setChecked(setting("point3d.track_source", QStringLiteral("smooth")).toString()
                            == QLatin1String("associated"));
    m_panBtn->setChecked(setting("point3d.drag_mode", QStringLiteral("rotate")).toString()
                         == QLatin1String("pan"));
    m_pointSizeSpin->setValue(setting("point3d.point_scale", 1.0).toDouble());
    m_lineWidthSpin->setValue(setting("point3d.track_line_width", 2.0).toDouble());
    m_labelSizeSpin->setValue(setting("point3d.label_scale", 1.0).toDouble());
    m_settingsBtn->setChecked(setting("point3d.settings_open", false).toBool());

    // 本窗口回放的倍速（只影响本窗口）
    m_speed = qBound(0.01, setting("point3d.play_speed", 100.0).toDouble(), 1.0e6);
    m_speedCombo->setEditText(QString::number(m_speed, 'g', 6));

    const double camYaw = setting("point3d.camera_yaw", 42.0).toDouble();
    const double camPitch = setting("point3d.camera_pitch", 23.0).toDouble();
    const double camZoom = setting("point3d.camera_zoom", 1.0).toDouble();
    const double camPanX = setting("point3d.camera_pan_x", 0.0).toDouble();
    const double camPanY = setting("point3d.camera_pan_y", 0.0).toDouble();
    m_canvas->setCamera(camYaw, camPitch, camZoom, camPanX, camPanY);

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

    // 航迹选择属于单个窗口：只随窗口会话（point3d.windows）保存，不写全局默认，
    // 否则新开的窗口会莫名继承别的窗口选过的航迹。
    m_restoring = false;
    syncPlaybackUi();
    updatePlaybackUi();
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
    setPlaying(false);
    m_canvas->setDataset(&m_data);
    m_canvas->setHighlightedBatch(-1);
    m_files = m_pendingFiles;
    // 重新导入点迹后，已选数据库航迹需要按新帧重新映射 Z 值
    reloadOverlaysFromKeys();
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
    const QSet<int> isolated = m_canvas->isolatedBatches();
    if (!isolated.isEmpty())
        text += QStringLiteral(" | 按地图选择单独显示 %1 条").arg(isolated.size());
    if (m_skippedTracks > 0)
        text += QStringLiteral(" | %1 条航迹与点迹时间不匹配已忽略").arg(m_skippedTracks);
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
