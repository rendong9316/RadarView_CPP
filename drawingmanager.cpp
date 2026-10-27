#include "drawingmanager.h"
#include "geocalc.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QtMath>
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

int DrawingManager::addFeature(const DrawingFeature &f)
{
    if (f.pts.isEmpty())
        return 0;
    if (f.pts.size() < 2 && f.kind != DrawKind::Text)
        return 0;
    DrawingFeature item = f;
    item.id = m_nextId++;
    m_features.append(item);
    ++m_version;
    emit changed();
    return item.id;
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
    DrawingFeature &f = m_features[i];
    if (!f.isClosed())
        return false;
    if (fill.isValid() && fill.alpha() == 255) {
        QColor c = fill;
        c.setAlphaF(0.35);          // 不透明度没指定时给个能看见底图的默认值
        f.fill = c;
    } else {
        f.fill = fill;
    }
    ++m_version;
    emit changed();
    return true;
}

bool DrawingManager::removeFeature(int id)
{
    const int i = indexOf(id);
    if (i < 0)
        return false;
    m_features.remove(i);
    ++m_version;
    emit changed();
    return true;
}

// 框选删除一次删一批：只在最后 emit 一次，避免每个要素都触发一次重绘
void DrawingManager::removeFeatures(const QVector<int> &ids)
{
    if (ids.isEmpty())
        return;
    bool any = false;
    for (int i = m_features.size() - 1; i >= 0; --i) {
        if (ids.contains(m_features.at(i).id)) {
            m_features.remove(i);
            any = true;
        }
    }
    if (any) {
        ++m_version;
        emit changed();
    }
}

void DrawingManager::clear()
{
    if (m_features.isEmpty())
        return;
    m_features.clear();
    ++m_version;
    emit changed();
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
        if (f.kind == DrawKind::Circle) {
            QJsonArray center;
            center.append(round6(f.centerLon));
            center.append(round6(f.centerLat));
            props.insert(QStringLiteral("center"), center);
            props.insert(QStringLiteral("radiusKm"), f.radiusKm);
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

        const QJsonArray center = props.value(QStringLiteral("center")).toArray();
        if (center.size() >= 2) {
            f.centerLon = center.at(0).toDouble();
            f.centerLat = center.at(1).toDouble();
        }
        f.radiusKm = props.value(QStringLiteral("radiusKm")).toDouble(0.0);
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
