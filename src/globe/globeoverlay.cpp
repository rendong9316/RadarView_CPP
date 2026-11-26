#include "globeoverlay.h"
#include "drawingmanager.h"
#include "globecamera.h"
#include "globedrawcontroller.h"
#include "maptools.h"
#include "spatiallasso.h"
#include "theme.h"
#include "track.h"
#include "tracklayer.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>
#include <cmath>

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
    if (!m_wanted || !m_ctx.camera)
        return;
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    drawLabels(p);
    drawFeatures(p);
    drawInProgress(p);
    drawBand(p);
    drawRuler(p);
    drawLasso(p);
    drawFlags(p);
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
