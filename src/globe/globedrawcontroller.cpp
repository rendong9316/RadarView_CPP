#include "globedrawcontroller.h"
#include "globecamera.h"

#include <QLineF>
#include <QMouseEvent>
#include <QtMath>
#include <cmath>

namespace {

const double kPickTolPx = 6.0;
const int kClickSlopPx = 4;
const double kSiteDotPx = 6.0;      // 站点圆点半径：与 TrackLayer 端点圆球基准 6px 一致
const double kSnapPx = 10.0;        // 吸附半径（屏幕像素）

// 点到线段的屏幕距离（橡皮擦命中测试）
double distToSegment(const QPointF &p, const QPointF &a, const QPointF &b)
{
    const QPointF ab = b - a;
    const double len2 = ab.x() * ab.x() + ab.y() * ab.y();
    if (len2 < 1e-12)
        return QLineF(p, a).length();
    double t = ((p.x() - a.x()) * ab.x() + (p.y() - a.y()) * ab.y()) / len2;
    t = qBound(0.0, t, 1.0);
    return QLineF(p, QPointF(a.x() + ab.x() * t, a.y() + ab.y() * t)).length();
}

// 屏幕线段与矩形是否相交（框选删除用；任一顶点在框内也算）
bool segmentHitsRect(const QPointF &a, const QPointF &b, const QRectF &r)
{
    if (r.contains(a) || r.contains(b))
        return true;
    const QPointF corner[4] = { r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft() };
    auto cross = [](const QPointF &o, const QPointF &p, const QPointF &q) {
        return (p.x() - o.x()) * (q.y() - o.y()) - (p.y() - o.y()) * (q.x() - o.x());
    };
    for (int i = 0; i < 4; ++i) {
        const QPointF &c = corner[i], &d = corner[(i + 1) % 4];
        const double d1 = cross(c, d, a), d2 = cross(c, d, b);
        const double d3 = cross(a, b, c), d4 = cross(a, b, d);
        if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0))
            && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0)))
            return true;
    }
    return false;
}

} // namespace

GlobeDrawController::GlobeDrawController(const GlobeCamera *camera, QObject *parent)
    : QObject(parent),
      m_camera(camera)
{
}

void GlobeDrawController::setDrawingManager(DrawingManager *drawing)
{
    if (m_draw)
        disconnect(m_draw, nullptr, this, nullptr);
    m_draw = drawing;
    if (m_draw) {
        // 撤销/删除后选中的要素可能已经不存在了，顺手把选中状态清掉
        connect(m_draw, &DrawingManager::changed, this, [this]() {
            if (m_selectedFeature > 0 && m_draw->indexOf(m_selectedFeature) < 0)
                setSelectedFeature(0);
            if (m_host.requestUpdate)
                m_host.requestUpdate();
        });
    }
    if (m_host.requestUpdate)
        m_host.requestUpdate();
}

void GlobeDrawController::setTool(DrawTool t)
{
    if (m_tool == t)
        return;
    cancel();
    m_tool = t;
    m_hoverFeature = 0;
    if (t == DrawTool::None) {
        if (m_host.unsetCursorShape)
            m_host.unsetCursorShape();
    } else if (t == DrawTool::Select) {
        if (m_host.setCursorShape)
            m_host.setCursorShape(Qt::ArrowCursor);        // 选择工具用箭头，跟「画」区分开
    } else {
        if (m_host.setCursorShape)
            m_host.setCursorShape(Qt::CrossCursor);
    }
    if (m_host.requestUpdate)
        m_host.requestUpdate();
}

bool GlobeDrawController::cancel()
{
    const bool had = m_strokeActive || !m_stroke.isEmpty() || m_bandActive || m_hoverFeature != 0
                     || m_snapActive;
    m_stroke.clear();
    m_strokeActive = false;
    m_bandActive = false;
    m_bandRect = QRect();
    m_hoverFeature = 0;
    m_snapActive = false;
    m_dragging = false;
    m_clickCandidate = false;
    if (had && m_host.requestUpdate)
        m_host.requestUpdate();
    return had;
}

void GlobeDrawController::setSelectedFeature(int id)
{
    if (m_selectedFeature == id)
        return;
    m_selectedFeature = id;
    emit drawSelectionChanged(id);
    if (m_host.requestUpdate)
        m_host.requestUpdate();
}

bool GlobeDrawController::deleteSelected()
{
    if (!m_draw || m_selectedFeature <= 0)
        return false;
    const int id = m_selectedFeature;
    setSelectedFeature(0);
    return m_draw->removeFeature(id);
}

bool GlobeDrawController::hasOverlayContent() const
{
    return (m_tool != DrawTool::None && (!m_stroke.isEmpty() || m_bandActive)) || m_snapActive;
}

// 只有需要「落点精确」的工具才吸附：直线 / 矩形 / 多边形。
// 自由手绘是逐点采样，吸了反而会把线扯到别的图形上。
bool GlobeDrawController::snappingTool() const
{
    return m_tool == DrawTool::Line || m_tool == DrawTool::Rect || m_tool == DrawTool::Polygon;
}

// 吸附：找屏幕上离鼠标最近、且在 kSnapPx（10px）以内的已有顶点。
// 命中时返回该顶点的 WGS84 经纬度与屏幕坐标，调用方用它替换当前落点。
bool GlobeDrawController::snapToFeature(const QPointF &pos, double *lonDeg, double *latDeg,
                                        QPointF *screen) const
{
    if (!m_draw || m_draw->features().isEmpty() || !m_camera || !m_host.viewport)
        return false;
    const QSize vp = m_host.viewport();
    double best = kSnapPx;
    bool found = false;
    QPointF bestPos;
    for (const DrawingFeature &f : m_draw->features()) {
        for (const QPointF &pt : f.pts) {
            QPointF sp;
            if (!m_camera->visiblePos(pt.x(), pt.y(), 0.0, vp, &sp))
                continue;
            const double d = std::hypot(sp.x() - pos.x(), sp.y() - pos.y());
            if (d <= best) {
                best = d;
                bestPos = sp;
                *lonDeg = pt.x();
                *latDeg = pt.y();
                found = true;
            }
        }
    }
    if (found && screen)
        *screen = bestPos;
    return found;
}

void GlobeDrawController::finishDrawStroke(double lat, double lon)
{
    if (!m_draw)
        return;
    int id = 0;
    if (m_tool == DrawTool::Line)
        id = m_draw->addLine(m_pressLat, m_pressLon, lat, lon, m_drawColor);
    else if (m_tool == DrawTool::Rect)
        id = m_draw->addRect(m_pressLat, m_pressLon, lat, lon, m_drawColor);
    if (id > 0)
        emit drawFeatureAdded(id);
}

void GlobeDrawController::closeDrawPolygon()
{
    if (m_draw && m_stroke.size() >= 3) {
        const int id = m_draw->addPolygon(m_stroke, m_drawColor);
        if (id > 0)
            emit drawFeatureAdded(id);
    }
    m_stroke.clear();
    m_strokeActive = false;
    if (m_host.requestUpdate)
        m_host.requestUpdate();
}

void GlobeDrawController::applyBoxSelect()
{
    if (!m_draw)
        return;
    const QRectF r = QRectF(m_bandRect).normalized();
    if (r.width() < 3.0 && r.height() < 3.0)
        return;                       // 误点：没拉出框就不删
    QVector<int> ids;
    for (const DrawingFeature &f : m_draw->features())
        if (featureHitsRect(f, r))
            ids.append(f.id);
    m_draw->removeFeatures(ids);
}

// 闭合图形整圈都可见时返回投影后的多边形；有任何一点在背面就返回空并置 complete=false
QPolygonF GlobeDrawController::projectedRing(const GlobeCamera &cam, const QSize &viewport,
                                             const DrawingFeature &f, bool *complete)
{
    QPolygonF ring;
    ring.reserve(f.pts.size());
    for (const QPointF &pt : f.pts) {
        QPointF sp;
        if (!cam.visiblePos(pt.x(), pt.y(), 0.0, viewport, &sp)) {
            if (complete)
                *complete = false;
            return QPolygonF();
        }
        ring << sp;
    }
    if (complete)
        *complete = true;
    return ring;
}

// 橡皮擦：屏幕空间点到折线的距离，取最近的一个（后画的优先）
int GlobeDrawController::featureAtScreen(const QPointF &pos, double tolPx) const
{
    if (!m_draw || !m_camera || !m_host.viewport)
        return 0;
    const QSize vp = m_host.viewport();
    const QVector<DrawingFeature> &fs = m_draw->features();
    int best = 0;
    double bestD = tolPx;
    for (int i = fs.size() - 1; i >= 0; --i) {
        const DrawingFeature &f = fs.at(i);
        if (f.isText()) {
            QPointF sp;
            if (!f.pts.isEmpty() && m_camera->visiblePos(f.pts.first().x(), f.pts.first().y(), 0.0, vp, &sp)) {
                const double d = QLineF(sp, pos).length();
                if (d <= 14.0 && d <= bestD + 8.0) {   // 文字锚点允许宽松一点
                    best = f.id;
                    bestD = qMin(bestD, d);
                }
            }
            continue;
        }
        if (f.kind == DrawKind::Dot) {
            QPointF sp;
            if (!f.pts.isEmpty() && m_camera->visiblePos(f.pts.first().x(), f.pts.first().y(), 0.0, vp, &sp)) {
                const double d = QLineF(sp, pos).length();
                if (d <= kSiteDotPx + 3.0 && d <= bestD + 8.0) {
                    best = f.id;
                    bestD = qMin(bestD, d);
                }
            }
            continue;
        }
        QPointF prev;
        bool have = false;
        for (const QPointF &pt : f.pts) {
            QPointF sp;
            if (!m_camera->visiblePos(pt.x(), pt.y(), 0.0, vp, &sp)) {
                have = false;
                continue;
            }
            if (have) {
                const double d = distToSegment(pos, prev, sp);
                if (d < bestD) {
                    bestD = d;
                    best = f.id;
                }
            }
            prev = sp;
            have = true;
        }
    }
    return best;
}

// 油漆桶：点是否落在某个闭合图形里（屏幕空间射线法，整圈可见才判）
int GlobeDrawController::closedFeatureAt(const QPointF &pos) const
{
    if (!m_draw || !m_camera || !m_host.viewport)
        return 0;
    const QSize vp = m_host.viewport();
    const QVector<DrawingFeature> &fs = m_draw->features();
    for (int i = fs.size() - 1; i >= 0; --i) {
        const DrawingFeature &f = fs.at(i);
        if (!f.isClosed())
            continue;
        bool complete = false;
        const QPolygonF ring = projectedRing(*m_camera, vp, f, &complete);
        if (complete && ring.size() >= 3 && ring.containsPoint(pos, Qt::OddEvenFill))
            return f.id;
    }
    return 0;
}

// 框选（交叉选择）：任一顶点在框内，或任一线段与框边相交
bool GlobeDrawController::featureHitsRect(const DrawingFeature &f, const QRectF &rect) const
{
    if (f.pts.isEmpty() || !m_camera || !m_host.viewport)
        return false;
    const QSize vp = m_host.viewport();
    if (f.isText()) {
        QPointF sp;
        return m_camera->visiblePos(f.pts.first().x(), f.pts.first().y(), 0.0, vp, &sp) && rect.contains(sp);
    }
    QPointF prev;
    bool have = false;
    for (const QPointF &pt : f.pts) {
        QPointF sp;
        if (!m_camera->visiblePos(pt.x(), pt.y(), 0.0, vp, &sp)) {
            have = false;
            continue;
        }
        if (rect.contains(sp))
            return true;
        if (have && segmentHitsRect(prev, sp, rect))
            return true;
        prev = sp;
        have = true;
    }
    return false;
}

void GlobeDrawController::mousePress(QMouseEvent *e)
{
    // 绘图模式：左键归画笔，右键拖拽旋转（不弹右键菜单）；滚轮缩放照常
    if (e->button() == Qt::RightButton) {
        m_dragging = true;
        m_clickCandidate = true;
        m_lastPos = m_pressPos = e->pos();
        return;
    }
    if (e->button() != Qt::LeftButton || !m_camera || !m_host.viewport)
        return;
    const QSize vp = m_host.viewport();
    double lon = 0.0, lat = 0.0;
    const bool hit = m_camera->geoAt(e->pos(), vp, &lon, &lat);
    // 吸附：落点离已有顶点不到 10px 就吸过去（直线/矩形/多边形）
    m_snapActive = false;
    if (hit && snappingTool()) {
        QPointF sp;
        double sl = 0.0, sa = 0.0;
        if (snapToFeature(e->pos(), &sl, &sa, &sp)) {
            lon = sl;
            lat = sa;
            m_snapActive = true;
            m_snapPos = sp;
        }
    }
    switch (m_tool) {
    case DrawTool::Text:
        if (hit)
            emit drawTextRequested(lat, lon, e->globalPos());
        return;
    case DrawTool::Fill:
        emit drawFillRequested(hit ? closedFeatureAt(e->pos()) : 0);
        return;
    case DrawTool::Eraser: {
        const int id = hit ? featureAtScreen(e->pos(), kPickTolPx) : 0;
        if (id > 0 && m_draw)
            m_draw->removeFeature(id);
        return;
    }
    case DrawTool::BoxSelect:
        m_bandActive = true;
        m_bandRect = QRect(e->pos(), QSize(0, 0));
        m_pressPos = e->pos();
        return;
    case DrawTool::Polygon:
        if (!hit)
            return;
        if (m_stroke.isEmpty())
            m_strokeActive = true;
        // 吸到起点就自动闭合：闭合边严丝合缝，不会差出一点点
        if (m_stroke.size() >= 3 && QPointF(lon, lat) == m_stroke.first()) {
            closeDrawPolygon();
            return;
        }
        m_stroke.append(QPointF(lon, lat));      // 多边形：单击放顶点
        if (m_host.requestUpdate)
            m_host.requestUpdate();
        return;
    case DrawTool::Select: {
        int id = hit ? featureAtScreen(e->pos(), kPickTolPx) : 0;
        if (id <= 0 && hit)
            id = closedFeatureAt(e->pos());      // 点在填充/多边形内部也能选中
        setSelectedFeature(id);
        return;
    }
    case DrawTool::Break:
        if (!hit)
            return;
        m_strokeActive = true;                   // 按下开始「下刀」，松开时判断是点还是拖
        m_pressLat = lat;
        m_pressLon = lon;
        m_pressPos = e->pos();
        if (m_host.requestUpdate)
            m_host.requestUpdate();
        return;
    case DrawTool::Line:
    case DrawTool::Rect:
    case DrawTool::Polyline:
    default:
        if (!hit)
            return;
        m_strokeActive = true;
        m_stroke.clear();
        m_stroke.append(QPointF(lon, lat));
        m_pressLat = lat;
        m_pressLon = lon;
        m_pressPos = e->pos();
        if (m_host.requestUpdate)
            m_host.requestUpdate();
        return;
    }
}

void GlobeDrawController::mouseMove(QMouseEvent *e)
{
    if (!m_camera || !m_host.viewport)
        return;
    const QSize vp = m_host.viewport();
    double lon = 0.0, lat = 0.0;
    const bool hit = m_camera->geoAt(e->pos(), vp, &lon, &lat);
    // 吸附：预览点直接吸到已有顶点上，落笔位置和看到的一致
    m_snapActive = false;
    if (hit && snappingTool()) {
        QPointF sp;
        double sl = 0.0, sa = 0.0;
        if (snapToFeature(e->pos(), &sl, &sa, &sp)) {
            lon = sl;
            lat = sa;
            m_snapActive = true;
            m_snapPos = sp;
        }
    }
    if (m_host.setCursorGeo)
        m_host.setCursorGeo(hit, qDegreesToRadians(lon), qDegreesToRadians(lat));
    if (m_dragging) {                       // 右键拖拽旋转
        if (m_clickCandidate && (e->pos() - m_pressPos).manhattanLength() > kClickSlopPx)
            m_clickCandidate = false;
        if (!m_clickCandidate) {
            if (m_host.panPixels)
                m_host.panPixels(m_lastPos, e->pos());
            m_lastPos = e->pos();
        }
        m_camera->updateMatrices(vp);
        if (m_host.requestStatus)
            m_host.requestStatus();
        return;
    }
    if (m_bandActive) {
        m_bandRect = QRect(m_pressPos, e->pos()).normalized();
        if (m_host.requestUpdate)
            m_host.requestUpdate();
        if (m_host.requestStatus)
            m_host.requestStatus();
        return;
    }
    if (m_strokeActive && m_tool == DrawTool::Polyline && hit) {
        // 与上一点隔 ~200 m 才采点：手一停不会堆出上千个重复点
        const QPointF last = m_stroke.last();
        if (std::hypot(lon - last.x(), lat - last.y()) > 0.002)
            m_stroke.append(QPointF(lon, lat));
        if (m_host.requestUpdate)
            m_host.requestUpdate();
    } else if (m_strokeActive || (m_tool == DrawTool::Polygon && !m_stroke.isEmpty())) {
        if (m_host.requestUpdate)
            m_host.requestUpdate();         // 直线/矩形/多边形：重画预览
    }
    if (m_tool == DrawTool::Eraser) {
        const int id = hit ? featureAtScreen(e->pos(), kPickTolPx) : 0;
        if (id != m_hoverFeature) {
            m_hoverFeature = id;
            if (m_host.requestUpdate)
                m_host.requestUpdate();
        }
    }
    if (m_host.requestStatus)
        m_host.requestStatus();
}

void GlobeDrawController::mouseRelease(QMouseEvent *e)
{
    // 绘图模式：右键单击 = 闭合多边形 / 取消当前笔画；左键松开 = 落笔
    if (e->button() == Qt::RightButton) {
        const bool click = m_clickCandidate;
        m_dragging = false;
        m_clickCandidate = false;
        if (click) {
            if (m_tool == DrawTool::Polygon && m_stroke.size() >= 3)
                closeDrawPolygon();
            else
                cancel();
        }
        return;
    }
    if (e->button() != Qt::LeftButton || !m_camera || !m_host.viewport)
        return;
    const QSize vp = m_host.viewport();
    if (m_bandActive) {
        applyBoxSelect();
        m_bandActive = false;
        m_bandRect = QRect();
        if (m_host.requestUpdate)
            m_host.requestUpdate();
        return;
    }
    // 裁剪/打断：拖得短算「点一下」（打断），拖得长算「划一刀」（裁剪）
    if (m_tool == DrawTool::Break) {
        const bool click = (e->pos() - m_pressPos).manhattanLength() < 5;
        double lon = 0.0, lat = 0.0;
        const bool hit = m_camera->geoAt(e->pos(), vp, &lon, &lat);
        m_strokeActive = false;
        m_snapActive = false;
        if (m_draw && hit) {
            if (click) {
                const int id = featureAtScreen(e->pos(), kPickTolPx);
                if (id <= 0)
                    emit drawMessage(tr("这里没有可打断的线段"));
                else if (!m_draw->breakFeature(id, m_draw->nearestVertex(id, lat, lon)))
                    emit drawMessage(tr("这里断不开（文本不能打断，端点处也断不出两段）"));
                else
                    emit drawMessage(tr("已在该点打断成两段"));
            } else {
                const int n = m_draw->trimCrossing(m_pressLat, m_pressLon, lat, lon);
                emit drawMessage(n > 0 ? tr("已裁剪 %1 个图形").arg(n)
                                       : tr("剪切线没有穿过任何开放线段"));
            }
        }
        if (m_host.requestUpdate)
            m_host.requestUpdate();
        return;
    }
    if (!m_strokeActive)
        return;
    if (m_tool == DrawTool::Polygon)
        return;                              // 多边形等双击/右键闭合
    double lon = 0.0, lat = 0.0;
    const bool hit = m_camera->geoAt(e->pos(), vp, &lon, &lat);
    if (hit && snappingTool()) {             // 落笔点也吸附，终点才能精确落在已有顶点上
        double sl = 0.0, sa = 0.0;
        if (snapToFeature(e->pos(), &sl, &sa, nullptr)) {
            lon = sl;
            lat = sa;
        }
    }
    if (m_tool == DrawTool::Polyline) {
        if (m_stroke.size() >= 2 && m_draw) {
            const int id = m_draw->addPolyline(m_stroke, m_drawColor);
            if (id > 0)
                emit drawFeatureAdded(id);
        }
    } else if (hit && (e->pos() - m_pressPos).manhattanLength() >= 3) {
        finishDrawStroke(lat, lon);           // 直线/矩形：太短的拖动当成误点
    }
    m_stroke.clear();
    m_strokeActive = false;
    if (m_host.requestUpdate)
        m_host.requestUpdate();
}

void GlobeDrawController::mouseDoubleClick(QMouseEvent *e)
{
    // 绘图模式：多边形双击闭合；其他工具下双击也不能误放旗标
    if (m_tool == DrawTool::Polygon && e->button() == Qt::LeftButton) {
        closeDrawPolygon();
    } else if (m_tool == DrawTool::Select && e->button() == Qt::LeftButton) {
        // 双击 = 打开属性窗（颜色 / 线宽 / 填充透明度 / 圆的半径）
        int id = featureAtScreen(e->pos(), kPickTolPx);
        if (id <= 0)
            id = closedFeatureAt(e->pos());
        if (id > 0) {
            setSelectedFeature(id);
            emit drawFeatureEditRequested(id);
        }
    }
}
