#include "point3dcanvas.h"

#include "theme.h"

#include <QMouseEvent>
#include <QLineF>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

const double kSqrt3 = 1.7320508075688772;

double clamp01(double v)
{
    return qBound(0.0, v, 1.0);
}

// Google Turbo 色图（256 级 LUT），纯多项式生成，无外部依赖
const QColor *turboLut()
{
    static QColor lut[256];
    static bool ready = false;
    if (!ready) {
        for (int i = 0; i < 256; ++i) {
            const double x = i / 255.0;
            const double r = 0.13572138 + x * (4.61539260 + x * (-42.66032258 + x * (132.13108234
                           + x * (-152.94239396 + x * 59.28637943))));
            const double g = 0.09140261 + x * (2.19418839 + x * (4.84296658 + x * (-14.18503333
                           + x * (4.27729857 + x * 2.82956604))));
            const double b = 0.10667330 + x * (12.64194608 + x * (-60.58204836 + x * (110.36276771
                           + x * (-89.90310912 + x * 27.34824973))));
            lut[i] = QColor::fromRgbF(clamp01(r), clamp01(g), clamp01(b));
        }
        ready = true;
    }
    return lut;
}

QColor turboColor(double t)
{
    const int i = qBound(0, int(std::lround(clamp01(t) * 255.0)), 255);
    return turboLut()[i];
}

// 航迹默认配色：黄金角旋转，保证相邻航迹颜色差异大（MATLAB lines() 风格）
QColor distinctTrackColor(int index)
{
    const double hue = std::fmod(index * 137.508, 360.0) / 360.0;
    return QColor::fromHslF(hue, 0.78, 0.62);
}

QFont plotFont(int px, bool bold = false)
{
    QFont f;
    f.setFamilies(QStringList() << QStringLiteral("Segoe UI")
                                << QStringLiteral("Microsoft YaHei")
                                << QStringLiteral("WenQuanYi Micro Hei")
                                << QStringLiteral("Noto Sans CJK SC")
                                << QStringLiteral("DejaVu Sans"));
    f.setPixelSize(qMax(9, px));
    f.setBold(bold);
    return f;
}

QString viewAxisTitle(Point3dViewMode mode, bool xAxis)
{
    switch (mode) {
    case Point3dViewMode::RVF:
        return xAxis ? QStringLiteral("距离 / m") : QStringLiteral("Doppler 径向速度 / (m/s)");
    case Point3dViewMode::RAF:
        return xAxis ? QStringLiteral("距离 / m") : QStringLiteral("方位角 / (°)");
    case Point3dViewMode::VAF:
        return xAxis ? QStringLiteral("Doppler 径向速度 / (m/s)") : QStringLiteral("方位角 / (°)");
    }
    return QString();
}

QString viewName(Point3dViewMode mode)
{
    switch (mode) {
    case Point3dViewMode::RVF: return QStringLiteral("距离–速度–帧号");
    case Point3dViewMode::RAF: return QStringLiteral("距离–方位–帧号");
    case Point3dViewMode::VAF: return QStringLiteral("速度–方位–帧号");
    }
    return QString();
}

double niceStep(double span, int count)
{
    if (!(span > 0.0) || count < 2)
        return 1.0;
    const double raw = span / count;
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    const double n = raw / mag;
    double step = 10.0;
    if (n <= 1.0)
        step = 1.0;
    else if (n <= 2.0)
        step = 2.0;
    else if (n <= 5.0)
        step = 5.0;
    return step * mag;
}

QString tickText(double value, double step)
{
    int decimals = step < 1.0 ? int(std::ceil(-std::log10(step))) : 0;
    decimals = qBound(0, decimals, 3);
    return QString::number(value, 'f', decimals);
}

double distanceToSegment(const QPointF &p, const QPointF &a, const QPointF &b)
{
    const QPointF ab = b - a;
    const double len2 = ab.x() * ab.x() + ab.y() * ab.y();
    if (len2 <= 1e-12)
        return QLineF(p, a).length();
    double t = ((p.x() - a.x()) * ab.x() + (p.y() - a.y()) * ab.y()) / len2;
    t = qBound(0.0, t, 1.0);
    const QPointF q(a.x() + t * ab.x(), a.y() + t * ab.y());
    return QLineF(p, q).length();
}

} // namespace

Point3dCanvas::Point3dCanvas(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(480, 360);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    connect(UiScale::instance(), &UiScale::changed, this, [this]() { update(); });
    rebuildLimits();
}

void Point3dCanvas::setDataset(const Point3dDataset *data)
{
    m_data = data;
    m_current = (data && !data->frames.isEmpty()) ? data->frames.size() - 1 : 0;
    m_hoverPoint = -1;
    m_hoverTrack = -1;
    rebuildLimits();
    update();
}

void Point3dCanvas::refreshLimits()
{
    rebuildLimits();
    update();
}

void Point3dCanvas::setCurrentFrame(int index)
{
    if (!m_data || m_data->frames.isEmpty()) {
        if (m_current != 0) {
            m_current = 0;
            update();
        }
        return;
    }
    const int v = qBound(0, index, m_data->frames.size() - 1);
    if (v != m_current) {
        m_current = v;
        update();
    }
}

void Point3dCanvas::setWindowLength(int len)
{
    const int v = qMax(0, len);
    if (v != m_windowLength) {
        m_windowLength = v;
        update();
    }
}

void Point3dCanvas::setViewMode(Point3dViewMode mode)
{
    if (mode != m_viewMode) {
        m_viewMode = mode;
        update();
    }
}

void Point3dCanvas::setTrackSource(Point3dTrackSource source)
{
    if (source != m_trackSource) {
        m_trackSource = source;
        update();
    }
}

void Point3dCanvas::setShowDetections(bool on)
{
    if (on != m_showDetections) {
        m_showDetections = on;
        update();
    }
}

void Point3dCanvas::setShowTracks(bool on)
{
    if (on != m_showTracks) {
        m_showTracks = on;
        update();
    }
}

void Point3dCanvas::setShowLabels(bool on)
{
    if (on != m_showLabels) {
        m_showLabels = on;
        update();
    }
}

void Point3dCanvas::setHighlightedBatch(int batchNo)
{
    if (batchNo != m_highlightedBatch) {
        m_highlightedBatch = batchNo;
        update();
    }
}

void Point3dCanvas::setTrackLineWidth(double w)
{
    const double v = qBound(0.5, w, 12.0);
    if (!qFuzzyCompare(v, m_trackLineWidth)) {
        m_trackLineWidth = v;
        update();
    }
}

void Point3dCanvas::setPointScale(double s)
{
    const double v = qBound(0.2, s, 5.0);
    if (!qFuzzyCompare(v, m_pointScale)) {
        m_pointScale = v;
        update();
    }
}

void Point3dCanvas::setLabelScale(double s)
{
    const double v = qBound(0.5, s, 3.0);
    if (!qFuzzyCompare(v, m_labelScale)) {
        m_labelScale = v;
        update();
    }
}

void Point3dCanvas::setIsolatedBatches(const QSet<int> &batches)
{
    if (batches != m_isolatedBatches) {
        m_isolatedBatches = batches;
        update();
    }
}

void Point3dCanvas::setCamera(double yaw, double pitch, double zoom, double panX, double panY)
{
    m_cam.yaw = yaw;
    m_cam.pitch = qBound(-89.0, pitch, 89.0);
    m_cam.zoom = qBound(0.25, zoom, 8.0);
    m_panX = panX;
    m_panY = panY;
    update();
}

void Point3dCanvas::camera(double *yaw, double *pitch, double *zoom,
                           double *panX, double *panY) const
{
    if (yaw)
        *yaw = m_cam.yaw;
    if (pitch)
        *pitch = m_cam.pitch;
    if (zoom)
        *zoom = m_cam.zoom;
    if (panX)
        *panX = m_panX;
    if (panY)
        *panY = m_panY;
}

void Point3dCanvas::setDragMode(DragMode mode)
{
    if (mode == m_dragMode)
        return;
    m_dragMode = mode;
    setCursor(mode == DragMode::Pan ? Qt::OpenHandCursor : Qt::ArrowCursor);
    update();
}

void Point3dCanvas::resetCamera()
{
    m_cam = Camera();
    m_panX = 0.0;
    m_panY = 0.0;
    update();
    emit cameraChanged();
}

QRectF Point3dCanvas::plotRect() const
{
    const int left = ui::sz(52);
    const int top = ui::sz(34);
    const int right = ui::sz(96);
    const int bottom = ui::sz(48);
    return QRectF(left, top, qMax(1, width() - left - right), qMax(1, height() - top - bottom));
}

void Point3dCanvas::rebuildLimits()
{
    for (int m = 0; m < 3; ++m) {
        m_limits[m].xmin = std::numeric_limits<double>::infinity();
        m_limits[m].xmax = -std::numeric_limits<double>::infinity();
        m_limits[m].ymin = std::numeric_limits<double>::infinity();
        m_limits[m].ymax = -std::numeric_limits<double>::infinity();
    }
    if (!m_data)
        return;

    auto add = [this](Point3dViewMode mode, double range, double vr, double az) {
        double x = 0, y = 0;
        point3dViewCoordinates(mode, range, vr, az, &x, &y);
        Limits &l = m_limits[int(mode)];
        l.xmin = qMin(l.xmin, x);
        l.xmax = qMax(l.xmax, x);
        l.ymin = qMin(l.ymin, y);
        l.ymax = qMax(l.ymax, y);
    };
    for (const DetPointFrame &f : m_data->frames)
        for (const DetPointItem &p : f.points)
            for (int m = 0; m < 3; ++m)
                add(Point3dViewMode(m), p.range, p.vr, p.az);
    for (const TrackOverlay &t : m_data->tracks)
        for (const TrackOverlayPoint &p : t.points)
            for (int m = 0; m < 3; ++m)
                add(Point3dViewMode(m), p.prange, p.pvr, p.paz);

    for (int m = 0; m < 3; ++m) {
        Limits &l = m_limits[m];
        if (!std::isfinite(l.xmin) || !std::isfinite(l.xmax)) {
            l = Limits();
            continue;
        }
        double xs = l.xmax - l.xmin;
        double ys = l.ymax - l.ymin;
        if (!(xs > 0.0))
            xs = qMax(1.0, qAbs(l.xmax));
        if (!(ys > 0.0))
            ys = qMax(1.0, qAbs(l.ymax));
        l.xmin -= 0.05 * xs;
        l.xmax += 0.05 * xs;
        l.ymin -= 0.05 * ys;
        l.ymax += 0.05 * ys;
        if (m != int(Point3dViewMode::VAF) && l.xmin < 0.0)
            l.xmin = 0.0;                       // 距离轴不出现负值
    }
}

void Point3dCanvas::updateBasis(const QRectF &plot)
{
    const double az = qDegreesToRadians(m_cam.yaw);
    const double el = qDegreesToRadians(m_cam.pitch);
    const double ca = std::cos(az), sa = std::sin(az);
    const double ce = std::cos(el), se = std::sin(el);
    Basis &b = m_basis;
    b.ex = ce * ca;
    b.ey = ce * sa;
    b.ez = se;
    b.rx = -sa;
    b.ry = ca;
    b.rz = 0.0;
    b.ux = -se * ca;
    b.uy = -se * sa;
    b.uz = ce;
    b.scale = qMin(plot.width(), plot.height()) * 0.42 * m_cam.zoom;
    b.cx = plot.center().x() + m_panX;
    b.cy = plot.center().y() + m_panY;
}

void Point3dCanvas::zRange(double *lo, double *hi) const
{
    *lo = 0.0;
    *hi = 1.0;
    if (!m_data || m_data->frames.isEmpty())
        return;
    const int n = m_data->frames.size();
    const int cur = qBound(0, m_current, n - 1);
    int first = 0, last = n - 1;
    if (m_windowLength > 0) {
        first = qMax(0, cur - m_windowLength + 1);
        last = qMax(qMin(n - 1, m_windowLength - 1), cur);
    }
    *lo = m_data->frames.at(first).zValue;
    *hi = m_data->frames.at(last).zValue;
    if (!(*hi > *lo)) {
        *lo -= 0.5;
        *hi += 0.5;
    }
}

double Point3dCanvas::normalizedX(double x) const
{
    const Limits &l = m_limits[int(m_viewMode)];
    const double span = l.xmax - l.xmin;
    return span > 0.0 ? 2.0 * (x - l.xmin) / span - 1.0 : 0.0;
}

double Point3dCanvas::normalizedY(double y) const
{
    const Limits &l = m_limits[int(m_viewMode)];
    const double span = l.ymax - l.ymin;
    return span > 0.0 ? 2.0 * (y - l.ymin) / span - 1.0 : 0.0;
}

double Point3dCanvas::normalizedZ(double z, double zlo, double zhi) const
{
    const double span = zhi - zlo;
    return span > 0.0 ? 2.0 * (z - zlo) / span - 1.0 : 0.0;
}

QPointF Point3dCanvas::project(double nx, double ny, double nz, double *depth) const
{
    const Basis &b = m_basis;
    const double sx = nx * b.rx + ny * b.ry + nz * b.rz;
    const double sy = nx * b.ux + ny * b.uy + nz * b.uz;
    if (depth)
        *depth = nx * b.ex + ny * b.ey + nz * b.ez;
    return QPointF(b.cx + sx * b.scale, b.cy - sy * b.scale);
}

int Point3dCanvas::visibleFrameCount() const
{
    if (!m_data || m_data->frames.isEmpty())
        return 0;
    const int n = m_data->frames.size();
    const int cur = qBound(0, m_current, n - 1);
    const int first = m_windowLength > 0 ? qMax(0, cur - m_windowLength + 1) : 0;
    return cur - first + 1;
}

QString Point3dCanvas::statusText() const
{
    if (!m_data || m_data->frames.isEmpty())
        return QStringLiteral("未导入点迹数据");
    const int n = m_data->frames.size();
    const int cur = qBound(0, m_current, n - 1);
    const DetPointFrame &f = m_data->frames.at(cur);
    const QString fid = std::isfinite(f.frameId) ? QString::number(f.frameId, 'g', 12)
                                                 : QStringLiteral("-");
    return QStringLiteral("%1 | 帧 %2/%3 | frameID=%4 | 本帧点数=%5 | 窗口点数=%6 | 窗口航迹=%7")
        .arg(viewName(m_viewMode)).arg(cur + 1).arg(n).arg(fid)
        .arg(f.points.size()).arg(m_lastVisibleCount).arg(m_lastVisibleTracks);
}

void Point3dCanvas::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(9, 12, 20));

    const QRectF plot = plotRect();
    double zlo = 0.0, zhi = 1.0;
    zRange(&zlo, &zhi);
    updateBasis(plot);

    if (!m_data || m_data->frames.isEmpty()) {
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setFont(plotFont(ui::px(0.62)));
        p.setPen(QColor(150, 160, 180));
        p.drawText(plot, Qt::AlignCenter,
                   QStringLiteral("尚未导入点迹数据\n点击工具栏「导入点迹…」选择 detPoint_*.mat（可多选）"));
        return;
    }

    drawGridAndAxes(p, plot, zlo, zhi);
    drawDetections(p, zlo, zhi);
    drawTracks(p, zlo, zhi);
    drawHover(p);
    drawTitle(p, plot, zlo, zhi);
    drawColorBar(p, plot);

    // 重绘后统计数字才准确：通知窗口刷新状态栏（避免滞后一帧）
    const QString stats = statusText();
    if (stats != m_lastStatusText) {
        m_lastStatusText = stats;
        emit statsChanged();
    }
}

void Point3dCanvas::drawGridAndAxes(QPainter &p, const QRectF &plot, double zlo, double zhi)
{
    Q_UNUSED(zlo);
    Q_UNUSED(zhi);
    const Limits &lim = m_limits[int(m_viewMode)];

    auto P = [this](double nx, double ny, double nz, double *depth = nullptr) {
        return project(nx, ny, nz, depth);
    };

    // 立方体 8 角与 12 条棱
    static const double C[8][3] = {
        { -1, -1, -1 }, { 1, -1, -1 }, { 1, 1, -1 }, { -1, 1, -1 },
        { -1, -1, 1 }, { 1, -1, 1 }, { 1, 1, 1 }, { -1, 1, 1 }
    };
    static const int E[12][2] = {
        { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 },
        { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 },
        { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }
    };

    // 背面三面网格
    const double bx = m_basis.ex >= 0.0 ? -1.0 : 1.0;
    const double by = m_basis.ey >= 0.0 ? -1.0 : 1.0;
    const double bz = m_basis.ez >= 0.0 ? -1.0 : 1.0;
    p.setPen(QPen(QColor(140, 158, 184, 46), 1.0));
    const int div = 6;
    for (int i = 0; i <= div; ++i) {
        const double t = -1.0 + 2.0 * i / div;
        p.drawLine(P(bx, t, -1), P(bx, t, 1));
        p.drawLine(P(bx, -1, t), P(bx, 1, t));
        p.drawLine(P(t, by, -1), P(t, by, 1));
        p.drawLine(P(-1, by, t), P(1, by, t));
        p.drawLine(P(t, -1, bz), P(t, 1, bz));
        p.drawLine(P(-1, t, bz), P(1, t, bz));
    }

    p.setPen(QPen(QColor(200, 212, 230, 130), 1.0));
    for (const auto &e : E)
        p.drawLine(P(C[e[0]][0], C[e[0]][1], C[e[0]][2]),
                   P(C[e[1]][0], C[e[1]][1], C[e[1]][2]));

    // 刻度与轴标题
    p.setFont(plotFont(ui::px(0.46)));
    const QColor tickColor(214, 224, 240, 210);
    const QColor titleColor(230, 236, 246, 230);
    const int fontH = QFontMetricsF(p.font()).height();

    const double xStep = niceStep(lim.xmax - lim.xmin, 5);
    const double yStep = niceStep(lim.ymax - lim.ymin, 5);
    for (double v = lim.xmin; v <= lim.xmax + 1e-9; v += xStep) {
        const double nx = normalizedX(v);
        const QPointF pos = P(nx, -1, -1);
        p.setPen(tickColor);
        p.drawLine(pos, pos + QPointF(0, ui::sz(4)));
        p.drawText(QRectF(pos.x() - 30, pos.y() + ui::sz(6), 60, fontH),
                   Qt::AlignHCenter | Qt::AlignTop, tickText(v, xStep));
    }
    for (double v = lim.ymin; v <= lim.ymax + 1e-9; v += yStep) {
        const double ny = normalizedY(v);
        const QPointF pos = P(-1, ny, -1);
        p.setPen(tickColor);
        p.drawLine(pos, pos + QPointF(-ui::sz(4), 0));
        p.drawText(QRectF(pos.x() - 70, pos.y() - fontH / 2.0, 62, fontH),
                   Qt::AlignRight | Qt::AlignVCenter, tickText(v, yStep));
    }
    // Z 轴（帧号）刻度
    const double zStep = niceStep(zhi - zlo, 5);
    for (double v = zlo; v <= zhi + 1e-9; v += zStep) {
        const double nz = normalizedZ(v, zlo, zhi);
        const QPointF pos = P(-1, -1, nz);
        p.setPen(tickColor);
        p.drawLine(pos, pos + QPointF(-ui::sz(4), 0));
        p.drawText(QRectF(pos.x() - 70, pos.y() - fontH / 2.0, 62, fontH),
                   Qt::AlignRight | Qt::AlignVCenter, tickText(v, zStep));
    }

    p.setFont(plotFont(ui::px(0.52), true));
    p.setPen(titleColor);
    const QPointF xMid = P(0, -1, -1);
    p.drawText(QRectF(xMid.x() - 140, xMid.y() + ui::sz(8) + fontH, 280, fontH * 1.2),
               Qt::AlignHCenter | Qt::AlignTop, viewAxisTitle(m_viewMode, true));
    const QPointF yMid = P(-1, 0, -1);
    p.drawText(QRectF(yMid.x() - 220, yMid.y() - fontH / 2.0, 200, fontH),
               Qt::AlignRight | Qt::AlignVCenter, viewAxisTitle(m_viewMode, false));
    const QPointF zTop = P(-1, -1, 1);
    const double zTitleY = qMax<double>(ui::sz(3), zTop.y() - fontH * 1.8);
    p.drawText(QRectF(zTop.x() - 120, zTitleY, 240, fontH * 1.4),
               Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("帧号"));
    Q_UNUSED(plot);
}

void Point3dCanvas::drawDetections(QPainter &p, double zlo, double zhi)
{
    m_projected.clear();
    m_lastVisibleCount = 0;
    if (!m_data || m_data->frames.isEmpty() || !m_showDetections)
        return;

    const int n = m_data->frames.size();
    const int cur = qBound(0, m_current, n - 1);
    const int first = m_windowLength > 0 ? qMax(0, cur - m_windowLength + 1) : 0;

    int total = 0;
    for (int i = first; i <= cur; ++i)
        total += m_data->frames.at(i).points.size();
    const int step = total > 25000 ? int(std::ceil(double(total) / 25000.0)) : 1;
    const double snrSpan = qMax(1e-6, m_data->snrMax - m_data->snrMin);
    const double markerScale = qBound(0.6, UiScale::instance()->scale(), 1.5) * m_pointScale;

    m_projected.reserve(qMin(total, 25000) + 16);
    int globalIndex = 0;
    for (int fi = first; fi <= cur; ++fi) {
        const DetPointFrame &frame = m_data->frames.at(fi);
        const double nz = normalizedZ(frame.zValue, zlo, zhi);
        for (int pi = 0; pi < frame.points.size(); ++pi) {
            if ((globalIndex++ % step) != 0)
                continue;
            const DetPointItem &item = frame.points.at(pi);
            double x = 0, y = 0;
            point3dViewCoordinates(m_viewMode, item.range, item.vr, item.az, &x, &y);
            double depth = 0.0;
            const QPointF screen = project(normalizedX(x), normalizedY(y), nz, &depth);

            const double c = frame.continuity.size() == frame.points.size()
                                 ? double(frame.continuity.at(pi)) : 0.0;
            const double area = 3.0 + 0.5 * qMax(double(item.snr), 0.0) + 45.0 * c * c;
            const double radius = qBound(0.8, 1.333 * std::sqrt(area) * markerScale * 0.5, 9.0);

            ProjectedPoint pp;
            pp.screen = screen;
            pp.radius = float(radius);
            pp.frame = fi;
            pp.point = pi;
            pp.depth = depth;
            m_projected.append(pp);
        }
    }
    m_lastVisibleCount = m_projected.size() * step;
    if (m_projected.isEmpty())
        return;

    // 深度分桶（远 -> 近），避免逐帧排序
    const int kBins = 128;
    QVector<QVector<int>> bins(kBins);
    for (int i = 0; i < m_projected.size(); ++i) {
        const double t = clamp01((m_projected.at(i).depth / kSqrt3 + 1.0) * 0.5);
        const int b = qBound(0, int(t * (kBins - 1)), kBins - 1);
        bins[b].append(i);
    }

    const bool antialias = m_projected.size() <= 6000;
    p.setRenderHint(QPainter::Antialiasing, antialias);
    for (int b = 0; b < kBins; ++b) {
        for (int idx : qAsConst(bins[b])) {
            const ProjectedPoint &pp = m_projected.at(idx);
            const DetPointFrame &frame = m_data->frames.at(pp.frame);
            const DetPointItem &item = frame.points.at(pp.point);
            const double c = frame.continuity.size() == frame.points.size()
                                 ? double(frame.continuity.at(pp.point)) : 0.0;
            const double alpha = qBound(0.008, 0.008 + 0.90 * c * c, 0.92);
            QColor col = turboColor((double(item.snr) - m_data->snrMin) / snrSpan);
            col.setAlphaF(alpha);
            if (pp.radius <= 1.3) {
                p.setPen(QPen(col, qMax(1.0, double(pp.radius) * 2.0), Qt::SolidLine, Qt::RoundCap));
                p.drawPoint(pp.screen);
            } else {
                p.setPen(Qt::NoPen);
                p.setBrush(col);
                p.drawEllipse(pp.screen, pp.radius, pp.radius);
            }
        }
    }
    p.setBrush(Qt::NoBrush);
}

void Point3dCanvas::drawTracks(QPainter &p, double zlo, double zhi)
{
    m_tracks.clear();
    m_lastVisibleTracks = 0;
    if (!m_data || !m_showTracks || m_data->tracks.isEmpty())
        return;

    const int n = m_data->frames.size();
    const int cur = qBound(0, m_current, qMax(0, n - 1));
    const double curZ = m_data->frames.isEmpty() ? 0.0 : m_data->frames.at(cur).zValue;
    const double lineScale = qBound(0.7, UiScale::instance()->scale(), 1.4);

    // 当前高亮批次是否存在于本窗口：不存在时不要把其它航迹全部压暗
    bool hasHighlighted = false;
    if (m_highlightedBatch >= 0)
        for (const TrackOverlay &t : qAsConst(m_data->tracks))
            if (t.batchNo == m_highlightedBatch) {
                hasHighlighted = true;
                break;
            }

    p.setRenderHint(QPainter::Antialiasing, true);
    for (int ti = 0; ti < m_data->tracks.size(); ++ti) {
        const TrackOverlay &tr = m_data->tracks.at(ti);
        // 地图单独显示/筛选联动：只画被选中的批次
        if (!m_isolatedBatches.isEmpty() && !m_isolatedBatches.contains(tr.batchNo))
            continue;
        // 点源：同一条航迹可能同时带平滑点列与关联点列，按当前模式取其中一列
        // （平滑=主点列、关联=asscPointList 点列、预测=库中未存）
        const QVector<TrackOverlayPoint> *pts = point3dOverlayPointsFor(tr, m_trackSource);
        if (!pts)
            continue;
        ProjectedTrack out;
        out.track = ti;
        out.batchNo = tr.batchNo;
        out.source = tr.source;
        const double zmin = zlo;
        QPointF last;
        bool haveLast = false;
        qint64 t0 = 0, t1 = 0;
        for (const TrackOverlayPoint &op : *pts) {
            if (op.zValue < zmin || op.zValue > curZ + 1e-9)
                continue;
            double x = 0, y = 0;
            point3dViewCoordinates(m_viewMode, op.prange, op.pvr, op.paz, &x, &y);
            const QPointF screen = project(normalizedX(x), normalizedY(y),
                                           normalizedZ(op.zValue, zlo, zhi));
            out.polyline.append(screen);
            last = screen;
            haveLast = true;
            if (t0 == 0 || op.timeMs < t0)
                t0 = op.timeMs;
            if (op.timeMs > t1)
                t1 = op.timeMs;
        }
        if (!haveLast)
            continue;
        out.head = last;
        out.t0 = t0;
        out.t1 = t1;
        ++m_lastVisibleTracks;

        const bool highlighted = m_highlightedBatch >= 0 && tr.batchNo == m_highlightedBatch;
        const bool dimmed = hasHighlighted && !highlighted;
        QColor col = tr.color.isValid() ? tr.color : distinctTrackColor(ti);
        if (dimmed)
            col.setAlphaF(0.30);
        const double width = m_trackLineWidth * lineScale * (highlighted ? 2.0 : 1.0);

        if (out.polyline.size() >= 2) {
            p.setPen(QPen(col, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPolyline(out.polyline.constData(), out.polyline.size());
        }
        // 圆点（bead）
        p.setPen(QPen(QColor(240, 245, 255, dimmed ? 90 : 220), 1.0));
        p.setBrush(col);
        for (const QPointF &pt : qAsConst(out.polyline))
            p.drawEllipse(pt, width * 0.9, width * 0.9);
        // 菱形头
        const double d = qMax(3.5, width * 2.6);
        QPolygonF diamond;
        diamond << last + QPointF(0, -d) << last + QPointF(d, 0)
                << last + QPointF(0, d) << last + QPointF(-d, 0);
        p.setPen(QPen(Qt::white, 1.0));
        p.setBrush(col);
        p.drawPolygon(diamond);
        p.setBrush(Qt::NoBrush);

        if (m_showLabels) {
            p.setFont(plotFont(ui::px(0.48 * m_labelScale), true));
            const QString label = QStringLiteral("T%1").arg(tr.batchNo);
            const QFontMetricsF fm(p.font());
            const QRectF lr(last.x() + ui::sz(6), last.y() - fm.height() - ui::sz(2),
                            fm.width(label) + ui::sz(4), fm.height());
            p.setPen(dimmed ? QColor(col.red(), col.green(), col.blue(), 110) : col);
            p.drawText(lr, Qt::AlignLeft | Qt::AlignVCenter, label);
            out.labelRect = lr;
        }
        m_tracks.append(out);
    }
}

int Point3dCanvas::hitTrack(const QPointF &pos) const
{
    int best = -1;
    double bestDist = 8.0;
    for (int i = 0; i < m_tracks.size(); ++i) {
        const ProjectedTrack &t = m_tracks.at(i);
        if (t.labelRect.isValid() && t.labelRect.adjusted(-4, -4, 4, 4).contains(pos))
            return i;
        for (int j = 1; j < t.polyline.size(); ++j) {
            const double d = distanceToSegment(pos, t.polyline.at(j - 1), t.polyline.at(j));
            if (d < bestDist) {
                bestDist = d;
                best = i;
            }
        }
        if (t.polyline.size() == 1 && QLineF(pos, t.polyline.first()).length() < bestDist)
            best = i;
    }
    return best;
}

void Point3dCanvas::drawHover(QPainter &p)
{
    // 悬停航迹：加粗描边提示
    if (m_hoverTrack >= 0 && m_hoverTrack < m_tracks.size()) {
        const ProjectedTrack &t = m_tracks.at(m_hoverTrack);
        if (t.polyline.size() >= 2) {
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(QPen(QColor(255, 255, 255, 170), 1.0, Qt::DotLine));
            p.drawPolyline(t.polyline.constData(), t.polyline.size());
        }
    }
    if (m_hoverPoint < 0 || m_hoverPoint >= m_projected.size() || !m_data)
        return;

    // 画笔/画刷必须成对恢复：提示框用了不透明画刷，泄漏到后面的色标条会把渐变整个盖住
    p.save();
    const ProjectedPoint &pp = m_projected.at(m_hoverPoint);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(255, 255, 255, 220), 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(pp.screen, pp.radius + 4.0, pp.radius + 4.0);

    const DetPointFrame &frame = m_data->frames.at(pp.frame);
    const DetPointItem &item = frame.points.at(pp.point);
    const QString fid = std::isfinite(frame.frameId) ? QString::number(frame.frameId, 'g', 12)
                                                     : QString::number(frame.zValue, 'g', 12);
    const QStringList lines = QStringList()
        << QStringLiteral("点迹ID: %1").arg(item.pointID)
        << QStringLiteral("距离: %1 m").arg(double(item.range), 0, 'f', 1)
        << QStringLiteral("方位: %1°").arg(double(item.az), 0, 'f', 2)
        << QStringLiteral("速度: %1 m/s").arg(double(item.vr), 0, 'f', 2)
        << QStringLiteral("SNR: %1 dB").arg(double(item.snr), 0, 'f', 2)
        << QStringLiteral("帧号: %1").arg(fid);

    p.setFont(plotFont(ui::px(0.48)));
    const QFontMetricsF fm(p.font());
    double w = 0.0;
    for (const QString &s : lines)
        w = qMax(w, fm.width(s));
    const double lineH = fm.height() + ui::sz(2);
    const double pad = ui::sz(6);
    QRectF box(pp.screen.x() + 12, pp.screen.y() + 12,
               w + pad * 2, lineH * lines.size() + pad * 2 - ui::sz(2));
    if (box.right() > width() - 4)
        box.moveLeft(pp.screen.x() - box.width() - 12);
    if (box.bottom() > height() - 4)
        box.moveTop(pp.screen.y() - box.height() - 12);
    p.setPen(QPen(QColor(90, 100, 120, 220), 1.0));
    p.setBrush(QColor(14, 18, 28, 235));
    p.drawRoundedRect(box, 4, 4);
    p.setPen(QColor(232, 238, 248));
    for (int i = 0; i < lines.size(); ++i)
        p.drawText(QPointF(box.left() + pad, box.top() + pad + lineH * (i + 0.8)), lines.at(i));
    p.restore();
}

void Point3dCanvas::drawColorBar(QPainter &p, const QRectF &plot)
{
    if (!m_data)
        return;
    const double w = ui::sz(14);
    const double x = plot.right() + ui::sz(20);
    const QRectF bar(x, plot.top(), w, plot.height());
    if (bar.right() > width() - 4)
        return;

    // 色标条每次都完整重画：不受前面悬停提示等绘制残留画笔/画刷的影响
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    QLinearGradient grad(bar.left(), bar.bottom(), bar.left(), bar.top());
    for (int i = 0; i < 256; ++i)
        grad.setColorAt(i / 255.0, turboColor(i / 255.0));
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::NoBrush);
    p.fillRect(bar, grad);
    p.setPen(QPen(QColor(150, 160, 180, 180), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRect(bar);

    p.setFont(plotFont(ui::px(0.46)));
    p.setPen(QColor(214, 224, 240, 220));
    const int ticks = 5;
    const QFontMetricsF fm(p.font());
    for (int i = 0; i < ticks; ++i) {
        const double t = double(i) / (ticks - 1);
        const double y = bar.bottom() - t * bar.height();
        const double v = m_data->snrMin + t * (m_data->snrMax - m_data->snrMin);
        p.drawLine(QPointF(bar.right(), y), QPointF(bar.right() + ui::sz(4), y));
        p.drawText(QRectF(bar.right() + ui::sz(6), y - fm.height() / 2.0,
                          ui::sz(52), fm.height()),
                   Qt::AlignLeft | Qt::AlignVCenter, QString::number(v, 'g', 3));
    }
    p.setFont(plotFont(ui::px(0.5), true));
    const double titleH = QFontMetricsF(p.font()).height();
    p.drawText(QRectF(x - ui::sz(20), plot.top() - titleH - ui::sz(6),
                      ui::sz(90), titleH + ui::sz(4)),
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("SNR / dB"));
    p.restore();
}

void Point3dCanvas::drawTitle(QPainter &p, const QRectF &plot, double zlo, double zhi)
{
    Q_UNUSED(zlo);
    Q_UNUSED(zhi);
    p.setFont(plotFont(ui::px(0.52), true));
    p.setPen(QColor(232, 238, 248, 235));
    p.drawText(QRectF(plot.left(), ui::sz(6), plot.width(), ui::sz(22)),
               Qt::AlignLeft | Qt::AlignVCenter, statusText());
}

void Point3dCanvas::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        m_dragging = true;
        m_lastMouse = e->pos();
        setCursor(Qt::ClosedHandCursor);
        setFocus(Qt::MouseFocusReason);
    }
    QWidget::mousePressEvent(e);
}

void Point3dCanvas::mouseMoveEvent(QMouseEvent *e)
{
    if (m_dragging) {
        const QPoint d = e->pos() - m_lastMouse;
        m_lastMouse = e->pos();
        if (m_dragMode == DragMode::Pan) {
            m_panX += d.x();
            m_panY += d.y();
        } else {
            m_cam.yaw += d.x() * 0.4;
            m_cam.pitch = qBound(-89.0, m_cam.pitch - d.y() * 0.4, 89.0);
        }
        update();
        return;
    }

    // 悬停：先找点，再找航迹
    int bestPoint = -1;
    double bestDist = 1e9;
    for (int i = 0; i < m_projected.size(); ++i) {
        const ProjectedPoint &pp = m_projected.at(i);
        const double d = QLineF(e->pos(), pp.screen).length();
        const double limit = qMax(9.0, double(pp.radius) + 5.0);
        if (d <= limit && d < bestDist) {
            bestDist = d;
            bestPoint = i;
        }
    }
    int bestTrack = bestPoint < 0 ? hitTrack(e->pos()) : -1;
    if (bestPoint != m_hoverPoint || bestTrack != m_hoverTrack) {
        m_hoverPoint = bestPoint;
        m_hoverTrack = bestTrack;
        if (bestTrack >= 0 && bestTrack < m_tracks.size()) {
            const ProjectedTrack &t = m_tracks.at(bestTrack);
            emit trackFocusChanged(t.batchNo, t.source, t.t0, t.t1);
        } else if (m_tracks.size() > 0) {
            emit trackFocusChanged(-1, TrackSource::Radar, 0, 0);
        }
        update();
        emit hoverChanged();
    }
    // 平移模式下始终保持手爪（提示"可以拖着走"），旋转模式按是否命中点/航迹换指针
    if (m_dragMode == DragMode::Pan)
        setCursor(Qt::OpenHandCursor);
    else
        setCursor(bestPoint >= 0 || bestTrack >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    QWidget::mouseMoveEvent(e);
}

void Point3dCanvas::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_dragging && e->button() == Qt::LeftButton) {
        m_dragging = false;
        setCursor(m_dragMode == DragMode::Pan ? Qt::OpenHandCursor : Qt::ArrowCursor);
        emit cameraChanged();
        return;
    }
    QWidget::mouseReleaseEvent(e);
}

void Point3dCanvas::mouseDoubleClickEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        const int t = hitTrack(e->pos());
        if (t >= 0) {
            const ProjectedTrack &pt = m_tracks.at(t);
            emit trackActivated(pt.batchNo, pt.source, pt.t0, pt.t1);
        } else {
            resetCamera();
        }
    }
    QWidget::mouseDoubleClickEvent(e);
}

void Point3dCanvas::wheelEvent(QWheelEvent *e)
{
    m_cam.zoom = qBound(0.25, m_cam.zoom * std::pow(1.0015, e->angleDelta().y()), 8.0);
    update();
    emit cameraChanged();
    e->accept();
}

void Point3dCanvas::leaveEvent(QEvent *e)
{
    if (m_hoverPoint >= 0 || m_hoverTrack >= 0) {
        m_hoverPoint = -1;
        m_hoverTrack = -1;
        update();
        emit hoverChanged();
    }
    QWidget::leaveEvent(e);
}
