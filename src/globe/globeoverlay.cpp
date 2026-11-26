#include "globeoverlay.h"
#include "drawingmanager.h"
#include "geocalc.h"
#include "globecamera.h"
#include "globedrawcontroller.h"
#include "maptools.h"
#include "radarsite.h"
#include "spatiallasso.h"
#include "theme.h"
#include "track.h"
#include "tracklayer.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>
#include <cmath>
#include <limits>

namespace {

const double kFlagScaleDefault = 1.2;
const QColor kRulerColor(0xf5, 0x9e, 0x0b);        // #f59e0b
const QColor kLassoColor(0x10, 0xb9, 0x81);        // #10b981（RadarView 套索绿）
const QColor kSelectColor(0x3b, 0x82, 0xf6);       // #3b82f6（选中高亮蓝，和绘图橙区分开）
const double kSiteDotPx = 6.0;                      // 站点圆点半径：与 TrackLayer 端点圆球基准 6px 一致

// RadarView flagRenderer.ts「flag-pin」：32×32 红色图钉，白色描边，中间白点
QPixmap makeFlagPin(qreal dpr)
{
    const int size = 32;
    QPixmap pm(int(size * dpr), int(size * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor red(0xff, 0x44, 0x44);
    const QPointF c(size / 2.0, size / 2.0 - 4);
    p.setBrush(red);
    p.setPen(QPen(Qt::white, 2));
    p.drawEllipse(c, 10, 10);
    QPainterPath tail;
    tail.moveTo(size / 2.0 - 5, size / 2.0 + 2);
    tail.lineTo(size / 2.0, size - 4);
    tail.lineTo(size / 2.0 + 5, size / 2.0 + 2);
    p.setPen(Qt::NoPen);
    p.fillPath(tail, red);
    p.strokePath(tail, QPen(Qt::white, 1.5));
    p.setBrush(Qt::white);
    p.drawEllipse(c, 4, 4);
    return pm;
}

// Cesium FILL_AND_OUTLINE 文字：先描边再填充
void drawOutlinedText(QPainter &p, const QPointF &baseline, const QString &text, const QFont &font,
                      const QColor &fill, const QColor &outline, qreal outlineWidth)
{
    QPainterPath path;
    path.addText(baseline, font, text);
    p.strokePath(path, QPen(outline, outlineWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.fillPath(path, fill);
}

QFont pxFont(int px, bool bold = false)
{
    QFont f(QStringLiteral("Microsoft YaHei"));
    f.setFamilies(QStringList() << QStringLiteral("Segoe UI") << QStringLiteral("Microsoft YaHei")
                                << QStringLiteral("WenQuanYi Micro Hei") << QStringLiteral("Noto Sans CJK SC"));
    f.setPixelSize(px);
    f.setBold(bold);
    return f;
}

// 逻辑 rem 字号 -> 随全局根字号缩放的像素字号（地图内文字同样参与「字号大小」设置）
QFont remFont(double rem, bool bold = false)
{
    return pxFont(ui::px(rem), bold);
}

} // namespace

GlobeOverlay::GlobeOverlay(QWidget *parent)
    : QWidget(parent),
      m_flagScale(kFlagScaleDefault)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::NoFocus);
}

// 旗标图标：底边中点对准旗标位置，32px × 0.8 × 缩放
QRectF GlobeOverlay::flagIconRect(const QPointF &anchor) const
{
    const double s = 32.0 * 0.8 * m_flagScale;
    return QRectF(anchor.x() - s / 2.0, anchor.y() - s, s, s);
}

QString GlobeOverlay::flagAt(const QPointF &pos, const GlobeCamera &cam, const FlagStore *flags) const
{
    if (!flags)
        return QString();
    const QSize vp = parentWidget() ? parentWidget()->size() : size();
    const QVector<MapFlag> &list = flags->flags();
    for (int i = list.size() - 1; i >= 0; --i) {   // 后放的在上面
        QPointF sp;
        if (cam.visiblePos(list[i].lon, list[i].lat, 0.0, vp, &sp) && flagIconRect(sp).contains(pos))
            return list[i].id;
    }
    return QString();
}

QString GlobeOverlay::trackLabel(const Track &t)
{
    QStringList parts;
    if (!t.flightNo.isEmpty())
        parts << t.flightNo;
    if (!t.aircraftType.isEmpty())
        parts << t.aircraftType;
    return parts.isEmpty() ? t.id : parts.join(QStringLiteral(" | "));
}

void GlobeOverlay::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    m_labelsDrawn = m_flagsDrawn = m_rulerMarkersDrawn = m_lassoVerticesDrawn = 0;
    m_featuresDrawn = 0;
    m_filterOverlayItems = 0;
    m_compareLinksDrawn = 0;
    m_radarPpiPrimitives = 0;
    m_diagnosticsItemsDrawn = 0;
    if (!m_wanted || !m_ctx.camera)
        return;
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    drawRadarSites(p);
    drawFilterOverlay(p);
    drawCompareLinks(p);
    drawLabels(p);
    drawFeatures(p);
    drawInProgress(p);
    drawBand(p);
    drawRuler(p);
    drawLasso(p);
    drawFlags(p);
    drawDiagnosticsOverlay(p);
    drawFocusPoint(p);
    drawPointTip(p);
}

void GlobeOverlay::drawRuler(QPainter &p)
{
    m_rulerMarkersDrawn = 0;
    const RulerState *ruler = m_ctx.ruler;
    if (!ruler || !ruler->isActive() || ruler->waypoints().isEmpty())
        return;
    const QSize vp = size();
    const GlobeCamera &cam = *m_ctx.camera;
    const QVector<RulerState::Waypoint> &w = ruler->waypoints();
    QPen seg(kRulerColor, 2.0, Qt::CustomDashLine, Qt::FlatCap);
    seg.setDashPattern(QVector<qreal>() << 6.0 << 6.0);   // Cesium dashLength 12：一半实一半空
    p.setBrush(Qt::NoBrush);
    for (int i = 0; i + 1 < w.size(); ++i) {
        p.setPen(seg);
        for (const QPolygonF &line : cam.geodesicLines(w[i].lat, w[i].lon, w[i + 1].lat, w[i + 1].lon, vp))
            p.drawPolyline(line);
    }
    if (ruler->hasMouse()) {
        QColor c = kRulerColor;
        c.setAlphaF(0.45);
        QPen pre(c, 1.5, Qt::CustomDashLine, Qt::FlatCap);
        pre.setDashPattern(QVector<qreal>() << 8.0 / 1.5 / 2.0 << 8.0 / 1.5 / 2.0);
        p.setPen(pre);
        for (const QPolygonF &line : cam.geodesicLines(w.last().lat, w.last().lon,
                                                       ruler->mouseLat(), ruler->mouseLon(), vp))
            p.drawPolyline(line);
    }
    // 航点：r=11 琥珀色圆、黑色描边、白色粗体序号
    const QFont f = remFont(0.857, true);
    p.setFont(f);
    for (int i = 0; i < w.size(); ++i) {
        QPointF sp;
        if (!cam.visiblePos(w[i].lon, w[i].lat, 0.0, vp, &sp))
            continue;
        p.setPen(QPen(Qt::black, 1.5));
        p.setBrush(kRulerColor);
        const double r = ui::sz(11);
        p.drawEllipse(sp, r, r);
        p.setPen(Qt::white);
        p.drawText(QRectF(sp.x() - r, sp.y() - r, 2 * r, 2 * r), Qt::AlignCenter, QString::number(i + 1));
        ++m_rulerMarkersDrawn;
    }
    p.setBrush(Qt::NoBrush);
}

// 空间套索：填充多边形 + 边界 + 顶点序号；绘制中额外画「最后顶点→鼠标→首顶点」虚线预览。
// 填充与边界共用同一批采样点，保证两者严丝合缝。
void GlobeOverlay::drawLasso(QPainter &p)
{
    m_lassoVerticesDrawn = 0;
    const SpatialLasso *lasso = m_ctx.lasso;
    if (!lasso)
        return;
    const QVector<LassoVertex> &v = lasso->vertices();
    const int n = v.size();
    if (n == 0)
        return;
    const QSize vp = size();
    const GlobeCamera &cam = *m_ctx.camera;
    const bool closed = lasso->isClosed() && n >= 3;

    // 顶点投影（画序号用）
    QVector<QPointF> vpos(n);
    QVector<bool> vvis(n, false);
    for (int i = 0; i < n; ++i)
        vvis[i] = cam.visiblePos(v[i].lon, v[i].lat, 0.0, vp, &vpos[i]);

    // 边界采样：闭合时含收尾边，背面点自动断开
    QVector<QPointF> ring;
    QVector<bool> ringVis;
    for (int i = 0; i < n; ++i) {
        if (i + 1 >= n && !closed)
            break;
        const int j = (i + 1) % n;
        cam.sampleLonLatLine(v[i].lat, v[i].lon, v[j].lat, v[j].lon, vp, &ring, &ringVis);
    }
    if (closed) {              // 采样不含终点：补上首顶点，边界才是真正闭合的一圈
        ring << vpos[0];
        ringVis << vvis[0];
    }

    // 填充：闭合且整圈都在可见半球时才填，否则会出现穿过地球的怪异多边形
    bool fillOk = closed && ring.size() >= 3;
    for (int i = 0; i < ringVis.size() && fillOk; ++i)
        if (!ringVis[i])
            fillOk = false;
    if (fillOk) {
        QPainterPath path;
        path.moveTo(ring[0]);
        for (int i = 1; i < ring.size(); ++i)
            path.lineTo(ring[i]);
        path.closeSubpath();
        QColor fill = kLassoColor;
        fill.setAlphaF(0.15f);
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawPath(path);
    }

    // 边界
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(kLassoColor, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    QPolygonF cur;
    for (int i = 0; i < ring.size(); ++i) {
        if (ringVis[i]) {
            cur << ring[i];
        } else if (!cur.isEmpty()) {
            if (cur.size() > 1)
                p.drawPolyline(cur);
            cur.clear();
        }
    }
    if (cur.size() > 1)
        p.drawPolyline(cur);

    // 绘制中的预览虚线：最后顶点 → 鼠标，鼠标 → 首顶点（与最终边界同一套采样）
    if (lasso->isActive() && !lasso->isClosed() && lasso->hasMouse()) {
        QColor pc = kLassoColor;
        pc.setAlphaF(0.45f);
        QPen pre(pc, 1.5, Qt::CustomDashLine, Qt::FlatCap);
        pre.setDashPattern(QVector<qreal>() << 6.0 << 6.0);
        p.setPen(pre);
        auto dashLine = [&](double lat1, double lon1, double lat2, double lon2) {
            QVector<QPointF> pts;
            QVector<bool> vis;
            cam.sampleLonLatLine(lat1, lon1, lat2, lon2, vp, &pts, &vis);
            QPointF endPt;
            const bool endOk = cam.visiblePos(lon2, lat2, 0.0, vp, &endPt);
            pts.append(endPt);
            vis.append(endOk);
            QPolygonF line;
            for (int i = 0; i < pts.size(); ++i) {
                if (vis[i]) {
                    line << pts[i];
                } else if (!line.isEmpty()) {
                    if (line.size() > 1)
                        p.drawPolyline(line);
                    line.clear();
                }
            }
            if (line.size() > 1)
                p.drawPolyline(line);
        };
        dashLine(v.last().lat, v.last().lon, lasso->mouseLat(), lasso->mouseLon());
        if (n >= 2)
            dashLine(lasso->mouseLat(), lasso->mouseLon(), v[0].lat, v[0].lon);
    }

    // 顶点：绿色圆、黑色描边、白色序号（默认半径很小，圆太小时不画序号，免得糊成一团）
    const QFont f = remFont(0.857, true);
    p.setFont(f);
    const double r = ui::sz(m_lassoVertexPx);
    const bool showIndex = r >= ui::sz(6);
    for (int i = 0; i < n; ++i) {
        if (!vvis[i])
            continue;
        p.setPen(QPen(Qt::black, qMax(1.0, r / 6.0)));
        p.setBrush(kLassoColor);
        p.drawEllipse(vpos[i], r, r);
        if (showIndex) {
            p.setPen(Qt::white);
            p.drawText(QRectF(vpos[i].x() - r, vpos[i].y() - r, 2 * r, 2 * r),
                       Qt::AlignCenter, QString::number(i + 1));
        }
        ++m_lassoVerticesDrawn;
    }
    p.setBrush(Qt::NoBrush);
}

// 贴地绘图：DrawingManager 里的顶点都已经按大圆加密（圆是 Vincenty 正算点环，
// 折线每段用 slerp 插值），这里只做「WGS84 -> 屏幕」投影 + 折线，和标尺/套索同一套口径。
// 背面顶点投影失败时断开折线，否则会画出一条横穿地球的假线。
void GlobeOverlay::drawFeatures(QPainter &p)
{
    m_featuresDrawn = 0;
    const DrawingManager *draw = m_ctx.draw;
    if (!draw || draw->features().isEmpty())
        return;
    const QSize vp = size();
    const GlobeCamera &cam = *m_ctx.camera;
    const int selectedId = m_ctx.drawCtrl ? m_ctx.drawCtrl->selectedFeature() : 0;

    for (const DrawingFeature &f : draw->features()) {
        const bool sel = (f.id == selectedId);
        // 文本框：锚点投影后画描边文字（和航迹标签同一套描边参数）
        if (f.isText()) {
            if (f.pts.isEmpty())
                continue;
            QPointF sp;
            if (!cam.visiblePos(f.pts.first().x(), f.pts.first().y(), 0.0, vp, &sp))
                continue;
            QFont font = pxFont(ui::sz(int(f.textSizePx)), true);
            QColor outline(0, 0, 0, 210);
            drawOutlinedText(p, sp, f.text, font, sel ? kSelectColor : f.color, outline, 2.5);
            if (sel) {                       // 选中的文本：套一个虚线框，一眼看出选中的是谁
                const QRectF box = QFontMetricsF(font).boundingRect(f.text).translated(sp);
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(kSelectColor, 1.0, Qt::DashLine));
                p.drawRect(box.adjusted(-4, -3, 4, 3));
            }
            ++m_featuresDrawn;
            continue;
        }
        // 站点圆点：屏幕尺寸固定的实心圆，不随缩放变化（大小同航迹端点圆球）
        if (f.kind == DrawKind::Dot) {
            if (f.pts.isEmpty())
                continue;
            QPointF sp;
            if (!cam.visiblePos(f.pts.first().x(), f.pts.first().y(), 0.0, vp, &sp))
                continue;
            p.setPen(QPen(QColor(255, 255, 255, 210), qMax(1.0, f.widthPx * 0.5)));
            p.setBrush(sel ? kSelectColor : (f.fill.isValid() ? f.fill : f.color));
            p.drawEllipse(sp, kSiteDotPx, kSiteDotPx);
            if (sel) {
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(kSelectColor, 1.0, Qt::DashLine));
                p.drawEllipse(sp, kSiteDotPx + 4.0, kSiteDotPx + 4.0);
            }
            ++m_featuresDrawn;
            continue;
        }
        if (f.pts.size() < 2)
            continue;

        // 填充：只有整圈都在可见半球时才填，否则会出现穿过地球的怪异多边形
        if (f.fill.isValid() && f.isClosed()) {
            bool complete = false;
            const QPolygonF ring = GlobeDrawController::projectedRing(cam, vp, f, &complete);
            if (complete && ring.size() >= 3) {
                QPainterPath path;
                path.addPolygon(ring);
                path.closeSubpath();
                p.setPen(Qt::NoPen);
                p.setBrush(f.fill);
                p.drawPath(path);
            }
        }

        p.setBrush(Qt::NoBrush);
        // 纯填充要素不描边；没设填充色的填充要素按线画，避免完全看不见
        if (f.kind == DrawKind::Fill && f.fill.isValid()) {
            ++m_featuresDrawn;
            continue;
        }
        // 先把顶点投影成屏幕折线（背面断开），再统一描边：选中的先画一圈光晕加粗
        QVector<QPolygonF> chunks;
        QPolygonF cur;
        bool any = false;
        for (const QPointF &pt : f.pts) {        // x = 经度, y = 纬度
            QPointF sp;
            if (cam.visiblePos(pt.x(), pt.y(), 0.0, vp, &sp)) {
                cur << sp;
                any = true;
            } else if (!cur.isEmpty()) {
                if (cur.size() > 1)
                    chunks.append(cur);
                cur.clear();
            }
        }
        if (cur.size() > 1)
            chunks.append(cur);
        if (sel) {
            QColor halo = kSelectColor;
            halo.setAlphaF(0.45f);
            p.setPen(QPen(halo, qMax(1.0, f.widthPx) + 6.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            for (const QPolygonF &c : chunks)
                p.drawPolyline(c);
        }
        p.setPen(QPen(sel ? kSelectColor : f.color, qMax(1.0, f.widthPx) + (sel ? 1.0 : 0.0),
                      Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        for (const QPolygonF &c : chunks)
            p.drawPolyline(c);

        // 打断/裁剪点：白心黑边的小圆点。标记挂在要素上，要素删了自然就没了
        for (const QPointF &mk : f.marks) {
            QPointF sp;
            if (!cam.visiblePos(mk.x(), mk.y(), 0.0, vp, &sp))
                continue;
            p.setPen(QPen(QColor(0, 0, 0, 200), 1.0));
            p.setBrush(QColor(255, 255, 255));
            p.drawEllipse(sp, 3.5, 3.5);
            p.setBrush(Qt::NoBrush);
        }
        if (any)
            ++m_featuresDrawn;
    }
}

// 正在画的图形（预览）：直线/矩形从按下点连到鼠标，多边形画已有边 + 虚线预览，
// 自由曲线把已采到的点直接连起来。预览统一用半透明虚线，和已完成的图形区分开。
void GlobeOverlay::drawInProgress(QPainter &p)
{
    m_drawPreviewSegments = 0;
    const GlobeDrawController *dc = m_ctx.drawCtrl;
    if (!dc || dc->tool() == DrawTool::None)
        return;
    const QSize vp = size();
    const GlobeCamera &cam = *m_ctx.camera;
    const DrawTool tool = dc->tool();
    const QColor c = dc->color();
    QColor pc = c;
    pc.setAlphaF(0.55f);
    QPen solid(c, 3.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    QPen preview(pc, 2.0, Qt::CustomDashLine, Qt::FlatCap);
    preview.setDashPattern(QVector<qreal>() << 5.0 << 5.0);
    p.setBrush(Qt::NoBrush);

    const QVector<QPointF> &stroke = dc->stroke();
    if (dc->strokeActive() && tool == DrawTool::Polyline) {
        p.setPen(solid);
        QPolygonF poly;
        for (const QPointF &pt : stroke) {
            QPointF sp;
            if (cam.visiblePos(pt.x(), pt.y(), 0.0, vp, &sp))
                poly << sp;
        }
        if (poly.size() > 1) {
            p.drawPolyline(poly);
            ++m_drawPreviewSegments;
        }
        return;
    }

    const double curLon = qRadiansToDegrees(m_ctx.cursorLonRad);
    const double curLat = qRadiansToDegrees(m_ctx.cursorLatRad);
    const double pressLat = dc->pressLat();
    const double pressLon = dc->pressLon();
    if (dc->strokeActive() && (tool == DrawTool::Line || tool == DrawTool::Rect) && m_ctx.cursorValid) {
        p.setPen(preview);
        if (tool == DrawTool::Line) {
            for (const QPolygonF &line : cam.geodesicLines(pressLat, pressLon, curLat, curLon, vp)) {
                p.drawPolyline(line);
                ++m_drawPreviewSegments;
            }
        } else {
            const double s = qMin(pressLat, curLat), n = qMax(pressLat, curLat);
            const double w = qMin(pressLon, curLon), e = qMax(pressLon, curLon);
            const double lats[5] = { s, s, n, n, s };
            const double lons[5] = { w, e, e, w, w };
            for (int i = 0; i < 4; ++i) {
                for (const QPolygonF &line : cam.geodesicLines(lats[i], lons[i], lats[i + 1], lons[i + 1], vp)) {
                    p.drawPolyline(line);
                    ++m_drawPreviewSegments;
                }
            }
        }
    }

    // 裁剪/打断：拖动时显示一条红色虚线，就是这把「刀」走的路径
    if (tool == DrawTool::Break && dc->strokeActive() && m_ctx.cursorValid) {
        QPen knife(QColor(0xef, 0x44, 0x44), 2.0, Qt::CustomDashLine);
        knife.setDashPattern(QVector<qreal>() << 4.0 << 4.0);
        p.setPen(knife);
        for (const QPolygonF &line : cam.geodesicLines(pressLat, pressLon, curLat, curLon, vp)) {
            p.drawPolyline(line);
            ++m_drawPreviewSegments;
        }
    }

    if (tool == DrawTool::Polygon && !stroke.isEmpty()) {
        p.setPen(solid);
        for (int i = 0; i + 1 < stroke.size(); ++i) {
            for (const QPolygonF &line : cam.geodesicLines(stroke[i].y(), stroke[i].x(),
                                                           stroke[i + 1].y(), stroke[i + 1].x(), vp)) {
                p.drawPolyline(line);
                ++m_drawPreviewSegments;
            }
        }
        if (m_ctx.cursorValid) {
            p.setPen(preview);
            const QPointF &last = stroke.last();
            for (const QPolygonF &line : cam.geodesicLines(last.y(), last.x(), curLat, curLon, vp))
                p.drawPolyline(line);
            if (stroke.size() >= 2) {
                const QPointF &first = stroke.first();
                for (const QPolygonF &line : cam.geodesicLines(curLat, curLon, first.y(), first.x(), vp))
                    p.drawPolyline(line);
            }
        }
        p.setPen(QPen(Qt::black, 1.0));
        p.setBrush(c);
        for (const QPointF &pt : stroke) {
            QPointF sp;
            if (cam.visiblePos(pt.x(), pt.y(), 0.0, vp, &sp))
                p.drawEllipse(sp, 3.0, 3.0);
        }
        p.setBrush(Qt::NoBrush);
    }

    // 吸附提示：绿色小方块画在吸到的那个顶点上，告诉用户「这一下会落在这里」
    if (dc->snapActive()) {
        p.setPen(QPen(QColor(0, 0, 0, 170), 1.0));
        p.setBrush(dc->snapColor());
        p.drawRect(QRectF(dc->snapPos() - QPointF(4.0, 4.0), QSizeF(8.0, 8.0)));
        p.setBrush(Qt::NoBrush);
    }
}

// 框选删除的橡皮筋：半透明填充 + 虚线边框
void GlobeOverlay::drawBand(QPainter &p)
{
    const GlobeDrawController *dc = m_ctx.drawCtrl;
    if (!dc || !dc->boxSelecting() || dc->bandRect().isNull())
        return;
    QColor fill = kLassoColor;
    fill.setAlphaF(0.15f);
    p.setBrush(fill);
    p.setPen(QPen(kLassoColor, 1.0, Qt::DashLine));
    p.drawRect(dc->bandRect().normalized());
    p.setBrush(Qt::NoBrush);
}

void GlobeOverlay::drawFlags(QPainter &p)
{
    m_flagsDrawn = 0;
    const FlagStore *flags = m_ctx.flags;
    if (!flags || flags->flags().isEmpty())
        return;
    const QSize vp = size();
    const GlobeCamera &cam = *m_ctx.camera;
    MapFlag a, b;
    if (flags->selectedPair(&a, &b)) {
        QColor c(Qt::yellow);
        c.setAlphaF(0.8);
        p.setPen(QPen(c, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        for (const QPolygonF &line : cam.geodesicLines(a.lat, a.lon, b.lat, b.lon, vp))
            p.drawPolyline(line);
    }
    const qreal iconDpr = devicePixelRatioF() * 0.8 * m_flagScale;   // 按显示尺寸生成，缩放后不发虚
    if (m_flagIcon.isNull() || !qFuzzyCompare(m_flagIcon.devicePixelRatio(), iconDpr))
        m_flagIcon = makeFlagPin(iconDpr);
    const QFont f = remFont(0.857 * m_flagScale);
    const QFontMetricsF fm(f);
    for (const MapFlag &fl : flags->flags()) {
        QPointF sp;
        if (!cam.visiblePos(fl.lon, fl.lat, 0.0, vp, &sp))
            continue;
        p.drawPixmap(flagIconRect(sp), m_flagIcon, QRectF(QPointF(0, 0), QSizeF(m_flagIcon.size())));
        // 标签：黄色，黑色描边 2，顶边在旗标位置下方 round(8 × 缩放) px
        const double w = fm.horizontalAdvance(fl.label);
        const QPointF base(sp.x() - w / 2.0, sp.y() + std::lround(8 * m_flagScale) + fm.ascent());
        drawOutlinedText(p, base, fl.label, f, Qt::yellow, Qt::black, 2.0);
        ++m_flagsDrawn;
    }
}

// RadarView trackRenderer.ts：18px，填充为航迹线颜色、黑色描边 2，底边在端点上方 20px，水平居中
void GlobeOverlay::drawLabels(QPainter &p)
{
    m_labelsDrawn = 0;
    if (!m_showLabels || !m_ctx.trackStore || !m_ctx.trackLayer)
        return;
    const QSize vp = size();
    const GlobeCamera &cam = *m_ctx.camera;
    const QVector<TrackLayer::EndPoint> &eps = m_ctx.trackLayer->endpoints();
    const QVector<Track> &tracks = m_ctx.trackStore->tracks();
    const QFont f = remFont(1.286);
    const QFontMetricsF fm(f);
    const qreal dpr = devicePixelRatioF();
    if (m_labelCache.size() > 20000)
        m_labelCache.clear();
    for (const TrackLayer::EndPoint &e : eps) {
        if (e.track < 0 || e.track >= tracks.size())
            continue;
        QPointF sp;
        if (!cam.visiblePos(e.lon, e.lat, TrackLayer::kAltitudeM, vp, &sp))
            continue;
        if (sp.x() < -200 || sp.y() < -50 || sp.x() > width() + 200 || sp.y() > height() + 50)
            continue;
        const QString text = trackLabel(tracks[e.track]);
        const QColor color = m_ctx.trackLayer->trackColor(e.track);
        const QString key = text + QLatin1Char('\x1f') + color.name();
        auto it = m_labelCache.find(key);
        if (it == m_labelCache.end()) {
            const QSizeF sz(fm.horizontalAdvance(text) + 6.0, fm.height() + 4.0);
            QPixmap pm(int(std::ceil(sz.width() * dpr)), int(std::ceil(sz.height() * dpr)));
            pm.setDevicePixelRatio(dpr);
            pm.fill(Qt::transparent);
            QPainter lp(&pm);
            lp.setRenderHint(QPainter::Antialiasing);
            drawOutlinedText(lp, QPointF(3.0, 2.0 + fm.ascent()), text, f, color, Qt::black, 2.0);
            lp.end();
            it = m_labelCache.insert(key, pm);
        }
        const QSizeF sz = QSizeF(it->size()) / dpr;
        p.drawPixmap(QPointF(sp.x() - sz.width() / 2.0, sp.y() - ui::sz(20) - sz.height() + 2.0), *it);
        ++m_labelsDrawn;
    }
}

// RadarView pointDotRenderer.ts showPointDotHover：两行，14px 白字黑底，内边距 (8, 6)，在点右侧 点径+10 px
void GlobeOverlay::drawPointTip(QPainter &p)
{
    if (m_ctx.tipTrack < 0 || !m_ctx.trackStore || !m_ctx.trackLayer
            || m_ctx.tipTrack >= m_ctx.trackStore->size())
        return;
    const Track &t = m_ctx.trackStore->tracks()[m_ctx.tipTrack];
    const QVector<TrackPoint> &pts = t.pointsForMode(m_ctx.trackLayer->radarPointMode());
    if (m_ctx.tipPoint < 0 || m_ctx.tipPoint >= pts.size())
        return;
    const TrackPoint &pt = pts[m_ctx.tipPoint];
    QPointF sp;
    if (!m_ctx.camera->visiblePos(pt.lon, pt.lat, TrackLayer::kAltitudeM, size(), &sp))
        return;
    const QString label = !t.flightNo.isEmpty() ? t.flightNo : !t.registration.isEmpty() ? t.registration : t.id;
    const QString l1 = QStringLiteral("%1  ·  %2m  ·  %3kt  ·  %4°")
                           .arg(label).arg(double(pt.alt), 0, 'f', 0).arg(double(pt.speed), 0, 'f', 0)
                           .arg(double(pt.heading), 0, 'f', 0);
    const QString l2 = formatBeijingTime(pt.t);
    QFont f = remFont(1.0);
    f.setPixelSize(14);   // 悬停信息独立于主界面字号，避免大面板遮挡地图。
    const QFontMetricsF fm(f);
    const double w = qMax(fm.horizontalAdvance(l1), fm.horizontalAdvance(l2));
    const double lineH = fm.height();
    const double pad = 8, padY = 6;
    const QRectF box(sp.x() + m_ctx.trackLayer->pointDotPx() + 10, sp.y() - lineH - padY,
                     w + 2 * pad, lineH * 2 + 2 * padY);
    p.fillRect(box, Qt::black);
    QColor outline(Qt::black);
    outline.setAlphaF(0.9);
    drawOutlinedText(p, QPointF(box.left() + pad, box.top() + padY + fm.ascent()), l1, f, Qt::white, outline, 2.0);
    drawOutlinedText(p, QPointF(box.left() + pad, box.top() + padY + lineH + fm.ascent()), l2, f, Qt::white, outline, 2.0);
}

void GlobeOverlay::drawFocusPoint(QPainter &p)
{
    if (!m_ctx.focusValid || !m_ctx.camera)
        return;
    QPointF sp;
    if (!m_ctx.camera->visiblePos(m_ctx.focusLon, m_ctx.focusLat, TrackLayer::kAltitudeM, size(), &sp))
        return;
    p.save();
    const QColor ring(0x38, 0xbd, 0xf8);
    // 外圈 + 准星十字线 + 内实心点
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Qt::black, 4.0));
    p.drawEllipse(sp, 14.0, 14.0);
    p.setPen(QPen(ring, 2.0));
    p.drawEllipse(sp, 14.0, 14.0);
    const double arm = 20.0;
    const double gap = 6.0;
    p.drawLine(QPointF(sp.x() - arm, sp.y()), QPointF(sp.x() - gap, sp.y()));
    p.drawLine(QPointF(sp.x() + gap, sp.y()), QPointF(sp.x() + arm, sp.y()));
    p.drawLine(QPointF(sp.x(), sp.y() - arm), QPointF(sp.x(), sp.y() - gap));
    p.drawLine(QPointF(sp.x(), sp.y() + gap), QPointF(sp.x(), sp.y() + arm));
    p.setPen(QPen(Qt::black, 1.5));
    p.setBrush(ring);
    p.drawEllipse(sp, 4.0, 4.0);
    if (!m_ctx.focusLabel.isEmpty()) {
        const QFont f = remFont(0.857, true);
        const QFontMetricsF fm(f);
        const QPointF base(sp.x() + 18.0, sp.y() - 6.0);
        drawOutlinedText(p, base, m_ctx.focusLabel, f, ring, Qt::black, 2.5);
    }
    p.restore();
}

void GlobeOverlay::drawFilterOverlay(QPainter &p)
{
    m_filterOverlayItems = 0;
    const FilterOverlayState &st = m_ctx.filterOverlay;
    if (!st.enabled || !m_ctx.camera || !m_ctx.trackStore || !m_ctx.trackLayer)
        return;
    const QSize vp = size();
    const GlobeCamera &cam = *m_ctx.camera;
    const TrackLayer *layer = m_ctx.trackLayer;
    const QVector<Track> &tracks = m_ctx.trackStore->tracks();

    const bool replay = layer->isReplayActive();
    const qint64 rTime = layer->replayTimeMs();
    const double trailSec = layer->trailSeconds();
    const qint64 winLo = (replay && trailSec > 0.0) ? (rTime - qint64(trailSec * 1000.0)) : std::numeric_limits<qint64>::min();
    const qint64 winHi = replay ? rTime : std::numeric_limits<qint64>::max();

    auto inWindow = [winLo, winHi](qint64 t) { return t >= winLo && t <= winHi; };

    // 防止全量几千条航迹同时开启叠加导致软绘卡顿：上限 5000 点
    const int kPointBudget = 5000;
    int budgetUsed = 0;
    bool budgetHit = false;

    const QColor colAssc(0x22, 0xc5, 0x5e);     // 绿：关联点（空心菱形）
    const QColor colPred(0xf5, 0x9e, 0x0b);     // 琥珀：预测点（叉号+虚线）
    const QColor colRes(0xf4, 0x3f, 0x5e);      // 玫红：预测 -> 关联 残差新息线

    p.save();
    for (int i = 0; i < tracks.size(); ++i) {
        if (!layer->isTrackShown(i))
            continue;
        const Track &t = tracks[i];
        if (t.source != TrackSource::Radar || (!t.hasRawPoints() && !t.hasPredPoints()))
            continue;
        const int need = t.rawPoints.size() + t.predPoints.size();
        if (budgetUsed > 0 && budgetUsed + need > kPointBudget) {
            budgetHit = true;
            break;
        }
        budgetUsed += need;

        // 1) 预测点折线 + × 标记
        if (st.showPredicted && t.hasPredPoints()) {
            QPen dashPen(QColor(colPred.red(), colPred.green(), colPred.blue(), 190), 1.4, Qt::DashLine);
            p.setPen(dashPen);
            p.setBrush(Qt::NoBrush);
            QPolygonF poly;
            for (const TrackPoint &pt : t.predPoints) {
                if (!inWindow(pt.t))
                    continue;
                QPointF sp;
                if (cam.visiblePos(pt.lon, pt.lat, TrackLayer::kAltitudeM, vp, &sp)) {
                    poly.append(sp);
                } else if (poly.size() >= 2) {
                    p.drawPolyline(poly);
                    poly.clear();
                } else {
                    poly.clear();
                }
            }
            if (poly.size() >= 2)
                p.drawPolyline(poly);

            p.setPen(QPen(colPred, 1.5));
            for (const TrackPoint &pt : t.predPoints) {
                if (!inWindow(pt.t))
                    continue;
                QPointF sp;
                if (!cam.visiblePos(pt.lon, pt.lat, TrackLayer::kAltitudeM, vp, &sp))
                    continue;
                const double r = 3.2;
                p.drawLine(QPointF(sp.x() - r, sp.y() - r), QPointF(sp.x() + r, sp.y() + r));
                p.drawLine(QPointF(sp.x() - r, sp.y() + r), QPointF(sp.x() + r, sp.y() - r));
                ++m_filterOverlayItems;
            }
        }

        // 2) 预测 -> 关联 残差新息连线
        if (st.showResiduals && t.hasRawPoints() && t.hasPredPoints()) {
            const QVector<trackanalysis::FilterResidualPoint> res = trackanalysis::computeFilterResiduals(t);
            p.setPen(QPen(QColor(colRes.red(), colRes.green(), colRes.blue(), 220), 1.6, Qt::SolidLine));
            for (const auto &r : res) {
                if (!r.hasPred || !inWindow(r.timeMs))
                    continue;
                QPointF spP, spA;
                if (cam.visiblePos(r.predLon, r.predLat, TrackLayer::kAltitudeM, vp, &spP)
                        && cam.visiblePos(r.asscLon, r.asscLat, TrackLayer::kAltitudeM, vp, &spA)) {
                    p.drawLine(spP, spA);
                    ++m_filterOverlayItems;
                }
            }
        }

        // 3) 关联点（空心菱形）
        if (st.showAssociated && t.hasRawPoints()) {
            p.setPen(QPen(colAssc, 1.6));
            p.setBrush(QColor(colAssc.red(), colAssc.green(), colAssc.blue(), 60));
            for (const TrackPoint &pt : t.rawPoints) {
                if (!inWindow(pt.t))
                    continue;
                QPointF sp;
                if (!cam.visiblePos(pt.lon, pt.lat, TrackLayer::kAltitudeM, vp, &sp))
                    continue;
                const double r = 4.2;
                const QPointF dia[4] = {
                    QPointF(sp.x(), sp.y() - r), QPointF(sp.x() + r, sp.y()),
                    QPointF(sp.x(), sp.y() + r), QPointF(sp.x() - r, sp.y())
                };
                p.drawPolygon(dia, 4);
                ++m_filterOverlayItems;
            }
        }
    }

    // 右上角轻量图例（开启叠加对比时常驻）
    const QFont f = remFont(0.78, false);
    const QFontMetricsF fm(f);
    QString legend = QStringLiteral("滤波叠加：◇关联点  ×预测点  —新息残差");
    if (budgetHit)
        legend += QStringLiteral("（已达上限，请独显或筛选少量航迹）");
    const double w = fm.horizontalAdvance(legend) + 16.0;
    const double h = fm.height() + 8.0;
    const QRectF box(width() - w - 12.0, 10.0, w, h);
    p.setPen(QPen(QColor(255, 255, 255, 60), 1.0));
    p.setBrush(QColor(15, 23, 42, 190));
    p.drawRoundedRect(box, 4.0, 4.0);
    p.setFont(f);
    p.setPen(Qt::white);
    p.drawText(box, Qt::AlignCenter, legend);
    p.restore();
}

void GlobeOverlay::drawCompareLinks(QPainter &p)
{
    m_compareLinksDrawn = 0;
    if (!m_ctx.compareLinksVisible || !m_ctx.compareSamples || m_ctx.compareSamples->isEmpty() || !m_ctx.camera)
        return;
    const QSize vp = size();
    const GlobeCamera &cam = *m_ctx.camera;
    const QVector<trackanalysis::TrackDiffSample> &samples = *m_ctx.compareSamples;

    p.save();
    for (int i = 0; i < samples.size(); ++i) {
        const auto &s = samples[i];
        QPointF spA, spB;
        if (!cam.visiblePos(s.lonA, s.latA, TrackLayer::kAltitudeM, vp, &spA)
                || !cam.visiblePos(s.lonB, s.latB, TrackLayer::kAltitudeM, vp, &spB))
            continue;
        const bool isMax = (i == m_ctx.compareMaxIdx);
        p.setPen(QPen(isMax ? QColor(0xef, 0x44, 0x44) : QColor(0xec, 0x48, 0x99, 195),
                      isMax ? 2.6 : 1.4));
        p.drawLine(spA, spB);
        if (isMax) {
            p.setBrush(QColor(0xef, 0x44, 0x44));
            p.drawEllipse(spB, 4.5, 4.5);
            const QFont f = remFont(0.8, true);
            const QString txt = QStringLiteral("Max %1 km").arg(s.geoDistKm, 0, 'f', 2);
            drawOutlinedText(p, QPointF(spB.x() + 8.0, spB.y() - 6.0), txt, f,
                             QColor(0xff, 0x80, 0x80), Qt::black, 2.2);
        }
        ++m_compareLinksDrawn;
    }
    p.restore();
}

void GlobeOverlay::drawRadarSites(QPainter &p)
{
    m_radarPpiPrimitives = 0;
    const RadarSiteStore *store = m_ctx.radarSites;
    if (!store || !store->ppiVisible() || store->sites().isEmpty() || !m_ctx.camera)
        return;
    const QSize vp = size();
    const GlobeCamera &cam = *m_ctx.camera;
    const QFont lblFont = remFont(0.76, true);
    const QFont tickFont = remFont(0.70, false);

    // 沿指定半径从 az0 到 az1 采样球面等距弧并投影为屏幕上可见折线段
    auto drawRangeArc = [&](double aLat, double aLon, double rKm, double az0, double az1,
                            const QPen &pen, QPointF *lastVisible = nullptr) {
        double span = az1 - az0;
        if (span <= 0.0)
            span += 360.0;
        const int steps = qBound(16, int(std::ceil(span / 2.0)), 180);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        QPolygonF seg;
        bool hasLast = false;
        for (int k = 0; k <= steps; ++k) {
            const double az = az0 + span * double(k) / double(steps);
            double lat = 0.0, lon = 0.0;
            if (!geocalc::destination(aLat, aLon, az, rKm, &lat, &lon))
                continue;
            QPointF sp;
            if (cam.visiblePos(lon, lat, 0.0, vp, &sp)) {
                seg.append(sp);
                if (lastVisible) {
                    *lastVisible = sp;
                    hasLast = true;
                }
            } else if (seg.size() >= 2) {
                p.drawPolyline(seg);
                ++m_radarPpiPrimitives;
                seg.clear();
            } else {
                seg.clear();
            }
        }
        if (seg.size() >= 2) {
            p.drawPolyline(seg);
            ++m_radarPpiPrimitives;
        }
        return hasLast;
    };

    p.save();
    for (const RadarSite &s : store->sites()) {
        if (!s.visible || !s.isValid())
            continue;
        double aLat = 0.0, aLon = 0.0;
        if (!s.anchorLatLon(&aLat, &aLon))
            continue;
        const QColor base = s.color.isValid() ? s.color : QColor(0x38, 0xbd, 0xf8);

        // 1) 探测扇区与同心距离环 + 方位射线
        if (s.maxRangeKm > 0.0 && (s.hasSector || s.showFullRings)) {
            const double az0 = s.showFullRings ? 0.0 : s.startAzDeg;
            const double az1 = s.showFullRings ? 360.0 : s.endAzDeg;
            double azSpan = az1 - az0;
            if (azSpan <= 0.0)
                azSpan += 360.0;

            // 半透明探测扇区底色填充（仅在非整圆扇区时）
            if (!s.showFullRings && azSpan < 359.5) {
                QPolygonF sectorPoly;
                const int steps = qBound(12, int(std::ceil(azSpan / 3.0)), 120);
                bool allVis = true;
                for (int k = 0; k <= steps; ++k) {
                    double la = 0.0, lo = 0.0;
                    if (geocalc::destination(aLat, aLon, az0 + azSpan * double(k) / double(steps), s.maxRangeKm, &la, &lo)) {
                        QPointF sp;
                        if (cam.visiblePos(lo, la, 0.0, vp, &sp))
                            sectorPoly.append(sp);
                        else
                            allVis = false;
                    }
                }
                if (s.minRangeKm > 1.0) {
                    for (int k = steps; k >= 0; --k) {
                        double la = 0.0, lo = 0.0;
                        if (geocalc::destination(aLat, aLon, az0 + azSpan * double(k) / double(steps), s.minRangeKm, &la, &lo)) {
                            QPointF sp;
                            if (cam.visiblePos(lo, la, 0.0, vp, &sp))
                                sectorPoly.append(sp);
                            else
                                allVis = false;
                        }
                    }
                } else {
                    QPointF cSp;
                    if (cam.visiblePos(aLon, aLat, 0.0, vp, &cSp))
                        sectorPoly.append(cSp);
                    else
                        allVis = false;
                }
                if (allVis && sectorPoly.size() >= 3) {
                    QColor fill = base;
                    fill.setAlpha(28);
                    p.setPen(Qt::NoPen);
                    p.setBrush(fill);
                    p.drawPolygon(sectorPoly);
                    ++m_radarPpiPrimitives;
                }
            }

            // 同心距离环（按步长取整）
            const double stepKm = s.effectiveRingStepKm();
            QColor gridCol = base;
            gridCol.setAlpha(120);
            const QPen ringPen(gridCol, 1.0, Qt::SolidLine);
            const QPen edgePen(QColor(base.red(), base.green(), base.blue(), 210), 1.6, Qt::SolidLine);

            if (s.minRangeKm > 1.0)
                drawRangeArc(aLat, aLon, s.minRangeKm, az0, az1, edgePen);
            QPointF outerEnd;
            if (drawRangeArc(aLat, aLon, s.maxRangeKm, az0, az1, edgePen, &outerEnd)) {
                drawOutlinedText(p, outerEnd + QPointF(6.0, 4.0),
                                 QStringLiteral("%1km").arg(s.maxRangeKm, 0, 'f', 0),
                                 tickFont, base, Qt::black, 2.0);
            }
            const double firstRing = std::ceil(qMax(stepKm, s.minRangeKm + 1e-3) / stepKm) * stepKm;
            for (double r = firstRing; r < s.maxRangeKm - stepKm * 0.15; r += stepKm) {
                QPointF endPt;
                if (drawRangeArc(aLat, aLon, r, az0, az1, ringPen, &endPt)) {
                    drawOutlinedText(p, endPt + QPointF(5.0, 4.0),
                                     QStringLiteral("%1km").arg(r, 0, 'f', 0),
                                     tickFont, base.lighter(115), Qt::black, 1.8);
                }
            }

            // 方位射线（边缘线 + 步长网格线）
            const double azStep = qBound(2.0, s.azStepDeg, 90.0);
            auto drawRay = [&](double az, const QPen &pen, const QString &tag) {
                double la0 = aLat, lo0 = aLon, la1 = aLat, lo1 = aLon;
                if (s.minRangeKm > 1.0)
                    geocalc::destination(aLat, aLon, az, s.minRangeKm, &la0, &lo0);
                if (!geocalc::destination(aLat, aLon, az, s.maxRangeKm, &la1, &lo1))
                    return;
                p.setPen(pen);
                p.setBrush(Qt::NoBrush);
                for (const QPolygonF &line : cam.geodesicLines(la0, lo0, la1, lo1, vp)) {
                    p.drawPolyline(line);
                    ++m_radarPpiPrimitives;
                }
                QPointF tip;
                if (!tag.isEmpty() && cam.visiblePos(lo1, la1, 0.0, vp, &tip))
                    drawOutlinedText(p, tip + QPointF(4.0, -4.0), tag, tickFont, base, Qt::black, 1.8);
            };

            if (!s.showFullRings && azSpan < 359.5) {
                drawRay(az0, edgePen, QStringLiteral("%1°").arg(az0, 0, 'f', 1));
                drawRay(az1, edgePen, QStringLiteral("%1°").arg(az1, 0, 'f', 1));
                const double firstAz = std::ceil((az0 + 1e-3) / azStep) * azStep;
                for (double a = firstAz; a < az0 + azSpan - 1e-3; a += azStep)
                    drawRay(a, ringPen, QStringLiteral("%1°").arg(std::fmod(a + 360.0, 360.0), 0, 'f', 0));
            } else {
                for (double a = 0.0; a < 360.0 - 1e-3; a += qMax(15.0, azStep))
                    drawRay(a, ringPen, QStringLiteral("%1°").arg(a, 0, 'f', 0));
            }

            // 波束中心指向轴
            if (std::isfinite(s.beamDirDeg)) {
                QPen beamPen(QColor(0xf5, 0x9e, 0x0b, 210), 1.5, Qt::DashDotLine);
                drawRay(s.beamDirDeg, beamPen, QStringLiteral("Beam %1°").arg(s.beamDirDeg, 0, 'f', 0));
            }
        }

        // 2) 双基地 Tx-Rx 基线
        if (s.hasTx && s.hasRx) {
            p.setPen(QPen(QColor(0xf5, 0x9e, 0x0b, 175), 1.4, Qt::DashLine));
            for (const QPolygonF &line : cam.geodesicLines(s.txLat, s.txLon, s.rxLat, s.rxLon, vp)) {
                p.drawPolyline(line);
                ++m_radarPpiPrimitives;
            }
        }

        // 3) 接收站 Rx 与发射站 Tx 站点图标
        if (s.hasRx) {
            QPointF sp;
            if (cam.visiblePos(s.rxLon, s.rxLat, 0.0, vp, &sp)) {
                p.setPen(QPen(Qt::black, 3.0));
                p.setBrush(base);
                p.drawEllipse(sp, 6.0, 6.0);
                p.setPen(QPen(Qt::white, 1.5));
                p.drawEllipse(sp, 6.0, 6.0);
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(base, 1.4));
                p.drawEllipse(sp, 11.0, 11.0);
                drawOutlinedText(p, sp + QPointF(13.0, 4.0),
                                 QStringLiteral("%1 (Rx)").arg(s.name), lblFont, base, Qt::black, 2.2);
                ++m_radarPpiPrimitives;
            }
        }
        if (s.hasTx) {
            QPointF sp;
            if (cam.visiblePos(s.txLon, s.txLat, 0.0, vp, &sp)) {
                const QColor txCol(0xf5, 0x9e, 0x0b);
                const QPointF tri[3] = {
                    QPointF(sp.x(), sp.y() - 7.5),
                    QPointF(sp.x() + 6.8, sp.y() + 5.5),
                    QPointF(sp.x() - 6.8, sp.y() + 5.5)
                };
                p.setPen(QPen(Qt::black, 2.6));
                p.setBrush(txCol);
                p.drawPolygon(tri, 3);
                p.setPen(QPen(Qt::white, 1.3));
                p.drawPolygon(tri, 3);
                drawOutlinedText(p, sp + QPointF(11.0, 4.0),
                                 QStringLiteral("%1 (Tx)").arg(s.name), lblFont, txCol, Qt::black, 2.2);
                ++m_radarPpiPrimitives;
            }
        }
    }
    p.restore();
}

void GlobeOverlay::drawDiagnosticsOverlay(QPainter &p)
{
    m_diagnosticsItemsDrawn = 0;
    if (!m_ctx.camera)
        return;
    const bool drawGate = m_ctx.gateAutopsyVisible && m_ctx.gateAutopsy && !m_ctx.gateAutopsy->frames.isEmpty();
    const bool drawFrag = m_ctx.fragmentBridgeVisible && m_ctx.trackStore
                          && m_ctx.fragmentIdxA >= 0 && m_ctx.fragmentIdxB >= 0
                          && m_ctx.fragmentIdxA < m_ctx.trackStore->size()
                          && m_ctx.fragmentIdxB < m_ctx.trackStore->size();
    const bool drawOrphan = m_ctx.orphanClusterVisible && m_ctx.orphanPoints && !m_ctx.orphanPoints->isEmpty();
    if (!drawGate && !drawFrag && !drawOrphan)
        return;

    const QSize vp = size();
    const GlobeCamera &cam = *m_ctx.camera;
    const QFont lblFont = remFont(0.76, true);
    const QFont smallFont = remFont(0.70, false);

    auto diagRangeKm = [](double r) -> double {
        if (!std::isfinite(r))
            return 0.0;
        return r > 20000.0 ? (r / 1000.0) : r;
    };

    bool hasAnchor = false;
    double aLat = 0.0, aLon = 0.0;
    if (m_ctx.radarSites) {
        const RadarSite *site = m_ctx.radarSites->activeSite();
        if (site && site->anchorLatLon(&aLat, &aLon))
            hasAnchor = true;
    }

    p.save();

    // 1) 断航波门尸检：预测轨迹 + 1x/2x 波门边界框 + 候选点标记
    if (drawGate) {
        const trackdiag::GateAutopsyResult &res = *m_ctx.gateAutopsy;
        const QColor colGate1(0xf9, 0x73, 0x16);    // 亮橙：1x 关联波门
        const QColor colGate2(0xfb, 0xbf, 0x24);    // 琥珀：2x 候选搜索框
        const QColor colIn(0x22, 0xc5, 0x5e);       // 绿：门内未抢占/已关联
        const QColor colStolen(0xec, 0x48, 0x99);   // 玫红：门内被其他批号抢占
        const QColor colOutSafe(0xfa, 0xcc, 0x15);  // 黄：门外 2x 候选

        auto drawGateBox = [&](const trackdiag::GateAutopsyFrame &fr, double scale,
                               const QPen &pen, const QColor &fillCol) {
            const double gRKm = qMax(0.5, diagRangeKm(double(fr.gateR)) * scale);
            const double gAz = qMax(0.15, double(fr.gateAz) * scale);
            QPolygonF poly;
            bool allVis = true;
            if (hasAnchor && std::isfinite(fr.predR) && std::isfinite(fr.predAz)) {
                const double rCenter = diagRangeKm(double(fr.predR));
                const double r0 = qMax(0.5, rCenter - gRKm);
                const double r1 = rCenter + gRKm;
                const double az0 = double(fr.predAz) - gAz;
                const double az1 = double(fr.predAz) + gAz;
                const int segs = 8;
                auto pushPolar = [&](double rKm, double azDeg) {
                    double la = 0.0, lo = 0.0;
                    if (!geocalc::destination(aLat, aLon, azDeg, rKm, &la, &lo)) {
                        allVis = false;
                        return;
                    }
                    QPointF sp;
                    if (cam.visiblePos(lo, la, TrackLayer::kAltitudeM, vp, &sp))
                        poly.append(sp);
                    else
                        allVis = false;
                };
                for (int k = 0; k <= segs; ++k)
                    pushPolar(r0, az0 + (az1 - az0) * double(k) / double(segs));
                for (int k = 0; k <= segs; ++k)
                    pushPolar(r1, az1 - (az1 - az0) * double(k) / double(segs));
            } else if (std::isfinite(fr.predLat) && std::isfinite(fr.predLon)) {
                const double dLat = gRKm / 111.0;
                const double cosLat = qMax(0.2, std::cos(qDegreesToRadians(fr.predLat)));
                const double dLon = gRKm / (111.0 * cosLat);
                const double lats[4] = { fr.predLat - dLat, fr.predLat - dLat, fr.predLat + dLat, fr.predLat + dLat };
                const double lons[4] = { fr.predLon - dLon, fr.predLon + dLon, fr.predLon + dLon, fr.predLon - dLon };
                for (int k = 0; k < 4; ++k) {
                    QPointF sp;
                    if (cam.visiblePos(lons[k], lats[k], TrackLayer::kAltitudeM, vp, &sp))
                        poly.append(sp);
                    else
                        allVis = false;
                }
            } else {
                return;
            }
            if (poly.size() >= 3 && allVis) {
                p.setPen(pen);
                p.setBrush(fillCol.isValid() ? QBrush(fillCol) : QBrush(Qt::NoBrush));
                p.drawPolygon(poly);
                ++m_diagnosticsItemsDrawn;
            }
        };

        // 预测外推折线
        QPolygonF predLine;
        for (const trackdiag::GateAutopsyFrame &fr : res.frames) {
            if (!std::isfinite(fr.predLat) || !std::isfinite(fr.predLon))
                continue;
            QPointF sp;
            if (cam.visiblePos(fr.predLon, fr.predLat, TrackLayer::kAltitudeM, vp, &sp))
                predLine.append(sp);
        }
        if (predLine.size() >= 2) {
            QPen pathPen(QColor(0xf9, 0x73, 0x16, 210), 2.0, Qt::DashLine);
            p.setPen(pathPen);
            p.setBrush(Qt::NoBrush);
            p.drawPolyline(predLine);
            ++m_diagnosticsItemsDrawn;
        }

        const int nFrames = res.frames.size();
        for (int fi = 0; fi < nFrames; ++fi) {
            const trackdiag::GateAutopsyFrame &fr = res.frames.at(fi);
            const bool isEdgeFrame = (fi == 0 || fi == nFrames - 1);

            // 2x 虚线外框 + 1x 实线波门框
            QPen pen2(QColor(colGate2.red(), colGate2.green(), colGate2.blue(), 135), 1.2, Qt::DashLine);
            drawGateBox(fr, 2.0, pen2, QColor());
            QPen pen1(QColor(colGate1.red(), colGate1.green(), colGate1.blue(), 225), isEdgeFrame ? 2.0 : 1.5);
            drawGateBox(fr, 1.0, pen1, QColor(colGate1.red(), colGate1.green(), colGate1.blue(), 28));

            // 预测点中心标记
            if (std::isfinite(fr.predLat) && std::isfinite(fr.predLon)) {
                QPointF sp;
                if (cam.visiblePos(fr.predLon, fr.predLat, TrackLayer::kAltitudeM, vp, &sp)) {
                    const double r = 4.2;
                    p.setPen(QPen(Qt::black, 2.6));
                    p.drawLine(QPointF(sp.x() - r, sp.y() - r), QPointF(sp.x() + r, sp.y() + r));
                    p.drawLine(QPointF(sp.x() - r, sp.y() + r), QPointF(sp.x() + r, sp.y() - r));
                    p.setPen(QPen(colGate1, 1.6));
                    p.drawLine(QPointF(sp.x() - r, sp.y() - r), QPointF(sp.x() + r, sp.y() + r));
                    p.drawLine(QPointF(sp.x() - r, sp.y() + r), QPointF(sp.x() + r, sp.y() - r));
                    ++m_diagnosticsItemsDrawn;
                    if (isEdgeFrame) {
                        drawOutlinedText(p, sp + QPointF(8.0, -5.0),
                                         QStringLiteral("波门 F%1").arg(fr.frameId),
                                         smallFont, colGate2, Qt::black, 2.0);
                    }
                }
            }

            // 2x 波门候选检测点高亮标记
            for (int ci = 0; ci < fr.candidates.size(); ++ci) {
                const trackdiag::GateCandidate &c = fr.candidates.at(ci);
                double cLat = fr.predLat;
                double cLon = fr.predLon;
                bool posOk = false;
                if (hasAnchor && std::isfinite(c.range) && std::isfinite(c.az)) {
                    posOk = geocalc::destination(aLat, aLon, double(c.az), diagRangeKm(double(c.range)), &cLat, &cLon);
                } else if (std::isfinite(fr.predLat) && std::isfinite(fr.predLon)) {
                    const double drKm = diagRangeKm(double(c.dR));
                    const double cosLat = qMax(0.2, std::cos(qDegreesToRadians(fr.predLat)));
                    cLat = fr.predLat + drKm / 111.0;
                    cLon = fr.predLon + (std::isfinite(c.dAz) ? (c.dAz * 0.2) : 0.0) / cosLat;
                    posOk = true;
                }
                if (!posOk)
                    continue;
                QPointF sp;
                if (!cam.visiblePos(cLon, cLat, TrackLayer::kAltitudeM, vp, &sp))
                    continue;
                const QColor cCol = c.insideGate ? (c.ownerBatchNo > 0 ? colStolen : colIn) : colOutSafe;
                p.setPen(QPen(Qt::black, 1.5));
                p.setBrush(cCol);
                if (c.insideGate) {
                    const double r = 5.0;
                    const QPointF dia[4] = {
                        QPointF(sp.x(), sp.y() - r), QPointF(sp.x() + r, sp.y()),
                        QPointF(sp.x(), sp.y() + r), QPointF(sp.x() - r, sp.y())
                    };
                    p.drawPolygon(dia, 4);
                } else {
                    p.drawEllipse(sp, 3.8, 3.8);
                }
                ++m_diagnosticsItemsDrawn;
                if (ci < 3 && isEdgeFrame) {
                    const QString tag = c.ownerBatchNo > 0
                        ? QStringLiteral("P%1→T%2").arg(c.pointId).arg(c.ownerBatchNo)
                        : QStringLiteral("P%1 %2dB").arg(c.pointId).arg(double(c.snr), 0, 'f', 0);
                    drawOutlinedText(p, sp + QPointF(7.0, 4.0), tag, smallFont, cCol, Qt::black, 1.8);
                }
            }
        }
    }

    // 2) 断批重起桥接：A 末点 -> B 起点 高亮虚线箭头与间隔标注
    if (drawFrag) {
        const Track &trA = m_ctx.trackStore->tracks().at(m_ctx.fragmentIdxA);
        const Track &trB = m_ctx.trackStore->tracks().at(m_ctx.fragmentIdxB);
        const RadarPointMode mode = m_ctx.trackLayer ? m_ctx.trackLayer->radarPointMode() : RadarPointMode::Smooth;
        const QVector<TrackPoint> &ptsA = trA.pointsForMode(mode);
        const QVector<TrackPoint> &ptsB = trB.pointsForMode(mode);
        if (!ptsA.isEmpty() && !ptsB.isEmpty()) {
            const TrackPoint &endA = ptsA.last();
            const TrackPoint &startB = ptsB.first();
            const QColor colBridge(0xf9, 0x73, 0x16);
            QPen bridgePen(colBridge, 2.6, Qt::CustomDashLine, Qt::RoundCap, Qt::RoundJoin);
            bridgePen.setDashPattern(QVector<qreal>() << 7.0 << 5.0);
            p.setPen(bridgePen);
            p.setBrush(Qt::NoBrush);
            const QVector<QPolygonF> lines = cam.geodesicLines(endA.lat, endA.lon, startB.lat, startB.lon, vp);
            for (const QPolygonF &line : lines) {
                p.drawPolyline(line);
                ++m_diagnosticsItemsDrawn;
            }
            QPointF spA, spB;
            const bool visA = cam.visiblePos(endA.lon, endA.lat, TrackLayer::kAltitudeM, vp, &spA);
            const bool visB = cam.visiblePos(startB.lon, startB.lat, TrackLayer::kAltitudeM, vp, &spB);
            if (visA) {
                p.setPen(QPen(Qt::black, 1.8));
                p.setBrush(QColor(0xef, 0x44, 0x44));
                p.drawEllipse(spA, 5.5, 5.5);
                ++m_diagnosticsItemsDrawn;
            }
            if (visB) {
                // 箭头指向 B 起点
                QPointF prevPt = spA;
                if (!lines.isEmpty() && lines.last().size() >= 2)
                    prevPt = lines.last().at(lines.last().size() - 2);
                const QPointF dir = spB - prevPt;
                const double len = std::hypot(dir.x(), dir.y());
                if (len > 2.0) {
                    const QPointF u(dir.x() / len, dir.y() / len);
                    const QPointF n(-u.y(), u.x());
                    const double arrowLen = 12.0;
                    const double arrowHalfW = 5.5;
                    const QPointF tri[3] = {
                        spB,
                        spB - u * arrowLen + n * arrowHalfW,
                        spB - u * arrowLen - n * arrowHalfW
                    };
                    p.setPen(QPen(Qt::black, 1.4));
                    p.setBrush(colBridge);
                    p.drawPolygon(tri, 3);
                } else {
                    p.setPen(QPen(Qt::black, 1.8));
                    p.setBrush(QColor(0x22, 0xc5, 0x5e));
                    p.drawEllipse(spB, 5.5, 5.5);
                }
                ++m_diagnosticsItemsDrawn;
            }
            if (visA && visB) {
                const double gapSec = double(startB.t - endA.t) / 1000.0;
                const double distKm = geocalc::vincentyKm(endA.lat, endA.lon, startB.lat, startB.lon);
                const QPointF mid = 0.5 * (spA + spB);
                const QString tag = QStringLiteral("断批桥接 %1→%2  Δt=%3s · %4km")
                                        .arg(trA.id, trB.id)
                                        .arg(gapSec, 0, 'f', 1)
                                        .arg(distKm, 0, 'f', 1);
                drawOutlinedText(p, mid + QPointF(8.0, -8.0), tag, lblFont, QColor(0xfd, 0xba, 0x74), Qt::black, 2.4);
            }
        }
    }

    // 3) 未关联漏网点迹链：经激活雷达站极坐标正算投影到地图
    if (drawOrphan && hasAnchor) {
        const QColor colChain(0xe8, 0x79, 0xf9);   // 亮品红
        const QColor colDot(0x22, 0xd3, 0xee);     // 青色高亮点
        QPolygonF chainPoly;
        QVector<QPointF> dotPts;
        dotPts.reserve(m_ctx.orphanPoints->size());
        for (const DiagOrphanPoint &op : *m_ctx.orphanPoints) {
            if (!std::isfinite(op.rangeKm) || !std::isfinite(op.azDeg) || op.rangeKm <= 0.0)
                continue;
            double la = 0.0, lo = 0.0;
            if (!geocalc::destination(aLat, aLon, op.azDeg, op.rangeKm, &la, &lo))
                continue;
            QPointF sp;
            if (cam.visiblePos(lo, la, TrackLayer::kAltitudeM, vp, &sp)) {
                chainPoly.append(sp);
                dotPts.append(sp);
            }
        }
        if (chainPoly.size() >= 2) {
            QPen chainPen(colChain, 2.2, Qt::DashLine);
            p.setPen(chainPen);
            p.setBrush(Qt::NoBrush);
            p.drawPolyline(chainPoly);
            ++m_diagnosticsItemsDrawn;
        }
        p.setPen(QPen(Qt::black, 1.5));
        p.setBrush(colDot);
        for (const QPointF &sp : qAsConst(dotPts)) {
            p.drawEllipse(sp, 4.5, 4.5);
            ++m_diagnosticsItemsDrawn;
        }
        if (!dotPts.isEmpty()) {
            const QString tag = QStringLiteral("漏批链 #%1（%2点 · SNR %3dB）")
                                    .arg(m_ctx.orphanClusterId)
                                    .arg(m_ctx.orphanPointCount)
                                    .arg(double(m_ctx.orphanMeanSnr), 0, 'f', 1);
            drawOutlinedText(p, dotPts.first() + QPointF(9.0, -7.0), tag, lblFont, colDot, Qt::black, 2.3);
        }
    }

    p.restore();
}
