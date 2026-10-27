#include "drawingmanager.h"
#include "drawcommands.h"
#include "geocalc.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QtMath>
#include <QUndoStack>
#include <cmath>

namespace {

// 坐标按 1e-6 度（约 0.1 m）落盘：足够贴合，文件也不会被浮点尾巴撑大
double round6(double v) { return qRound64(v * 1e6) / 1e6; }

QJsonArray pointJson(const QPointF &p)
{
    QJsonArray a;
    a.append(round6(p.x()));      // [经度, 纬度]：GeoJSON 的坐标顺序
    a.append(round6(p.y()));
    return a;
}

bool pointFromJson(const QJsonValue &v, QPointF *out)
{
    const QJsonArray a = v.toArray();
    if (a.size() < 2)
        return false;
    const double lon = a.at(0).toDouble(), lat = a.at(1).toDouble();
    if (!std::isfinite(lon) || !std::isfinite(lat))
        return false;
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0)
        return false;
    *out = QPointF(lon, lat);
    return true;
}

// 稀疏顶点 -> 贴地闭合环：每条边按大圆加密，末尾重复首点
QVector<QPointF> densifyRing(const QVector<QPointF> &verts)
{
    QVector<QPointF> out;
    if (verts.size() < 2)
        return out;
    for (int i = 0; i < verts.size(); ++i) {
        const QPointF &a = verts.at(i);
        const QPointF &b = verts.at((i + 1) % verts.size());
        const QVector<QPointF> seg = geocalc::geodesicPath(a.y(), a.x(), b.y(), b.x());
        for (int k = 0; k + 1 < seg.size(); ++k)      // 每段含终点，下一条边会补上
            out.append(seg.at(k));
    }
    if (!out.isEmpty())
        out.append(out.first());
    return out;
}

} // namespace

QString drawKindName(DrawKind k)
{
    switch (k) {
    case DrawKind::Circle:   return QStringLiteral("circle");
    case DrawKind::Line:     return QStringLiteral("line");
    case DrawKind::Polygon:  return QStringLiteral("polygon");
    case DrawKind::Rect:     return QStringLiteral("rect");
    case DrawKind::Fill:     return QStringLiteral("fill");
    case DrawKind::Text:     return QStringLiteral("text");
    case DrawKind::Polyline: break;
    }
    return QStringLiteral("polyline");
}

bool drawKindFromName(const QString &name, DrawKind *out)
{
    if (name == QLatin1String("circle"))   { *out = DrawKind::Circle;   return true; }
    if (name == QLatin1String("line"))     { *out = DrawKind::Line;     return true; }
    if (name == QLatin1String("polyline")) { *out = DrawKind::Polyline; return true; }
    if (name == QLatin1String("polygon"))  { *out = DrawKind::Polygon;  return true; }
    if (name == QLatin1String("rect"))     { *out = DrawKind::Rect;     return true; }
    if (name == QLatin1String("fill"))     { *out = DrawKind::Fill;     return true; }
    if (name == QLatin1String("text"))     { *out = DrawKind::Text;     return true; }
    return false;
}

DrawingManager::DrawingManager(QObject *parent) : QObject(parent) {}

const DrawingFeature *DrawingManager::feature(int id) const
{
    for (const DrawingFeature &f : m_features)
        if (f.id == id)
            return &f;
    return nullptr;
}

int DrawingManager::indexOf(int id) const
{
    for (int i = 0; i < m_features.size(); ++i)
        if (m_features[i].id == id)
            return i;
    return -1;
}

bool DrawingManager::pushOrApply(QUndoCommand *cmd)
{
    if (!cmd)
        return false;
    if (m_undo)
        m_undo->push(cmd);          // push() 内部会调用 redo()，由命令自己完成改动
    else {
        cmd->redo();                // 没有撤销栈（自检里的纯数据用法）：直接生效
        delete cmd;
    }
    return true;
}

int DrawingManager::addFeature(const DrawingFeature &f)
{
    if (f.pts.isEmpty())
        return 0;
    if (f.pts.size() < 2 && f.kind != DrawKind::Text)
        return 0;
    DrawingFeature item = f;
    item.id = m_nextId++;           // id 在造命令时就定下来，重做后才能还原同一个 id
    pushOrApply(new AddFeatureCommand(this, item));
    return item.id;
}

// 撤销栈专用：插入时保留 id（重做要恢复原样），并把 nextId 推到最大值之后
void DrawingManager::insertFeature(int index, const DrawingFeature &f)
{
    m_features.insert(qBound(0, index, m_features.size()), f);
    if (f.id >= m_nextId)
        m_nextId = f.id + 1;
    ++m_version;
    emit changed();
}

bool DrawingManager::takeFeature(int id)
{
    const int i = indexOf(id);
    if (i < 0)
        return false;
    m_features.remove(i);
    ++m_version;
    emit changed();
    return true;
}

bool DrawingManager::replaceFeature(const DrawingFeature &f)
{
    const int i = indexOf(f.id);
    if (i < 0)
        return false;
    m_features[i] = f;
    ++m_version;
    emit changed();
    return true;
}

// 旗标距离圆：以旗标为圆心、radiusKm 为测地半径生成一圈点。
// 180 个方位角采样点，相邻点约 2°，投影到屏幕上已经看不出折线感。
int DrawingManager::addCircle(double lat, double lon, double radiusKm, const QColor &color,
                              const QString &label)
{
    const QVector<QPointF> ring = geocalc::geodesicCircle(lat, lon, radiusKm, 180);
    if (ring.size() < 3)
        return 0;
    DrawingFeature f;
    f.kind = DrawKind::Circle;
    f.color = color.isValid() ? color : QColor(0xf5, 0x9e, 0x0b);
    f.widthPx = 3.0;
    f.pts = ring;
    f.pts.append(ring.first());          // 闭合：渲染时一条折线画完
    f.centerLat = lat;
    f.centerLon = lon;
    f.radiusKm = radiusKm;
    f.label = label;
    return addFeature(f);
}

int DrawingManager::addLine(double lat1, double lon1, double lat2, double lon2, const QColor &color,
                            double widthPx)
{
    const QVector<QPointF> path = geocalc::geodesicPath(lat1, lon1, lat2, lon2);
    if (path.size() < 2)
        return 0;
    DrawingFeature f;
    f.kind = DrawKind::Line;
    f.color = color.isValid() ? color : QColor(0xf5, 0x9e, 0x0b);
    f.widthPx = qBound(1.0, widthPx, 20.0);
    f.pts = path;
    return addFeature(f);
}

int DrawingManager::addPolyline(const QVector<QPointF> &wgsPts, const QColor &color, double widthPx)
{
    if (wgsPts.size() < 2)
        return 0;
    DrawingFeature f;
    f.kind = DrawKind::Polyline;
    f.color = color.isValid() ? color : QColor(0xf5, 0x9e, 0x0b);
    f.widthPx = qBound(1.0, widthPx, 20.0);
    f.pts = wgsPts;
    return addFeature(f);
}

int DrawingManager::addPolygon(const QVector<QPointF> &verts, const QColor &color, double widthPx)
{
    if (verts.size() < 3)
        return 0;
    const QVector<QPointF> ring = densifyRing(verts);
    if (ring.size() < 4)
        return 0;
    DrawingFeature f;
    f.kind = DrawKind::Polygon;
    f.color = color.isValid() ? color : QColor(0xf5, 0x9e, 0x0b);
    f.widthPx = qBound(1.0, widthPx, 20.0);
    f.pts = ring;
    return addFeature(f);
}

int DrawingManager::addRect(double lat1, double lon1, double lat2, double lon2, const QColor &color,
                            double widthPx)
{
    const double south = qMin(lat1, lat2), north = qMax(lat1, lat2);
    const double west = qMin(lon1, lon2), east = qMax(lon1, lon2);
    if (north - south < 1e-9 && east - west < 1e-9)
        return 0;
    QVector<QPointF> corners;
    corners << QPointF(west, south) << QPointF(east, south)
            << QPointF(east, north) << QPointF(west, north);
    const QVector<QPointF> ring = densifyRing(corners);
    if (ring.size() < 4)
        return 0;
    DrawingFeature f;
    f.kind = DrawKind::Rect;
    f.color = color.isValid() ? color : QColor(0xf5, 0x9e, 0x0b);
    f.widthPx = qBound(1.0, widthPx, 20.0);
    f.pts = ring;
    return addFeature(f);
}

int DrawingManager::addText(double lat, double lon, const QString &text, const QColor &color, int sizePx)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return 0;
    DrawingFeature f;
    f.kind = DrawKind::Text;
    f.color = color.isValid() ? color : QColor(Qt::white);
    f.widthPx = 3.0;
    f.text = trimmed;
    f.textSizePx = qBound(8, sizePx, 72);
    f.pts.append(QPointF(lon, lat));
    return addFeature(f);
}

bool DrawingManager::setFill(int id, const QColor &fill)
{
    const int i = indexOf(id);
    if (i < 0)
        return false;
    DrawingFeature after = m_features.at(i);
    if (!after.isClosed())
        return false;
    if (fill.isValid() && fill.alpha() == 255) {
        QColor c = fill;
        c.setAlphaF(0.35);          // 不透明度没指定时给个能看见底图的默认值
        after.fill = c;
    } else {
        after.fill = fill;
    }
    return updateFeature(after);    // 走 ModifyFeatureCommand，可撤销
}

// 属性编辑：颜色 / 线宽 / 填充 / 文本 / 半径，一条 ModifyFeatureCommand 就能整体回滚
bool DrawingManager::updateFeature(const DrawingFeature &f)
{
    const int i = indexOf(f.id);
    if (i < 0)
        return false;
    DrawingFeature after = f;
    // 圆按圆心 + 半径重新生成点环：改完半径才是真正的等距圆，而不是被拉伸的多边形
    if (after.kind == DrawKind::Circle && after.radiusKm > 0.0) {
        const QVector<QPointF> ring = geocalc::geodesicCircle(after.centerLat, after.centerLon,
                                                             after.radiusKm, 180);
        if (ring.size() >= 3) {
            after.pts = ring;
            after.pts.append(ring.first());
        }
    }
    return pushOrApply(new ModifyFeatureCommand(this, m_features.at(i), after, QStringLiteral("修改属性")));
}

bool DrawingManager::setCircleRadius(int id, double radiusKm)
{
    const DrawingFeature *f = feature(id);
    if (!f || !f->isCircle() || !(radiusKm > 0.0))
        return false;
    DrawingFeature after = *f;
    after.radiusKm = qMin(radiusKm, 20000.0);
    return updateFeature(after);
}

bool DrawingManager::removeFeature(int id)
{
    const int i = indexOf(id);
    if (i < 0)
        return false;
    QVector<int> idx;
    QVector<DrawingFeature> fs;
    idx.append(i);
    fs.append(m_features.at(i));
    return pushOrApply(new RemoveFeaturesCommand(this, idx, fs, QStringLiteral("删除图形")));
}

// 框选删除一次删一批：一个命令、一次重绘
void DrawingManager::removeFeatures(const QVector<int> &ids)
{
    if (ids.isEmpty())
        return;
    QVector<int> idx;
    QVector<DrawingFeature> fs;
    for (int i = 0; i < m_features.size(); ++i) {
        if (ids.contains(m_features.at(i).id)) {
            idx.append(i);
            fs.append(m_features.at(i));
        }
    }
    if (fs.isEmpty())
        return;
    pushOrApply(new RemoveFeaturesCommand(this, idx, fs,
                                          QStringLiteral("删除 %1 个图形").arg(fs.size())));
}

void DrawingManager::clear()
{
    if (m_features.isEmpty())
        return;
    QVector<int> idx;
    QVector<DrawingFeature> fs;
    for (int i = 0; i < m_features.size(); ++i) {
        idx.append(i);
        fs.append(m_features.at(i));
    }
    pushOrApply(new RemoveFeaturesCommand(this, idx, fs, QStringLiteral("清空绘图")));
}

// ---------------------------------------------------------------
//  打断 / 裁剪（CAD 的 Break / Trim）
// ---------------------------------------------------------------
bool DrawingManager::canBreak(int id) const
{
    const DrawingFeature *f = feature(id);
    if (!f || f->isText())
        return false;
    // 闭合图形要多两个点才够剪开（环本身首尾重合）
    return f->isClosed() ? f->pts.size() >= 5 : f->pts.size() >= 3;
}

int DrawingManager::nearestVertex(int id, double lat, double lon) const
{
    const DrawingFeature *f = feature(id);
    if (!f)
        return -1;
    int best = -1;
    double bestD = 0.0;
    for (int i = 0; i < f->pts.size(); ++i) {
        const double d = geocalc::vincentyKm(lat, lon, f->pts.at(i).y(), f->pts.at(i).x());
        if (best < 0 || d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

// 在 vertexIndex 处断开：
//   开放图形 -> 一分为二，两段共用这个顶点，几何一点不差；
//   闭合图形 -> 在这一点把环剪开，变成首尾重合的开放曲线（CAD 剪开一个环就是这个结果）。
// 断点记进 marks，渲染成小圆点；要素被删掉时标记自然跟着消失。
bool DrawingManager::breakFeature(int id, int vertexIndex)
{
    if (!canBreak(id))
        return false;
    const int i = indexOf(id);
    if (i < 0)
        return false;
    const DrawingFeature f = m_features.at(i);
    if (vertexIndex <= 0 || vertexIndex >= f.pts.size() - 1)
        return false;                       // 首尾顶点断不出两段

    QVector<DrawingFeature> pieces;
    if (f.isClosed()) {
        QVector<QPointF> ring = f.pts;
        if (ring.size() > 1 && ring.first() == ring.last())
            ring.removeLast();              // 去掉重复的收尾点，按下标绕环
        if (vertexIndex >= ring.size())
            return false;
        DrawingFeature nf = f;
        nf.kind = DrawKind::Polyline;       // 剪开后不再是闭合图形
        nf.fill = QColor();
        nf.centerLat = nf.centerLon = nf.radiusKm = 0.0;
        nf.pts = ring.mid(vertexIndex) + ring.mid(0, vertexIndex + 1);
        nf.marks.clear();
        nf.marks.append(f.pts.at(vertexIndex));
        pieces.append(nf);
    } else {
        DrawingFeature a = f, b = f;
        a.pts = f.pts.mid(0, vertexIndex + 1);
        b.pts = f.pts.mid(vertexIndex);
        if (a.pts.size() < 2 || b.pts.size() < 2)
            return false;
        a.marks.clear();
        a.marks.append(f.pts.at(vertexIndex));
        b.marks.clear();
        b.marks.append(f.pts.at(vertexIndex));
        pieces << a << b;
    }

    // 一次打断 = 删一个 + 加 N 个，包成宏命令，撤销时一步还原
    const bool macro = m_undo != nullptr;
    if (macro)
        m_undo->beginMacro(QStringLiteral("打断"));
    removeFeature(id);
    for (const DrawingFeature &p : pieces)
        addFeature(p);
    if (macro)
        m_undo->endMacro();
    return true;
}

QVector<int> DrawingManager::crossingFeatures(double lat1, double lon1, double lat2, double lon2) const
{
    QVector<int> out;
    for (const DrawingFeature &f : m_features) {
        if (f.isText() || f.pts.size() < 2)
            continue;                       // 文本不参与；闭合图形的环首尾重合，照样能求交
        for (int k = 0; k + 1 < f.pts.size(); ++k) {
            if (!geocalc::greatCircleIntersections(lat1, lon1, lat2, lon2,
                                                   f.pts.at(k).y(), f.pts.at(k).x(),
                                                   f.pts.at(k + 1).y(), f.pts.at(k + 1).x()).isEmpty()) {
                out.append(f.id);
                break;
            }
        }
    }
    return out;
}

// 裁剪：剪切线（press -> release 的大圆弧）与每个相交的开放图形求球面交点。
// 划两刀（两个交点）就把中间那一段删掉，两头的部分留下；只划一刀就退化成打断。
int DrawingManager::trimCrossing(double lat1, double lon1, double lat2, double lon2)
{
    const QVector<int> ids = crossingFeatures(lat1, lon1, lat2, lon2);
    if (ids.isEmpty())
        return 0;

    auto trimOne = [this, lat1, lon1, lat2, lon2](int id) -> bool {
        const int i = indexOf(id);
        if (i < 0)
            return false;
        const DrawingFeature f = m_features.at(i);
        QVector<int> seg;                   // 交点所在线段下标
        QVector<QPointF> hit;               // 交点（WGS84）
        for (int k = 0; k + 1 < f.pts.size(); ++k) {
            const QVector<QPointF> h = geocalc::greatCircleIntersections(
                lat1, lon1, lat2, lon2, f.pts.at(k).y(), f.pts.at(k).x(),
                f.pts.at(k + 1).y(), f.pts.at(k + 1).x());
            for (const QPointF &p : h) {
                seg.append(k);
                hit.append(p);
            }
        }
        if (hit.isEmpty())
            return false;

        const int s0 = seg.first();
        const int s1 = (hit.size() >= 2) ? seg.at(1) : seg.first();
        const QPointF p0 = hit.first();
        const QPointF p1 = hit.at(hit.size() >= 2 ? 1 : 0);

        if (f.isClosed()) {
            // 环：删掉两个交点之间那一段，剩下的是另一段弧（从 p1 绕过起点回到 p0），
            // 剪开后就是一条开放曲线，不再是闭合图形
            QVector<QPointF> ring = f.pts;
            if (ring.size() > 1 && ring.first() == ring.last())
                ring.removeLast();
            QVector<QPointF> keep;
            keep.append(p1);
            for (int k = s1 + 1; k < ring.size(); ++k)
                keep.append(ring.at(k));
            for (int k = 0; k <= s0 && k < ring.size(); ++k)
                keep.append(ring.at(k));
            keep.append(p0);
            DrawingFeature nf = f;
            nf.kind = DrawKind::Polyline;
            nf.fill = QColor();
            nf.centerLat = nf.centerLon = nf.radiusKm = 0.0;
            nf.pts = keep;
            nf.marks.clear();
            nf.marks << p0 << p1;
            removeFeature(id);
            addFeature(nf);
            return true;
        }

        QVector<QVector<QPointF>> pieces;
        QVector<QPointF> head = f.pts.mid(0, s0 + 1);
        head.append(p0);
        pieces.append(head);

        QVector<QPointF> tail;
        tail.append(p1);
        tail += f.pts.mid(s1 + 1);
        pieces.append(tail);

        removeFeature(id);
        for (int k = 0; k < pieces.size(); ++k) {
            if (pieces.at(k).size() < 2)
                continue;
            DrawingFeature nf = f;
            nf.pts = pieces.at(k);
            nf.marks.clear();
            nf.marks.append(k == 0 ? p0 : p1);   // 两段各留自己的断点标记
            addFeature(nf);
        }
        return true;
    };

    const bool macro = m_undo != nullptr;
    if (macro)
        m_undo->beginMacro(QStringLiteral("裁剪"));
    int changed = 0;
    for (int id : ids) {
        if (trimOne(id))
            ++changed;
    }
    if (macro)
        m_undo->endMacro();
    return changed;
}

QJsonObject DrawingManager::toJson() const
{
    QJsonArray features;
    for (const DrawingFeature &f : m_features) {
        QJsonArray coords;
        for (const QPointF &p : f.pts)
            coords.append(pointJson(p));

        QJsonObject geom;
        if (f.kind == DrawKind::Text) {
            geom.insert(QStringLiteral("type"), QStringLiteral("Point"));
            geom.insert(QStringLiteral("coordinates"), coords.first());
        } else if (f.isClosed()) {
            // GeoJSON 要求环首尾相同；内部顶点本来就已经闭合，这里只是兜底
            if (f.pts.size() > 2 && f.pts.first() != f.pts.last())
                coords.append(pointJson(f.pts.first()));
            QJsonArray rings;
            rings.append(coords);
            geom.insert(QStringLiteral("type"), QStringLiteral("Polygon"));
            geom.insert(QStringLiteral("coordinates"), rings);
        } else {
            geom.insert(QStringLiteral("type"), QStringLiteral("LineString"));
            geom.insert(QStringLiteral("coordinates"), coords);
        }

        QJsonObject props;
        props.insert(QStringLiteral("id"), f.id);
        props.insert(QStringLiteral("kind"), drawKindName(f.kind));
        props.insert(QStringLiteral("color"), f.color.name().toUpper());
        props.insert(QStringLiteral("width"), f.widthPx);
        if (!f.label.isEmpty())
            props.insert(QStringLiteral("label"), f.label);
        if (f.fill.isValid()) {
            props.insert(QStringLiteral("fill"), f.fill.name().toUpper());
            props.insert(QStringLiteral("fillOpacity"), f.fill.alphaF());
        }
        if (!f.marks.isEmpty()) {                 // 打断/裁剪点，跟着要素一起存取
            QJsonArray ma;
            for (const QPointF &p : f.marks)
                ma.append(pointJson(p));
            props.insert(QStringLiteral("marks"), ma);
        }
        if (f.kind == DrawKind::Circle) {
            QJsonArray center;
            center.append(round6(f.centerLon));
            center.append(round6(f.centerLat));
            props.insert(QStringLiteral("center"), center);
            props.insert(QStringLiteral("radiusKm"), f.radiusKm);
            // 显式声明这是「圆」而不是多边形：导入时按圆心+半径还原，半径还能继续调
            props.insert(QStringLiteral("shapeType"), QStringLiteral("circle"));
            props.insert(QStringLiteral("radius"), f.radiusKm);   // 半径，公里
        }
        if (f.kind == DrawKind::Text) {
            props.insert(QStringLiteral("text"), f.text);
            props.insert(QStringLiteral("textSize"), f.textSizePx);
        }

        QJsonObject fo;
        fo.insert(QStringLiteral("type"), QStringLiteral("Feature"));
        fo.insert(QStringLiteral("geometry"), geom);
        fo.insert(QStringLiteral("properties"), props);
        features.append(fo);
    }

    QJsonObject root;
    root.insert(QStringLiteral("type"), QStringLiteral("FeatureCollection"));
    QJsonObject meta;
    meta.insert(QStringLiteral("generator"), QStringLiteral("RadarView"));
    meta.insert(QStringLiteral("version"), 1);
    meta.insert(QStringLiteral("crs"), QStringLiteral("WGS84"));
    root.insert(QStringLiteral("properties"), meta);
    root.insert(QStringLiteral("features"), features);
    return root;
}

bool DrawingManager::fromJson(const QJsonObject &root, QString *error)
{
    auto fail = [error](const QString &msg) {
        if (error)
            *error = msg;
        return false;
    };
    if (root.value(QStringLiteral("type")).toString() != QLatin1String("FeatureCollection"))
        return fail(QStringLiteral("不是 GeoJSON FeatureCollection"));

    const QJsonArray features = root.value(QStringLiteral("features")).toArray();
    QVector<DrawingFeature> parsed;
    int nextId = 1;
    int skipped = 0;
    for (const QJsonValue &v : features) {
        const QJsonObject fo = v.toObject();
        const QJsonObject geom = fo.value(QStringLiteral("geometry")).toObject();
        const QJsonObject props = fo.value(QStringLiteral("properties")).toObject();
        const QString geomType = geom.value(QStringLiteral("type")).toString();

        QVector<QPointF> pts;
        if (geomType == QLatin1String("LineString")) {
            for (const QJsonValue &c : geom.value(QStringLiteral("coordinates")).toArray()) {
                QPointF p;
                if (pointFromJson(c, &p))
                    pts.append(p);
            }
        } else if (geomType == QLatin1String("Polygon")) {
            const QJsonArray rings = geom.value(QStringLiteral("coordinates")).toArray();
            for (const QJsonValue &c : rings.at(0).toArray()) {   // 只取外环
                QPointF p;
                if (pointFromJson(c, &p))
                    pts.append(p);
            }
        } else if (geomType == QLatin1String("Point")) {
            QPointF p;
            if (pointFromJson(geom.value(QStringLiteral("coordinates")), &p))
                pts.append(p);
        } else {
            ++skipped;                    // MultiLineString / MultiPolygon 之类暂不支持
            continue;
        }
        if (pts.isEmpty()) {
            ++skipped;
            continue;
        }

        DrawingFeature f;
        f.pts = pts;
        f.kind = geomType == QLatin1String("Polygon") ? DrawKind::Polygon : DrawKind::Polyline;
        if (geomType == QLatin1String("Point"))
            f.kind = DrawKind::Text;
        drawKindFromName(props.value(QStringLiteral("kind")).toString(), &f.kind);   // 声明优先
        if (f.kind != DrawKind::Text && f.pts.size() < 2) {
            ++skipped;
            continue;
        }
        if (f.isClosed() && f.pts.size() > 2 && f.pts.first() != f.pts.last())
            f.pts.append(f.pts.first());

        const QColor color(props.value(QStringLiteral("color")).toString());
        f.color = color.isValid() ? color : QColor(0xf5, 0x9e, 0x0b);
        const double w = props.value(QStringLiteral("width")).toDouble(3.0);
        f.widthPx = qBound(1.0, w > 0.0 ? w : 3.0, 20.0);
        f.label = props.value(QStringLiteral("label")).toString();

        const QColor fill(props.value(QStringLiteral("fill")).toString());
        if (fill.isValid()) {
            QColor c = fill;
            c.setAlphaF(qBound(0.0, props.value(QStringLiteral("fillOpacity")).toDouble(0.35), 1.0));
            f.fill = c;
        }
        for (const QJsonValue &mv : props.value(QStringLiteral("marks")).toArray()) {
            QPointF mp;
            if (pointFromJson(mv, &mp))
                f.marks.append(mp);
        }

        const QJsonArray center = props.value(QStringLiteral("center")).toArray();
        if (center.size() >= 2) {
            f.centerLon = center.at(0).toDouble();
            f.centerLat = center.at(1).toDouble();
        }
        f.radiusKm = props.value(QStringLiteral("radiusKm")).toDouble(0.0);
        // 圆：properties 里声明了 shapeType=circle 就还原成「圆心 + 半径」模型，点环重新生成，
        // 而不是把导出的 180 边形当成普通多边形（那样半径就再也调不了了）。
        const double radiusProp = props.value(QStringLiteral("radius")).toDouble(f.radiusKm);
        if (props.value(QStringLiteral("shapeType")).toString() == QLatin1String("circle")
            && center.size() >= 2 && radiusProp > 0.0) {
            f.kind = DrawKind::Circle;
            f.radiusKm = radiusProp;
            const QVector<QPointF> ring = geocalc::geodesicCircle(f.centerLat, f.centerLon,
                                                                 f.radiusKm, 180);
            if (ring.size() >= 3) {
                f.pts = ring;
                f.pts.append(ring.first());
            }
        }
        if (f.kind == DrawKind::Text) {
            f.text = props.value(QStringLiteral("text")).toString();
            f.textSizePx = qBound(8, props.value(QStringLiteral("textSize")).toInt(16), 72);
            if (f.text.isEmpty()) {
                ++skipped;
                continue;
            }
        }

        const int id = props.value(QStringLiteral("id")).toInt(0);
        f.id = id > 0 ? id : nextId;
        nextId = qMax(nextId, f.id + 1);
        parsed.append(f);
    }

    if (parsed.isEmpty() && !features.isEmpty())
        return fail(QStringLiteral("没有可用的要素（跳过 %1 个）").arg(skipped));

    m_features = parsed;
    m_nextId = nextId;
    ++m_version;
    emit changed();
    return true;
}

bool DrawingManager::saveToFile(const QString &path, QString *error) const
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    const QByteArray data = QJsonDocument(toJson()).toJson(QJsonDocument::Indented);
    if (f.write(data) != data.size()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}

bool DrawingManager::loadFromFile(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError) {
        if (error)
            *error = QStringLiteral("JSON 解析失败（偏移 %1）：%2").arg(err.offset).arg(err.errorString());
        return false;
    }
    return fromJson(doc.object(), error);
}
