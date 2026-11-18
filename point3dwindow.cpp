#include "point3dwindow.h"

#include "apppaths.h"
#include "point3dcanvas.h"
#include "point3dimportjob.h"
#include "point3dparsers.h"
#include "theme.h"
#include "trackdb.h"
#include "trackpicker.h"

#include <QAction>
#include <QActionGroup>
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
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QShortcut>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <functional>
#include <limits>

namespace {

// 侧栏滚动区：最小尺寸固定得很小，不跟着内容走。
// 否则 QScrollArea 会把内容的 minimumSizeHint 顶到窗口最小宽度上，窗口又拉不窄了。
class SideScrollArea : public QScrollArea
{
public:
    explicit SideScrollArea(QWidget *parent) : QScrollArea(parent) {}
    QSize minimumSizeHint() const override { return QSize(160, 160); }
};

QVariant setting(const char *key, const QVariant &def)
{
    return app::settings().value(QString::fromLatin1(key), def);
}

// 视图 -> 设置键后缀（三套区域筛选参数各存一份）
QString regionKey(int mode)
{
    return mode == int(Point3dViewMode::RAF) ? QStringLiteral("raf")
         : mode == int(Point3dViewMode::VAF) ? QStringLiteral("vaf")
                                             : QStringLiteral("rvf");
}

// 区域筛选三行的轴名（随视图模式变化）
void regionAxisTitles(Point3dViewMode mode, QString *x, QString *y, QString *z)
{
    switch (mode) {
    case Point3dViewMode::RVF:
        *x = QStringLiteral("X 距离 / m");
        *y = QStringLiteral("Y 速度 / (m/s)");
        break;
    case Point3dViewMode::RAF:
        *x = QStringLiteral("X 距离 / m");
        *y = QStringLiteral("Y 方位 / (°)");
        break;
    case Point3dViewMode::VAF:
        *x = QStringLiteral("X 速度 / (m/s)");
        *y = QStringLiteral("Y 方位 / (°)");
        break;
    }
    *z = QStringLiteral("Z 帧号");
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

// 色标（SNR→颜色）映射范围：默认自动取本窗口数据的 min/max，也可在高级参数里手动指定
// 把 SNR 设置同步到侧栏控件（高级参数里改过之后调用）
void Point3dWindow::syncSnrControls()
{
    m_snrAuto->setChecked(setting("point3d.snr_auto", true).toBool());
    m_snrLoSpin->setValue(setting("point3d.snr_min", 0.0).toDouble());
    m_snrHiSpin->setValue(setting("point3d.snr_max", 30.0).toDouble());
}

void Point3dWindow::applySnrRange()
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    if (setting("point3d.snr_auto", true).toBool()) {
        m_canvas->setSnrRange(nan, nan);
        return;
    }
    double lo = setting("point3d.snr_min", 0.0).toDouble();
    double hi = setting("point3d.snr_max", 30.0).toDouble();
    if (!(hi > lo))
        hi = lo + 1.0;
    m_canvas->setSnrRange(lo, hi);
}

// ---------------------------------------------------------------------------
//  区域筛选：三行 = 当前视图的三个轴；每个视图各存一套，切换视图原样恢复
// ---------------------------------------------------------------------------
Point3dRegion Point3dWindow::regionForView(Point3dViewMode mode) const
{
    return m_viewRegions[int(mode)].region;
}

bool Point3dWindow::regionAppliedForView(Point3dViewMode mode) const
{
    return m_viewRegions[int(mode)].applied;
}

void Point3dWindow::setViewMode(Point3dViewMode mode)
{
    const int idx = m_viewCombo->findData(int(mode));
    if (idx >= 0)
        m_viewCombo->setCurrentIndex(idx);
}

void Point3dWindow::loadRegionForCurrentView()
{
    const int vi = int(m_canvas->viewMode());
    ViewRegion &st = m_viewRegions[vi];
    if (!st.region.valid)
        st.region = point3dRegionExtent(m_data, Point3dViewMode(vi));

    QString xl, yl, zl;
    regionAxisTitles(Point3dViewMode(vi), &xl, &yl, &zl);

    m_regionLoading = true;
    m_regionAxisLabel[0]->setText(xl);
    m_regionAxisLabel[1]->setText(yl);
    m_regionAxisLabel[2]->setText(zl);
    m_regionMinSpin[0]->setValue(st.region.x0);
    m_regionMaxSpin[0]->setValue(st.region.x1);
    m_regionMinSpin[1]->setValue(st.region.y0);
    m_regionMaxSpin[1]->setValue(st.region.y1);
    m_regionMinSpin[2]->setValue(st.region.z0);
    m_regionMaxSpin[2]->setValue(st.region.z1);
    m_regionLoading = false;

    m_canvas->setRegionFilter(st.applied ? st.region : Point3dRegion());
    m_canvas->setRegionBox(regionBoxVisible() ? st.region : Point3dRegion());
    updateRegionButtons();
    updateStatus();
}

void Point3dWindow::onRegionSpinEdited()
{
    if (m_restoring || m_regionLoading)
        return;
    const int vi = int(m_canvas->viewMode());
    ViewRegion &st = m_viewRegions[vi];
    st.region.valid = true;
    st.region.x0 = m_regionMinSpin[0]->value();
    st.region.x1 = m_regionMaxSpin[0]->value();
    st.region.y0 = m_regionMinSpin[1]->value();
    st.region.y1 = m_regionMaxSpin[1]->value();
    st.region.z0 = m_regionMinSpin[2]->value();
    st.region.z1 = m_regionMaxSpin[2]->value();

    // 已应用后再改数字：自动退回全量视图，只显示候选框（点「应用」才重新放大+裁剪）
    if (st.applied) {
        st.applied = false;
        m_canvas->setRegionFilter(Point3dRegion());
        app::setValue(QStringLiteral("point3d.region_applied.%1").arg(regionKey(vi)), false);
    }
    m_canvas->setRegionBox(regionBoxVisible() ? st.region : Point3dRegion());
    persistRegion(vi);
    updateRegionButtons();
    updateStatus();
}

void Point3dWindow::applyRegion()
{
    if (m_data.frames.isEmpty())
        return;
    const int vi = int(m_canvas->viewMode());
    ViewRegion &st = m_viewRegions[vi];
    Point3dRegion r = st.region;
    r.valid = true;
    const Point3dRegion ext = point3dRegionExtent(m_data, Point3dViewMode(vi));
    if (ext.valid) {                        // 夹到数据量程内
        r.x0 = qMax(r.x0, ext.x0);
        r.x1 = qMin(r.x1, ext.x1);
        r.y0 = qMax(r.y0, ext.y0);
        r.y1 = qMin(r.y1, ext.y1);
        r.z0 = qMax(r.z0, ext.z0);
        r.z1 = qMin(r.z1, ext.z1);
    }
    if (!(r.x1 > r.x0) || !(r.y1 > r.y0) || !(r.z1 > r.z0)) {
        m_status->setText(QStringLiteral("区域筛选：区间无效（每行下限必须小于上限）"));
        return;
    }
    st.region = r;
    st.applied = true;

    m_regionLoading = true;
    m_regionMinSpin[0]->setValue(r.x0);
    m_regionMaxSpin[0]->setValue(r.x1);
    m_regionMinSpin[1]->setValue(r.y0);
    m_regionMaxSpin[1]->setValue(r.y1);
    m_regionMinSpin[2]->setValue(r.z0);
    m_regionMaxSpin[2]->setValue(r.z1);
    m_regionLoading = false;

    m_canvas->setRegionBox(r);
    m_canvas->setRegionFilter(r);           // 归一化改为区间、数据裁剪 -> 视图放大到框内
    persistRegion(vi);
    updateRegionButtons();
    m_canvas->repaint();
    updateStatus();
    if (m_canvas->visiblePointCount() == 0)
        m_status->setText(QStringLiteral("区域筛选：框内没有点迹，请放宽区间"));
}

void Point3dWindow::resetRegion()
{
    const int vi = int(m_canvas->viewMode());
    ViewRegion &st = m_viewRegions[vi];
    st.region = point3dRegionExtent(m_data, Point3dViewMode(vi));
    st.applied = false;

    m_regionLoading = true;
    m_regionMinSpin[0]->setValue(st.region.x0);
    m_regionMaxSpin[0]->setValue(st.region.x1);
    m_regionMinSpin[1]->setValue(st.region.y0);
    m_regionMaxSpin[1]->setValue(st.region.y1);
    m_regionMinSpin[2]->setValue(st.region.z0);
    m_regionMaxSpin[2]->setValue(st.region.z1);
    m_regionLoading = false;

    m_canvas->setRegionFilter(Point3dRegion());
    m_canvas->setRegionBox(regionBoxVisible() ? st.region : Point3dRegion());
    persistRegion(vi);
    updateRegionButtons();
    updateStatus();
}

void Point3dWindow::persistRegion(int mode)
{
    const ViewRegion &st = m_viewRegions[mode];
    const QString key = QStringLiteral("point3d.region.%1").arg(regionKey(mode));
    if (st.region.valid) {
        app::setValue(key, QStringLiteral("%1,%2,%3,%4,%5,%6")
                               .arg(st.region.x0, 0, 'g', 12)
                               .arg(st.region.x1, 0, 'g', 12)
                               .arg(st.region.y0, 0, 'g', 12)
                               .arg(st.region.y1, 0, 'g', 12)
                               .arg(st.region.z0, 0, 'g', 12)
                               .arg(st.region.z1, 0, 'g', 12));
    } else {
        app::removeValue(key);
    }
    app::setValue(QStringLiteral("point3d.region_applied.%1").arg(regionKey(mode)), st.applied);
}

void Point3dWindow::revalidateRegions()
{
    for (int i = 0; i < 3; ++i) {
        ViewRegion &st = m_viewRegions[i];
        const Point3dRegion ext = point3dRegionExtent(m_data, Point3dViewMode(i));
        if (!ext.valid)
            continue;
        if (!st.region.valid) {
            st.region = ext;
            st.applied = false;
            continue;
        }
        st.region.x0 = qBound(ext.x0, st.region.x0, ext.x1);
        st.region.x1 = qBound(ext.x0, st.region.x1, ext.x1);
        st.region.y0 = qBound(ext.y0, st.region.y0, ext.y1);
        st.region.y1 = qBound(ext.y0, st.region.y1, ext.y1);
        st.region.z0 = qBound(ext.z0, st.region.z0, ext.z1);
        st.region.z1 = qBound(ext.z0, st.region.z1, ext.z1);
        if (!(st.region.x1 > st.region.x0) || !(st.region.y1 > st.region.y0)
                || !(st.region.z1 > st.region.z0)) {
            st.region = ext;                 // 新数据完全不在原区间里：复位
            st.applied = false;
        }
        persistRegion(i);
    }
}

// 候选框只在"用户真的改过区间/已经应用"时显示；满量程时立方体本身就是边界，不用再描框
bool Point3dWindow::regionBoxVisible() const
{
    if (m_sidePanel && m_sidePanel->isHidden())
        return false;
    if (m_sideStack && m_sideStack->currentIndex() != 0)
        return false;      // 高级页时看不到区域控件，不必画候选框
    const int vi = int(m_canvas->viewMode());
    const ViewRegion &st = m_viewRegions[vi];
    if (st.applied)
        return true;
    if (!st.region.valid)
        return false;
    const Point3dRegion ext = point3dRegionExtent(m_data, Point3dViewMode(vi));
    if (!ext.valid)
        return false;
    const double eps = 1e-9;
    return st.region.x0 > ext.x0 + eps || st.region.x1 < ext.x1 - eps
        || st.region.y0 > ext.y0 + eps || st.region.y1 < ext.y1 - eps
        || st.region.z0 > ext.z0 + eps || st.region.z1 < ext.z1 - eps;
}

void Point3dWindow::updateRegionButtons()
{
    const bool haveData = !m_data.frames.isEmpty();
    m_regionApplyBtn->setEnabled(haveData);
    m_regionResetBtn->setEnabled(haveData && m_viewRegions[int(m_canvas->viewMode())].applied);
}

bool Point3dWindow::setRegionForCurrentView(const Point3dRegion &r, bool apply)
{
    const int vi = int(m_canvas->viewMode());
    ViewRegion &st = m_viewRegions[vi];
    st.region = r;
    st.region.valid = true;
    m_regionLoading = true;
    m_regionMinSpin[0]->setValue(r.x0);
    m_regionMaxSpin[0]->setValue(r.x1);
    m_regionMinSpin[1]->setValue(r.y0);
    m_regionMaxSpin[1]->setValue(r.y1);
    m_regionMinSpin[2]->setValue(r.z0);
    m_regionMaxSpin[2]->setValue(r.z1);
    m_regionLoading = false;
    if (!apply) {
        st.applied = false;
        m_canvas->setRegionFilter(Point3dRegion());
        m_canvas->setRegionBox(r);
        persistRegion(vi);
        updateRegionButtons();
        updateStatus();
        return true;
    }
    applyRegion();
    return st.applied;
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
    QJsonArray regions;
    for (int i = 0; i < 3; ++i) {
        const ViewRegion &st = m_viewRegions[i];
        QJsonObject ro;
        ro.insert(QStringLiteral("valid"), st.region.valid);
        ro.insert(QStringLiteral("applied"), st.applied);
        ro.insert(QStringLiteral("x0"), st.region.x0);
        ro.insert(QStringLiteral("x1"), st.region.x1);
        ro.insert(QStringLiteral("y0"), st.region.y0);
        ro.insert(QStringLiteral("y1"), st.region.y1);
        ro.insert(QStringLiteral("z0"), st.region.z0);
        ro.insert(QStringLiteral("z1"), st.region.z1);
        regions.append(ro);
    }
    o.insert(QStringLiteral("regions"), regions);
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

    // 区域筛选（三个视图各一套）：先恢复状态，再切视图，切完统一套用到画布
    const QJsonArray regions = obj.value(QStringLiteral("regions")).toArray();
    for (int i = 0; i < 3 && i < regions.size(); ++i) {
        const QJsonObject ro = regions.at(i).toObject();
        m_viewRegions[i].region.valid = ro.value(QStringLiteral("valid")).toBool();
        m_viewRegions[i].applied = ro.value(QStringLiteral("applied")).toBool()
                                   && m_viewRegions[i].region.valid;
        m_viewRegions[i].region.x0 = ro.value(QStringLiteral("x0")).toDouble();
        m_viewRegions[i].region.x1 = ro.value(QStringLiteral("x1")).toDouble();
        m_viewRegions[i].region.y0 = ro.value(QStringLiteral("y0")).toDouble();
        m_viewRegions[i].region.y1 = ro.value(QStringLiteral("y1")).toDouble();
        m_viewRegions[i].region.z0 = ro.value(QStringLiteral("z0")).toDouble();
        m_viewRegions[i].region.z1 = ro.value(QStringLiteral("z1")).toDouble();
    }

    const int mode = obj.value(QStringLiteral("view_mode")).toInt(
        int(Point3dViewMode::RVF));
    const int viewIdx = m_viewCombo->findData(mode);
    if (viewIdx >= 0)
        m_viewCombo->setCurrentIndex(viewIdx);
    m_canvas->setViewMode(Point3dViewMode(m_viewCombo->currentData().toInt()));
    loadRegionForCurrentView();

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
    m_sideStack->addWidget(buildSidePanel());
    m_sideStack->addWidget(buildAdvancedPanel());
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
    // 高级页：即时持久化；连续性参数防抖 ~0.4s 后重算；航迹长度/距离缩放立即重建叠加
    m_advTimer = new QTimer(this);
    m_advTimer->setSingleShot(true);
    m_advTimer->setInterval(400);
    connect(m_advTimer, &QTimer::timeout, this, &Point3dWindow::recomputeContinuityNow);
    auto advPersist = [this]() { persistAdvanced(); };
    connect(m_detVarEdit, &QLineEdit::textChanged, this, advPersist);
    connect(m_sysVarEdit, &QLineEdit::textChanged, this, advPersist);
    connect(m_trackVarEdit, &QLineEdit::textChanged, this, advPersist);
    connect(m_patternEdit, &QLineEdit::textChanged, this, advPersist);
    connect(m_azFieldCombo, &QComboBox::currentTextChanged, this, advPersist);
    connect(m_maxFramesSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, advPersist);
    connect(m_autoReloadCheck, &QCheckBox::toggled, this, advPersist);
    auto advContinuity = [this]() {
        persistAdvanced();
        if (!m_restoring)
            m_advTimer->start();
    };
    connect(m_gapSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, advContinuity);
    connect(m_rangeCSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, advContinuity);
    connect(m_dopplerCSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, advContinuity);
    connect(m_azimuthCSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, advContinuity);
    // 最小航迹长度 / 距离缩放立即重建叠加航迹；强连续阈值只影响状态统计
    connect(m_minTrackLenSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) {
        persistAdvanced();
        reloadOverlaysFromKeys();
        updateStatus();
    });
    connect(m_rangeScaleSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) {
        persistAdvanced();
        reloadOverlaysFromKeys();
        updateStatus();
    });
    connect(m_thresholdSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) {
        persistAdvanced();
        m_canvas->setStrongThreshold(v);
        updateStatus();
    });
    connect(m_sourceBtn, &QPushButton::toggled, this, [this](bool associated) {
        m_sourceBtn->setText(associated ? QStringLiteral("关联") : QStringLiteral("平滑"));
        m_canvas->setTrackSource(associated ? Point3dTrackSource::Associated
                                            : Point3dTrackSource::Smooth);
        if (!m_restoring)
            app::setValue(QStringLiteral("point3d.track_source"),
                          associated ? QStringLiteral("associated") : QStringLiteral("smooth"));
        updateStatus();
    });
    // 尺寸 / 色标 / 区域筛选
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
    connect(m_hoverSizeSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double v) {
                m_canvas->setHoverTextScale(v);
                if (!m_restoring)
                    app::setValue(QStringLiteral("point3d.hover_text_scale"), v);
            });
    connect(m_snrAuto, &QCheckBox::toggled, this, [this](bool on) {
        m_snrLoSpin->setEnabled(!on);
        m_snrHiSpin->setEnabled(!on);
        if (m_restoring)
            return;
        app::setValue(QStringLiteral("point3d.snr_auto"), on);
        applySnrRange();
    });
    auto snrEdited = [this](double) {
        if (m_restoring)
            return;
        app::setValue(QStringLiteral("point3d.snr_min"), m_snrLoSpin->value());
        app::setValue(QStringLiteral("point3d.snr_max"), m_snrHiSpin->value());
        applySnrRange();
    };
    connect(m_snrLoSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, snrEdited);
    connect(m_snrHiSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, snrEdited);
    for (int i = 0; i < 3; ++i) {
        connect(m_regionMinSpin[i], QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, &Point3dWindow::onRegionSpinEdited);
        connect(m_regionMaxSpin[i], QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, &Point3dWindow::onRegionSpinEdited);
    }
    connect(m_regionApplyBtn, &QPushButton::clicked, this, &Point3dWindow::applyRegion);
    connect(m_regionResetBtn, &QPushButton::clicked, this, &Point3dWindow::resetRegion);
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
    // 点源（平滑/关联）：按钮状态即设置值，按下时自动写回
    m_sourceBtn->setChecked(setting("point3d.track_source", QStringLiteral("smooth")).toString()
                            == QLatin1String("associated"));
    m_pointSizeSpin->setValue(setting("point3d.point_scale", 1.0).toDouble());
    m_lineWidthSpin->setValue(setting("point3d.track_line_width", 2.0).toDouble());
    m_labelSizeSpin->setValue(setting("point3d.label_scale", 1.0).toDouble());
    m_hoverSizeSpin->setValue(setting("point3d.hover_text_scale", 1.0).toDouble());
    // 左侧侧栏：恢复可见性 + 当前页（0=设置，1=高级）
    setSidePanelPage(setting("point3d.side_page", 0).toInt(),
                     setting("point3d.side_panel", true).toBool());
    // 色标 SNR 映射控件
    m_snrAuto->setChecked(setting("point3d.snr_auto", true).toBool());
    m_snrLoSpin->setValue(setting("point3d.snr_min", 0.0).toDouble());
    m_snrHiSpin->setValue(setting("point3d.snr_max", 30.0).toDouble());

    // 色标 SNR 映射（侧栏可改；默认按数据自动）+ 强连续阈值（只用于状态统计）
    applySnrRange();
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

    // 区域筛选：恢复三个视图各自的区间与是否已应用
    for (int i = 0; i < 3; ++i) {
        const QString key = QStringLiteral("point3d.region.%1").arg(regionKey(i));
        const QString joined = app::settings().value(key, QString()).toString();
        if (!joined.isEmpty()) {
            const QStringList parts = joined.split(QLatin1Char(','));
            if (parts.size() == 6) {
                m_viewRegions[i].region.valid = true;
                m_viewRegions[i].region.x0 = parts.at(0).toDouble();
                m_viewRegions[i].region.x1 = parts.at(1).toDouble();
                m_viewRegions[i].region.y0 = parts.at(2).toDouble();
                m_viewRegions[i].region.y1 = parts.at(3).toDouble();
                m_viewRegions[i].region.z0 = parts.at(4).toDouble();
                m_viewRegions[i].region.z1 = parts.at(5).toDouble();
            }
        }
        m_viewRegions[i].applied =
            app::settings().value(QStringLiteral("point3d.region_applied.%1").arg(regionKey(i)),
                                  false).toBool()
            && m_viewRegions[i].region.valid;
    }
    loadRegionForCurrentView();

    // 航迹选择属于单个窗口：只随窗口会话（point3d.windows）保存，不写全局默认，
    // 否则新开的窗口会莫名继承别的窗口选过的航迹。
    m_restoring = false;
    syncPlaybackUi();
    updatePlaybackUi();
    updateStatus();
}

// 左侧「显示」侧栏：对齐地图窗口的侧栏做法，参数分区、可滚动，窄窗口也能用
QWidget *Point3dWindow::buildSidePanel()
{
    QScrollArea *scroll = new SideScrollArea(this);
    scroll->setObjectName(QStringLiteral("point3dSide"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    // 允许横向滚动：否则侧栏内容的最小宽度会顶到窗口最小宽度上，窗口又拉不窄了
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setFixedWidth(ui::sz(300));

    QWidget *page = new QWidget(scroll);
    page->setObjectName(QStringLiteral("point3dSidePage"));
    QVBoxLayout *lay = new QVBoxLayout(page);
    lay->setContentsMargins(ui::sz(8), ui::sz(8), ui::sz(8), ui::sz(8));
    lay->setSpacing(ui::sz(8));

    auto mkSpin = [](QWidget *parent, double lo, double hi, double value,
                     double step, int decimals) {
        QDoubleSpinBox *s = new QDoubleSpinBox(parent);
        s->setRange(lo, hi);
        s->setDecimals(decimals);
        s->setSingleStep(step);
        s->setValue(value);
        return s;
    };
    auto addForm = [&](const QString &title) {
        QGroupBox *g = new QGroupBox(title, page);
        QFormLayout *f = new QFormLayout(g);
        f->setContentsMargins(ui::sz(8), ui::sz(10), ui::sz(8), ui::sz(8));
        f->setSpacing(ui::sz(6));
        f->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        lay->addWidget(g);
        return f;
    };

    // ---- 视图 ----
    {
        QFormLayout *f = addForm(QStringLiteral("视图"));
        m_viewCombo = new QComboBox(page);
        m_viewCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        m_viewCombo->setMinimumContentsLength(6);
        m_viewCombo->addItem(QStringLiteral("距离–速度–帧号"), int(Point3dViewMode::RVF));
        m_viewCombo->addItem(QStringLiteral("距离–方位–帧号"), int(Point3dViewMode::RAF));
        m_viewCombo->addItem(QStringLiteral("速度–方位–帧号"), int(Point3dViewMode::VAF));
        m_viewCombo->setToolTip(QStringLiteral("三维视图的 X/Y 轴组合，Z 轴恒为帧号"));
        f->addRow(QStringLiteral("三轴视图"), m_viewCombo);

        m_windowCombo = new QComboBox(page);
        m_windowCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        m_windowCombo->setMinimumContentsLength(4);
        const int presets[] = { 20, 40, 70, 150 };
        for (int v : presets)
            m_windowCombo->addItem(QStringLiteral("%1 帧").arg(v), v);
        m_windowCombo->addItem(QStringLiteral("全部"), 0);
        m_windowCombo->addItem(QStringLiteral("自定义…"), -1);
        m_windowCombo->setToolTip(QStringLiteral("同时显示最近多少帧（「全部」= 整个数据集）"));
        m_windowSpin = new QSpinBox(page);
        m_windowSpin->setRange(1, 100000);
        m_windowSpin->setValue(70);
        m_windowSpin->setVisible(false);
        m_windowSpin->setToolTip(QStringLiteral("自定义滑动窗口帧数"));
        QVBoxLayout *winCol = new QVBoxLayout;
        winCol->setContentsMargins(0, 0, 0, 0);
        winCol->setSpacing(ui::sz(4));
        winCol->addWidget(m_windowCombo);
        winCol->addWidget(m_windowSpin);
        f->addRow(QStringLiteral("滑动窗口"), winCol);
    }

    // ---- 显示 ----
    {
        QFormLayout *f = addForm(QStringLiteral("显示"));
        m_sourceBtn = new QPushButton(QStringLiteral("平滑"), page);
        m_sourceBtn->setCheckable(true);
        m_sourceBtn->setToolTip(QStringLiteral(
            "切换叠加航迹的点列：平滑（smoothPointList）↔ 关联（asscPointList）"));
        f->addRow(QStringLiteral("航迹点源"), m_sourceBtn);

        QHBoxLayout *row = new QHBoxLayout;
        row->setSpacing(ui::sz(4));
        m_pointBtn = new QPushButton(QStringLiteral("点迹 [P]"), page);
        m_pointBtn->setCheckable(true);
        m_pointBtn->setChecked(true);
        m_pointBtn->setToolTip(QStringLiteral("显示 / 隐藏点迹（P）"));
        m_trackBtn = new QPushButton(QStringLiteral("航迹 [T]"), page);
        m_trackBtn->setCheckable(true);
        m_trackBtn->setChecked(true);
        m_trackBtn->setToolTip(QStringLiteral("显示 / 隐藏航迹（T）"));
        m_labelBtn = new QPushButton(QStringLiteral("标签 [L]"), page);
        m_labelBtn->setCheckable(true);
        m_labelBtn->setChecked(true);
        m_labelBtn->setToolTip(QStringLiteral("显示 / 隐藏航迹编号（L）"));
        row->addWidget(m_pointBtn);
        row->addWidget(m_trackBtn);
        row->addWidget(m_labelBtn);
        f->addRow(QStringLiteral("图层"), row);
    }

    // ---- 尺寸 ----
    {
        QFormLayout *f = addForm(QStringLiteral("尺寸"));
        m_pointSizeSpin = mkSpin(page, 0.2, 5.0, 1.0, 0.1, 2);
        m_pointSizeSpin->setToolTip(QStringLiteral("点迹点的大小倍数（0.2–5）"));
        m_lineWidthSpin = mkSpin(page, 0.5, 12.0, 2.0, 0.1, 2);
        m_lineWidthSpin->setToolTip(QStringLiteral("叠加航迹的线宽（0.5–12）"));
        m_labelSizeSpin = mkSpin(page, 0.5, 3.0, 1.0, 0.1, 2);
        m_labelSizeSpin->setToolTip(QStringLiteral("航迹编号与坐标轴刻度数字的大小倍数（0.5–3）"));
        m_hoverSizeSpin = mkSpin(page, 0.5, 4.0, 1.0, 0.1, 2);
        m_hoverSizeSpin->setToolTip(QStringLiteral("鼠标悬停/点击信息浮窗的字号倍数（0.5–4）"));
        f->addRow(QStringLiteral("点迹尺寸"), m_pointSizeSpin);
        f->addRow(QStringLiteral("航迹线宽"), m_lineWidthSpin);
        f->addRow(QStringLiteral("标签大小"), m_labelSizeSpin);
        f->addRow(QStringLiteral("提示字号"), m_hoverSizeSpin);
    }

    // ---- 色标 SNR ----
    {
        QFormLayout *f = addForm(QStringLiteral("色标 SNR"));
        m_snrAuto = new QCheckBox(QStringLiteral("自动（按数据 min/max）"), page);
        m_snrLoSpin = mkSpin(page, -100.0, 300.0, 0.0, 0.5, 2);
        m_snrHiSpin = mkSpin(page, -100.0, 300.0, 30.0, 0.5, 2);
        f->addRow(m_snrAuto);
        f->addRow(QStringLiteral("下限 dB"), m_snrLoSpin);
        f->addRow(QStringLiteral("上限 dB"), m_snrHiSpin);
    }

    // ---- 区域筛选 ----
    {
        QGroupBox *g = new QGroupBox(QStringLiteral("区域筛选"), page);
        QVBoxLayout *gl = new QVBoxLayout(g);
        gl->setContentsMargins(ui::sz(8), ui::sz(10), ui::sz(8), ui::sz(8));
        gl->setSpacing(ui::sz(5));
        for (int i = 0; i < 3; ++i) {
            // 轴名一行、区间控件一行：侧栏不宽，挤一行会把滚动区撑出横向滚动条
            m_regionAxisLabel[i] = new QLabel(g);
            gl->addWidget(m_regionAxisLabel[i]);
            QHBoxLayout *row = new QHBoxLayout;
            row->setSpacing(ui::sz(4));
            m_regionMinSpin[i] = mkSpin(g, -1.0e7, 1.0e7, 0.0, 1.0, 2);
            m_regionMaxSpin[i] = mkSpin(g, -1.0e7, 1.0e7, 1.0, 1.0, 2);
            QPushButton *fullBtn = new QPushButton(QStringLiteral("满"), g);
            fullBtn->setToolTip(QStringLiteral("该轴设为满量程"));
            fullBtn->setFixedWidth(ui::sz(28));
            connect(fullBtn, &QPushButton::clicked, this, [this, i]() {
                const Point3dRegion ext = point3dRegionExtent(m_data, m_canvas->viewMode());
                if (!ext.valid)
                    return;
                m_regionMinSpin[i]->setValue(i == 0 ? ext.x0 : i == 1 ? ext.y0 : ext.z0);
                m_regionMaxSpin[i]->setValue(i == 0 ? ext.x1 : i == 1 ? ext.y1 : ext.z1);
            });
            row->addWidget(m_regionMinSpin[i], 1);
            row->addWidget(new QLabel(QStringLiteral("至"), g));
            row->addWidget(m_regionMaxSpin[i], 1);
            row->addWidget(fullBtn);
            gl->addLayout(row);
        }
        QHBoxLayout *brow = new QHBoxLayout;
        brow->setSpacing(ui::sz(6));
        m_regionApplyBtn = new QPushButton(QStringLiteral("应用"), g);
        m_regionApplyBtn->setToolTip(QStringLiteral("只显示框内数据，并把视图放大到框内"));
        m_regionResetBtn = new QPushButton(QStringLiteral("重置"), g);
        m_regionResetBtn->setToolTip(QStringLiteral("清除区域筛选，恢复全量数据"));
        brow->addWidget(m_regionApplyBtn);
        brow->addWidget(m_regionResetBtn);
        brow->addStretch(1);
        gl->addLayout(brow);
        lay->addWidget(g);
    }

    lay->addStretch(1);
    scroll->setWidget(page);
    return scroll;
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
    // 数据变了：各视图的区域区间按新量程夹紧/复位，再套用到画布
    revalidateRegions();
    loadRegionForCurrentView();
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
    for (int i = 0; i < 3; ++i)
        if (m_viewActions[i])
            m_viewActions[i]->setChecked(i == mode);
    // 切视图：加载该视图自己的区域筛选（三行标签 + 数值 + 是否已应用）
    loadRegionForCurrentView();
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
    if (m_viewRegions[int(m_canvas->viewMode())].applied)
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

// 左侧「高级」页：原来高级参数对话框的内容，改成常驻侧栏（点顶部「高级」展开/收起）
QWidget *Point3dWindow::buildAdvancedPanel()
{
    QScrollArea *scroll = new SideScrollArea(this);
    scroll->setObjectName(QStringLiteral("point3dAdvanced"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    QWidget *page = new QWidget(scroll);
    page->setObjectName(QStringLiteral("point3dAdvancedPage"));
    QVBoxLayout *lay = new QVBoxLayout(page);
    lay->setContentsMargins(ui::sz(8), ui::sz(8), ui::sz(8), ui::sz(8));
    lay->setSpacing(ui::sz(8));

    auto mkLine = [&](const QString &text) {
        QLineEdit *e = new QLineEdit(page);
        e->setText(text);
        return e;
    };
    auto mkSpin = [&](int lo, int hi, int value) {
        QSpinBox *sp = new QSpinBox(page);
        sp->setRange(lo, hi);
        sp->setValue(value);
        return sp;
    };
    auto mkDSpin = [&](double lo, double hi, int decimals, double value, double step) {
        QDoubleSpinBox *sp = new QDoubleSpinBox(page);
        sp->setRange(lo, hi);
        sp->setDecimals(decimals);
        sp->setSingleStep(step);
        sp->setValue(value);
        return sp;
    };
    auto addForm = [&](const QString &title) {
        QGroupBox *g = new QGroupBox(title, page);
        QFormLayout *f = new QFormLayout(g);
        f->setContentsMargins(ui::sz(8), ui::sz(10), ui::sz(8), ui::sz(8));
        f->setSpacing(ui::sz(6));
        f->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        lay->addWidget(g);
        return f;
    };

    {
        QFormLayout *f = addForm(QStringLiteral("解析（下次导入生效）"));
        m_detVarEdit = mkLine(setting("point3d.det_variable", QStringLiteral("DetPoint")).toString());
        m_sysVarEdit = mkLine(setting("point3d.syspara_variable", QStringLiteral("sysPara")).toString());
        m_azFieldCombo = new QComboBox(page);
        m_azFieldCombo->setEditable(true);
        m_azFieldCombo->addItems(QStringList() << QStringLiteral("az") << QStringLiteral("theta"));
        m_azFieldCombo->setCurrentText(
            setting("point3d.azimuth_field", QStringLiteral("az")).toString());
        m_trackVarEdit = mkLine(setting("point3d.track_variable", QStringLiteral("trackList")).toString());
        m_patternEdit = mkLine(setting("point3d.file_pattern", QStringLiteral("detPoint_*.mat")).toString());
        m_maxFramesSpin = mkSpin(0, 1000000, setting("point3d.max_frames", 0).toInt());
        m_maxFramesSpin->setSpecialValueText(QStringLiteral("全部"));
        f->addRow(QStringLiteral("DetPoint 变量"), m_detVarEdit);
        f->addRow(QStringLiteral("sysPara 变量"), m_sysVarEdit);
        f->addRow(QStringLiteral("方位字段"), m_azFieldCombo);
        f->addRow(QStringLiteral("航迹变量"), m_trackVarEdit);
        f->addRow(QStringLiteral("文件匹配模式"), m_patternEdit);
        f->addRow(QStringLiteral("最多帧数"), m_maxFramesSpin);
    }
    {
        QFormLayout *f = addForm(QStringLiteral("航迹"));
        m_minTrackLenSpin = mkSpin(1, 1000, setting("point3d.min_track_length", 4).toInt());
        m_rangeScaleSpin = mkDSpin(0.001, 1000.0, 3,
                                   setting("point3d.track_range_scale", 1.0).toDouble(), 0.1);
        f->addRow(QStringLiteral("最小航迹长度"), m_minTrackLenSpin);
        f->addRow(QStringLiteral("航迹距离缩放"), m_rangeScaleSpin);
    }
    {
        QFormLayout *f = addForm(QStringLiteral("连续性评分"));
        m_gapSpin = mkSpin(1, 20, setting("point3d.continuity_frame_gap", 3).toInt());
        m_rangeCSpin = mkDSpin(0.1, 100000.0, 1,
                               setting("point3d.continuity_range_scale", 80.0).toDouble(), 1.0);
        m_dopplerCSpin = mkDSpin(0.1, 1000.0, 1,
                                 setting("point3d.continuity_doppler_scale", 10.0).toDouble(), 0.5);
        m_azimuthCSpin = mkDSpin(0.1, 180.0, 1,
                                 setting("point3d.continuity_azimuth_scale", 6.0).toDouble(), 0.5);
        m_thresholdSpin = mkDSpin(0.0, 1.0, 2,
                                  setting("point3d.strong_threshold", 0.70).toDouble(), 0.05);
        f->addRow(QStringLiteral("帧间隔"), m_gapSpin);
        f->addRow(QStringLiteral("距离尺度"), m_rangeCSpin);
        f->addRow(QStringLiteral("速度尺度"), m_dopplerCSpin);
        f->addRow(QStringLiteral("方位尺度"), m_azimuthCSpin);
        f->addRow(QStringLiteral("强连续阈值"), m_thresholdSpin);
    }
    m_autoReloadCheck = new QCheckBox(QStringLiteral("启动时恢复上次的窗口与数据"), page);
    m_autoReloadCheck->setChecked(setting("point3d.auto_reload", true).toBool());
    lay->addWidget(m_autoReloadCheck);

    QLabel *note = new QLabel(QStringLiteral(
        "改动即时生效并写入设置：\n"
        "· 最小航迹长度 / 距离缩放 / 强连续阈值 立即作用于当前画面；\n"
        "· 连续性参数（帧间隔/尺度）停止输入约 0.4 秒后自动重算；\n"
        "· 变量名、方位字段、文件匹配模式、最多帧数在下次导入时生效；\n"
        "· 「启动时恢复」在下次启动时生效。"), page);
    note->setWordWrap(true);
    lay->addWidget(note);

    lay->addStretch(1);
    scroll->setWidget(page);
    return scroll;
}

// 高级页：所有控件改动即时写设置
void Point3dWindow::persistAdvanced()
{
    if (m_restoring || !m_detVarEdit)
        return;
    app::setValue(QStringLiteral("point3d.det_variable"), m_detVarEdit->text().trimmed());
    app::setValue(QStringLiteral("point3d.syspara_variable"), m_sysVarEdit->text().trimmed());
    app::setValue(QStringLiteral("point3d.azimuth_field"), m_azFieldCombo->currentText().trimmed());
    app::setValue(QStringLiteral("point3d.track_variable"), m_trackVarEdit->text().trimmed());
    app::setValue(QStringLiteral("point3d.file_pattern"), m_patternEdit->text().trimmed());
    app::setValue(QStringLiteral("point3d.max_frames"), m_maxFramesSpin->value());
    app::setValue(QStringLiteral("point3d.min_track_length"), m_minTrackLenSpin->value());
    app::setValue(QStringLiteral("point3d.track_range_scale"), m_rangeScaleSpin->value());
    app::setValue(QStringLiteral("point3d.continuity_frame_gap"), m_gapSpin->value());
    app::setValue(QStringLiteral("point3d.continuity_range_scale"), m_rangeCSpin->value());
    app::setValue(QStringLiteral("point3d.continuity_doppler_scale"), m_dopplerCSpin->value());
    app::setValue(QStringLiteral("point3d.continuity_azimuth_scale"), m_azimuthCSpin->value());
    app::setValue(QStringLiteral("point3d.strong_threshold"), m_thresholdSpin->value());
    app::setValue(QStringLiteral("point3d.auto_reload"), m_autoReloadCheck->isChecked());
}

// 连续性参数改动后（防抖）按新参数重算当前数据，点的大小/透明度立刻跟着变
void Point3dWindow::recomputeContinuityNow()
{
    if (m_data.frames.isEmpty())
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    ContinuityConfig cfg;
    cfg.frameGap = m_gapSpin->value();
    cfg.rangeScale = m_rangeCSpin->value();
    cfg.dopplerScale = m_dopplerCSpin->value();
    cfg.azimuthScale = m_azimuthCSpin->value();
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
    m_settingsBtn->setChecked(visible && m_sideStack->currentIndex() == 0);
    m_advancedBtn->setChecked(visible && m_sideStack->currentIndex() == 1);
    if (!visible)
        m_canvas->setRegionBox(Point3dRegion());
    else if (m_sideStack->currentIndex() == 0)
        loadRegionForCurrentView();
    if (!m_restoring) {
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
