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

} // namespace

QString drawKindName(DrawKind k)
{
    switch (k) {
    case DrawKind::Circle:   return QStringLiteral("circle");
    case DrawKind::Polygon:  return QStringLiteral("polygon");
    case DrawKind::Rect:     return QStringLiteral("rect");
    case DrawKind::Polyline: break;
    }
    return QStringLiteral("polyline");
}

bool drawKindFromName(const QString &name, DrawKind *out)
{
    if (name == QLatin1String("circle"))       { *out = DrawKind::Circle;   return true; }
    if (name == QLatin1String("polygon"))      { *out = DrawKind::Polygon;  return true; }
    if (name == QLatin1String("rect"))         { *out = DrawKind::Rect;     return true; }
    if (name == QLatin1String("polyline"))     { *out = DrawKind::Polyline; return true; }
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
    if (f.pts.size() < 2)
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
        if (f.kind == DrawKind::Polyline) {
            geom.insert(QStringLiteral("type"), QStringLiteral("LineString"));
            geom.insert(QStringLiteral("coordinates"), coords);
        } else {
            // GeoJSON 要求环首尾相同；内部顶点本来就已经闭合，这里只是兜底
            if (f.pts.size() > 2 && f.pts.first() != f.pts.last())
                coords.append(pointJson(f.pts.first()));
            QJsonArray rings;
            rings.append(coords);
            geom.insert(QStringLiteral("type"), QStringLiteral("Polygon"));
            geom.insert(QStringLiteral("coordinates"), rings);
        }

        QJsonObject props;
        props.insert(QStringLiteral("id"), f.id);
        props.insert(QStringLiteral("kind"), drawKindName(f.kind));
        props.insert(QStringLiteral("color"), f.color.name().toUpper());
        props.insert(QStringLiteral("width"), f.widthPx);
        if (!f.label.isEmpty())
            props.insert(QStringLiteral("label"), f.label);
        if (f.kind == DrawKind::Circle) {
            QJsonArray center;
            center.append(round6(f.centerLon));
            center.append(round6(f.centerLat));
            props.insert(QStringLiteral("center"), center);
            props.insert(QStringLiteral("radiusKm"), f.radiusKm);
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
        } else {
            ++skipped;                    // Point / MultiLineString 之类暂不支持
            continue;
        }
        if (pts.size() < 2) {
            ++skipped;
            continue;
        }

        DrawingFeature f;
        f.pts = pts;
        f.kind = geomType == QLatin1String("Polygon") ? DrawKind::Polygon : DrawKind::Polyline;
        drawKindFromName(props.value(QStringLiteral("kind")).toString(), &f.kind);   // 声明优先
        if (f.kind == DrawKind::Polyline && geomType == QLatin1String("Polygon"))
            f.kind = DrawKind::Polygon;   // kind 字段缺失/非法时按几何类型兜底
        if (f.kind != DrawKind::Polyline && f.pts.size() > 2 && f.pts.first() != f.pts.last())
            f.pts.append(f.pts.first());

        const QColor color(props.value(QStringLiteral("color")).toString());
        f.color = color.isValid() ? color : QColor(0xf5, 0x9e, 0x0b);
        const double w = props.value(QStringLiteral("width")).toDouble(3.0);
        f.widthPx = qBound(1.0, w > 0.0 ? w : 3.0, 20.0);
        f.label = props.value(QStringLiteral("label")).toString();
        const QJsonArray center = props.value(QStringLiteral("center")).toArray();
        if (center.size() >= 2) {
            f.centerLon = center.at(0).toDouble();
            f.centerLat = center.at(1).toDouble();
        }
        f.radiusKm = props.value(QStringLiteral("radiusKm")).toDouble(0.0);

        const int id = props.value(QStringLiteral("id")).toInt(0);
        f.id = id > 0 ? id : nextId;
        nextId = qMax(nextId, f.id + 1);
        parsed.append(f);
    }

    if (parsed.isEmpty() && !features.isEmpty())
        return fail(QStringLiteral("没有可用的线/多边形要素（跳过 %1 个）").arg(skipped));

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
