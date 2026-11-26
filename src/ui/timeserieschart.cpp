#include "timeserieschart.h"
#include "theme.h"
#include "track.h"

#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace {

double niceStep(double span, int targetTicks)
{
    if (!(span > 0.0) || !std::isfinite(span))
        return 1.0;
    const double raw = span / double(qMax(1, targetTicks));
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    const double norm = raw / mag;
    double step = 1.0;
    if (norm < 1.5) step = 1.0;
    else if (norm < 3.5) step = 2.0;
    else if (norm < 7.5) step = 5.0;
    else step = 10.0;
    return step * mag;
}

QString formatAxisVal(double v)
{
    if (std::fabs(v) >= 10000.0)
        return QString::number(v, 'f', 0);
    if (std::fabs(v) >= 100.0)
        return QString::number(v, 'f', 1);
    if (std::fabs(v) >= 1.0 || v == 0.0)
        return QString::number(v, 'f', 2);
    return QString::number(v, 'f', 3);
}

QString formatClockShort(qint64 ms)
{
    const QString full = formatBeijingTime(ms);   // "YYYY MM DD HH:mm:ss"
    return full.size() >= 19 ? full.mid(11, 8) : full;
}

} // namespace

TimeSeriesChart::TimeSeriesChart(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumHeight(145);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
}

void TimeSeriesChart::setTitle(const QString &title, const QString &unit)
{
    m_title = title;
    m_unit = unit;
    update();
}

void TimeSeriesChart::setSeries(const QVector<Series> &series)
{
    m_series = series;
    computeDataBounds();
    m_viewMinX = m_dataMinX;
    m_viewMaxX = m_dataMaxX;
    update();
}

void TimeSeriesChart::clear()
{
    m_series.clear();
    m_hasData = false;
    m_hoverValid = false;
    update();
}

void TimeSeriesChart::computeDataBounds()
{
    m_hasData = false;
    double x0 = 0.0, x1 = 0.0;
    for (const Series &s : qAsConst(m_series)) {
        for (const QPointF &pt : s.pts) {
            if (!std::isfinite(pt.x()) || !std::isfinite(pt.y()))
                continue;
            if (!m_hasData) {
                x0 = x1 = pt.x();
                m_hasData = true;
            } else {
                x0 = qMin(x0, pt.x());
                x1 = qMax(x1, pt.x());
            }
        }
    }
    if (!m_hasData) {
        m_dataMinX = 0.0;
        m_dataMaxX = 1000.0;
        return;
    }
    if (x1 <= x0) {
        x0 -= 5000.0;
        x1 += 5000.0;
    }
    m_dataMinX = x0;
    m_dataMaxX = x1;
}

void TimeSeriesChart::setXRange(double minMs, double maxMs, bool emitSignal)
{
    if (!std::isfinite(minMs) || !std::isfinite(maxMs) || maxMs <= minMs)
        return;
    if (qFuzzyCompare(m_viewMinX, minMs) && qFuzzyCompare(m_viewMaxX, maxMs))
        return;
    m_viewMinX = minMs;
    m_viewMaxX = maxMs;
    update();
    if (emitSignal)
        emit xRangeChanged(m_viewMinX, m_viewMaxX);
}

void TimeSeriesChart::resetZoom(bool emitSignal)
{
    setXRange(m_dataMinX, m_dataMaxX, emitSignal);
}

void TimeSeriesChart::setCursorTime(qint64 ms, bool active)
{
    if (m_cursorActive == active && m_cursorMs == ms)
        return;
    m_cursorActive = active;
    m_cursorMs = ms;
    update();
}

void TimeSeriesChart::setLinkedHoverTime(qint64 ms, bool valid)
{
    if (m_hoverValid == valid && (!valid || m_hoverMs == ms))
        return;
    m_hoverValid = valid;
    m_hoverMs = ms;
    update();
}

QRectF TimeSeriesChart::plotRect() const
{
    const double left = ui::sz(62);
    const double top = ui::sz(24);
    const double right = ui::sz(14);
    const double bottom = ui::sz(24);
    return QRectF(left, top, qMax(10.0, width() - left - right), qMax(10.0, height() - top - bottom));
}

void TimeSeriesChart::computeVisibleYRange(double *yMin, double *yMax) const
{
    bool any = false;
    double lo = 0.0, hi = 0.0;
    for (const Series &s : m_series) {
        for (const QPointF &pt : s.pts) {
            if (!std::isfinite(pt.y()))
                continue;
            if (pt.x() < m_viewMinX || pt.x() > m_viewMaxX)
                continue;
            if (!any) {
                lo = hi = pt.y();
                any = true;
            } else {
                lo = qMin(lo, pt.y());
                hi = qMax(hi, pt.y());
            }
        }
    }
    if (!any) {
        // 回退全局范围
        for (const Series &s : m_series) {
            for (const QPointF &pt : s.pts) {
                if (!std::isfinite(pt.y()))
                    continue;
                if (!any) {
                    lo = hi = pt.y();
                    any = true;
                } else {
                    lo = qMin(lo, pt.y());
                    hi = qMax(hi, pt.y());
                }
            }
        }
    }
    if (!any) {
        *yMin = 0.0;
        *yMax = 1.0;
        return;
    }
    const double span = hi - lo;
    const double pad = span > 1e-9 ? span * 0.08 : (std::fabs(lo) > 1e-6 ? std::fabs(lo) * 0.1 : 1.0);
    *yMin = lo - pad;
    *yMax = hi + pad;
}

double TimeSeriesChart::xToTime(double px, const QRectF &pr) const
{
    const double f = qBound(0.0, (px - pr.left()) / qMax(1.0, pr.width()), 1.0);
    return m_viewMinX + f * (m_viewMaxX - m_viewMinX);
}

double TimeSeriesChart::timeToX(double ms, const QRectF &pr) const
{
    const double span = qMax(1.0, m_viewMaxX - m_viewMinX);
    return pr.left() + ((ms - m_viewMinX) / span) * pr.width();
}

double TimeSeriesChart::valToY(double v, double yMin, double yMax, const QRectF &pr) const
{
    const double span = qMax(1e-9, yMax - yMin);
    return pr.bottom() - ((v - yMin) / span) * pr.height();
}

void TimeSeriesChart::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QColor bg = themeColor("bg-primary");
    const QColor plotBg = themeColor("bg-secondary");
    const QColor border = themeColor("border-primary");
    const QColor grid = themeColor("border-secondary");
    const QColor fg = themeColor("text-primary");
    const QColor fgMuted = themeColor("text-secondary");
    const QColor accent = themeColor("accent-primary");

    p.fillRect(rect(), bg);
    const QRectF pr = plotRect();
    p.fillRect(pr, plotBg);
    p.setPen(QPen(border, 1.0));
    p.drawRect(pr);

    QFont titleFont = font();
    titleFont.setPixelSize(ui::px(0.85));
    titleFont.setBold(true);
    QFont axisFont = font();
    axisFont.setPixelSize(ui::px(0.76));

    // 标题与图例（顶部一行）
    p.setFont(titleFont);
    p.setPen(fg);
    const QString head = m_unit.isEmpty() ? m_title : QStringLiteral("%1 (%2)").arg(m_title, m_unit);
    p.drawText(QRectF(pr.left(), 2, pr.width(), ui::sz(20)), Qt::AlignVCenter | Qt::AlignLeft, head);

    p.setFont(axisFont);
    const QFontMetricsF afm(axisFont);
    double lx = pr.right();
    for (int i = m_series.size() - 1; i >= 0; --i) {
        const Series &s = m_series[i];
        if (s.pts.isEmpty())
            continue;
        const double tw = afm.horizontalAdvance(s.name);
        lx -= tw;
        p.setPen(fgMuted);
        p.drawText(QRectF(lx, 2, tw + 2, ui::sz(20)), Qt::AlignVCenter | Qt::AlignLeft, s.name);
        lx -= 16.0;
        if (s.style == SeriesStyle::Dots) {
            p.setPen(Qt::NoPen);
            p.setBrush(s.color);
            p.drawEllipse(QPointF(lx + 6.0, 2.0 + ui::sz(10)), 3.0, 3.0);
        } else {
            QPen lp(s.color, 2.0, s.style == SeriesStyle::DashLine ? Qt::DashLine : Qt::SolidLine);
            p.setPen(lp);
            p.drawLine(QPointF(lx, 2.0 + ui::sz(10)), QPointF(lx + 12.0, 2.0 + ui::sz(10)));
        }
        lx -= 10.0;
    }

    if (!m_hasData) {
        p.setPen(fgMuted);
        p.drawText(pr, Qt::AlignCenter, QStringLiteral("无有效数据"));
        return;
    }

    double yMin = 0.0, yMax = 1.0;
    computeVisibleYRange(&yMin, &yMax);

    // Y 轴网格与刻度
    const double yStep = niceStep(yMax - yMin, qMax(3, int(pr.height() / 36.0)));
    const double yFirst = std::ceil(yMin / yStep) * yStep;
    p.setFont(axisFont);
    for (double yv = yFirst; yv <= yMax + 1e-9; yv += yStep) {
        const double py = valToY(yv, yMin, yMax, pr);
        if (py < pr.top() - 1.0 || py > pr.bottom() + 1.0)
            continue;
        p.setPen(QPen(grid, 1.0, Qt::DotLine));
        p.drawLine(QPointF(pr.left(), py), QPointF(pr.right(), py));
        p.setPen(fgMuted);
        p.drawText(QRectF(2, py - 9, pr.left() - 6, 18), Qt::AlignRight | Qt::AlignVCenter, formatAxisVal(yv));
    }

    // X 轴时间刻度
    const double xSpan = qMax(1.0, m_viewMaxX - m_viewMinX);
    const int xTicks = qBound(3, int(pr.width() / 95.0), 10);
    for (int i = 0; i <= xTicks; ++i) {
        const double tm = m_viewMinX + xSpan * double(i) / double(xTicks);
        const double px = timeToX(tm, pr);
        p.setPen(QPen(grid, 1.0, Qt::DotLine));
        p.drawLine(QPointF(px, pr.top()), QPointF(px, pr.bottom()));
        p.setPen(fgMuted);
        const QString lbl = formatClockShort(qint64(std::llround(tm)));
        const double w = afm.horizontalAdvance(lbl) + 4.0;
        const double tx = qBound(pr.left(), px - w * 0.5, pr.right() - w);
        p.drawText(QRectF(tx, pr.bottom() + 3, w, ui::sz(18)), Qt::AlignCenter, lbl);
    }

    // 曲线绘制（裁切在绘图区内）
    p.save();
    p.setClipRect(pr.adjusted(1, 1, -1, -1));
    for (const Series &s : qAsConst(m_series)) {
        if (s.pts.isEmpty())
            continue;
        if (s.style == SeriesStyle::Dots) {
            p.setPen(QPen(s.color, 1.3));
            p.setBrush(QColor(s.color.red(), s.color.green(), s.color.blue(), 170));
            for (const QPointF &pt : s.pts) {
                if (!std::isfinite(pt.y()) || pt.x() < m_viewMinX || pt.x() > m_viewMaxX)
                    continue;
                p.drawEllipse(QPointF(timeToX(pt.x(), pr), valToY(pt.y(), yMin, yMax, pr)), 2.8, 2.8);
            }
        } else {
            QPen pen(s.color, s.style == SeriesStyle::DashLine ? 1.5 : 1.8,
                     s.style == SeriesStyle::DashLine ? Qt::DashLine : Qt::SolidLine,
                     Qt::RoundCap, Qt::RoundJoin);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            QPainterPath path;
            bool started = false;
            for (const QPointF &pt : s.pts) {
                if (!std::isfinite(pt.y())) {
                    started = false;
                    continue;
                }
                const QPointF sp(timeToX(pt.x(), pr), valToY(pt.y(), yMin, yMax, pr));
                if (!started) {
                    path.moveTo(sp);
                    started = true;
                } else {
                    path.lineTo(sp);
                }
            }
            p.drawPath(path);
        }
    }

    // 主回放时钟竖线
    if (m_cursorActive && double(m_cursorMs) >= m_viewMinX && double(m_cursorMs) <= m_viewMaxX) {
        const double cx = timeToX(double(m_cursorMs), pr);
        p.setPen(QPen(QColor(0xf9, 0x73, 0x16), 1.6));
        p.drawLine(QPointF(cx, pr.top()), QPointF(cx, pr.bottom()));
    }

    // 悬停十字线与读数浮窗
    if (m_hoverValid && double(m_hoverMs) >= m_viewMinX && double(m_hoverMs) <= m_viewMaxX) {
        const double hx = timeToX(double(m_hoverMs), pr);
        p.setPen(QPen(accent, 1.0, Qt::DashLine));
        p.drawLine(QPointF(hx, pr.top()), QPointF(hx, pr.bottom()));

        QStringList tipLines;
        tipLines << formatBeijingTime(m_hoverMs);
        for (const Series &s : qAsConst(m_series)) {
            if (s.pts.isEmpty())
                continue;
            // 二分找最近采样点
            auto it = std::lower_bound(s.pts.constBegin(), s.pts.constEnd(), double(m_hoverMs),
                                       [](const QPointF &pt, double v) { return pt.x() < v; });
            int best = -1;
            double bestD = 1e30;
            if (it != s.pts.constEnd()) {
                bestD = std::fabs(it->x() - double(m_hoverMs));
                best = int(it - s.pts.constBegin());
            }
            if (it != s.pts.constBegin()) {
                const double d = std::fabs((it - 1)->x() - double(m_hoverMs));
                if (d < bestD) {
                    bestD = d;
                    best = int(it - s.pts.constBegin()) - 1;
                }
            }
            if (best >= 0 && std::isfinite(s.pts[best].y())) {
                const QPointF sp(timeToX(s.pts[best].x(), pr), valToY(s.pts[best].y(), yMin, yMax, pr));
                p.setPen(QPen(bg, 1.5));
                p.setBrush(s.color);
                p.drawEllipse(sp, 3.8, 3.8);
                tipLines << QStringLiteral("%1: %2").arg(s.name, formatAxisVal(s.pts[best].y()));
            }
        }
        double bw = 0.0;
        for (const QString &ln : qAsConst(tipLines))
            bw = qMax(bw, afm.horizontalAdvance(ln));
        const double lh = afm.height();
        const double boxW = bw + 12.0;
        const double boxH = lh * tipLines.size() + 8.0;
        const double bx = (hx + 10.0 + boxW <= pr.right()) ? (hx + 10.0) : qMax(pr.left() + 4.0, hx - 10.0 - boxW);
        const QRectF box(bx, pr.top() + 6.0, boxW, boxH);
        QColor tipBg = bg;
        tipBg.setAlpha(220);
        p.setPen(QPen(border, 1.0));
        p.setBrush(tipBg);
        p.drawRoundedRect(box, 3.0, 3.0);
        p.setPen(fg);
        for (int i = 0; i < tipLines.size(); ++i)
            p.drawText(QRectF(box.left() + 6.0, box.top() + 4.0 + i * lh, boxW - 12.0, lh),
                       Qt::AlignVCenter | Qt::AlignLeft, tipLines[i]);
    }

    // 框选缩放预览
    if (m_boxZooming) {
        const double x0 = qBound(pr.left(), double(qMin(m_pressPos.x(), m_curPos.x())), pr.right());
        const double x1 = qBound(pr.left(), double(qMax(m_pressPos.x(), m_curPos.x())), pr.right());
        QColor sel = accent;
        sel.setAlpha(48);
        p.fillRect(QRectF(x0, pr.top(), x1 - x0, pr.height()), sel);
        p.setPen(QPen(accent, 1.0));
        p.drawRect(QRectF(x0, pr.top(), x1 - x0, pr.height()));
    }
    p.restore();
}

void TimeSeriesChart::mousePressEvent(QMouseEvent *e)
{
    const QRectF pr = plotRect();
    if (!m_hasData || !pr.contains(e->pos()))
        return;
    m_pressPos = m_curPos = e->pos();
    if (e->button() == Qt::LeftButton) {
        m_boxZooming = true;
    } else if (e->button() == Qt::RightButton) {
        m_panning = true;
        m_panStartMinX = m_viewMinX;
        m_panStartMaxX = m_viewMaxX;
        setCursor(Qt::ClosedHandCursor);
    }
}

void TimeSeriesChart::mouseMoveEvent(QMouseEvent *e)
{
    const QRectF pr = plotRect();
    m_curPos = e->pos();
    if (!m_hasData)
        return;
    if (m_panning) {
        const double dx = double(e->pos().x() - m_pressPos.x());
        const double span = m_panStartMaxX - m_panStartMinX;
        const double dt = -dx / qMax(1.0, pr.width()) * span;
        setXRange(m_panStartMinX + dt, m_panStartMaxX + dt, true);
        return;
    }
    if (m_boxZooming) {
        update();
        return;
    }
    if (pr.contains(e->pos())) {
        const qint64 t = qint64(std::llround(xToTime(e->pos().x(), pr)));
        m_hoverValid = true;
        m_hoverMs = t;
        update();
        emit hoverTimeChanged(t, true);
    } else if (m_hoverValid) {
        m_hoverValid = false;
        update();
        emit hoverTimeChanged(0, false);
    }
}

void TimeSeriesChart::mouseReleaseEvent(QMouseEvent *e)
{
    const QRectF pr = plotRect();
    if (e->button() == Qt::RightButton && m_panning) {
        m_panning = false;
        unsetCursor();
        return;
    }
    if (e->button() == Qt::LeftButton && m_boxZooming) {
        m_boxZooming = false;
        const int dx = std::abs(e->pos().x() - m_pressPos.x());
        if (dx >= 8) {
            const double t0 = xToTime(qMin(m_pressPos.x(), e->pos().x()), pr);
            const double t1 = xToTime(qMax(m_pressPos.x(), e->pos().x()), pr);
            if (t1 - t0 >= 500.0)
                setXRange(t0, t1, true);
        } else if (pr.contains(e->pos())) {
            const qint64 t = qint64(std::llround(xToTime(e->pos().x(), pr)));
            emit timePicked(t);
        }
        update();
    }
}

void TimeSeriesChart::mouseDoubleClickEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && m_hasData)
        resetZoom(true);
}

void TimeSeriesChart::wheelEvent(QWheelEvent *e)
{
    const QRectF pr = plotRect();
    if (!m_hasData || !pr.contains(e->pos())) {
        e->ignore();
        return;
    }
    const double factor = e->angleDelta().y() > 0 ? 0.78 : 1.28;
    const double anchor = xToTime(e->pos().x(), pr);
    const double lo = anchor - (anchor - m_viewMinX) * factor;
    const double hi = anchor + (m_viewMaxX - anchor) * factor;
    if (hi - lo >= 1000.0) {
        const double clampedLo = qMax(m_dataMinX - (m_dataMaxX - m_dataMinX) * 0.1, lo);
        const double clampedHi = qMin(m_dataMaxX + (m_dataMaxX - m_dataMinX) * 0.1, hi);
        if (clampedHi > clampedLo)
            setXRange(clampedLo, clampedHi, true);
    }
    e->accept();
}

void TimeSeriesChart::leaveEvent(QEvent *)
{
    if (m_hoverValid) {
        m_hoverValid = false;
        update();
        emit hoverTimeChanged(0, false);
    }
}
