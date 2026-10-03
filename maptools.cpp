#include "maptools.h"
#include "apppaths.h"
#include "geocalc.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QUuid>

// ---------------------------------------------------------------
//  旗标
// ---------------------------------------------------------------
FlagStore::FlagStore(QObject *parent) : QObject(parent)
{
}

int FlagStore::indexOf(const QString &id) const
{
    for (int i = 0; i < m_flags.size(); ++i)
        if (m_flags[i].id == id)
            return i;
    return -1;
}

QString FlagStore::nextLabel() const
{
    static const QRegularExpression re(QStringLiteral("^旗标\\s*(\\d+)$"));
    QSet<int> used;
    for (const MapFlag &f : m_flags) {
        const QRegularExpressionMatch m = re.match(f.label);
        if (m.hasMatch())
            used.insert(m.captured(1).toInt());
    }
    int n = 1;
    while (used.contains(n))
        ++n;
    return QStringLiteral("旗标 %1").arg(n);
}

QString FlagStore::addFlag(double lat, double lon, const QString &label)
{
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0)
        return QString();
    MapFlag f;
    f.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    f.lat = lat;
    f.lon = lon;
    f.label = label.isEmpty() ? nextLabel() : label;
    f.createdAt = QDateTime::currentMSecsSinceEpoch();
    m_flags.append(f);
    save();
    return f.id;
}

void FlagStore::removeFlag(const QString &id)
{
    const int i = indexOf(id);
    if (i < 0)
        return;
    m_flags.remove(i);
    m_selected.removeAll(id);
    save();
}

void FlagStore::renameFlag(const QString &id, const QString &label)
{
    const QString t = label.trimmed();
    const int i = indexOf(id);
    if (t.isEmpty() || i < 0)
        return;
    m_flags[i].label = t;
    save();
}

void FlagStore::toggleSelect(const QString &id)
{
    if (m_selected.contains(id)) {
        m_selected.removeAll(id);
    } else if (m_selected.size() >= 2) {
        m_selected = QStringList() << m_selected[1] << id;
    } else {
        m_selected << id;
    }
    save();
}

void FlagStore::clearAll()
{
    m_flags.clear();
    m_selected.clear();
    save();
}

bool FlagStore::selectedPair(MapFlag *a, MapFlag *b) const
{
    if (m_selected.size() != 2)
        return false;
    const int i = indexOf(m_selected[0]), j = indexOf(m_selected[1]);
    if (i < 0 || j < 0)
        return false;
    *a = m_flags[i];
    *b = m_flags[j];
    return true;
}

void FlagStore::save()
{
    QJsonArray arr;
    for (const MapFlag &f : m_flags) {
        QJsonObject o;
        o.insert(QStringLiteral("id"), f.id);
        o.insert(QStringLiteral("latitude"), f.lat);
        o.insert(QStringLiteral("longitude"), f.lon);
        o.insert(QStringLiteral("label"), f.label);
        o.insert(QStringLiteral("createdAt"), double(f.createdAt));
        arr.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("flags"), arr);
    root.insert(QStringLiteral("selectedFlagIds"), QJsonArray::fromStringList(m_selected));
    app::settings().setValue(QStringLiteral("flags.data"),
                             QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    emit changed();
}

void FlagStore::load()
{
    const QJsonObject root = QJsonDocument::fromJson(
        app::settings().value(QStringLiteral("flags.data")).toString().toUtf8()).object();
    m_flags.clear();
    for (const QJsonValue &v : root.value(QStringLiteral("flags")).toArray()) {
        const QJsonObject o = v.toObject();
        MapFlag f;
        f.id = o.value(QStringLiteral("id")).toString();
        f.lat = o.value(QStringLiteral("latitude")).toDouble();
        f.lon = o.value(QStringLiteral("longitude")).toDouble();
        f.label = o.value(QStringLiteral("label")).toString();
        f.createdAt = qint64(o.value(QStringLiteral("createdAt")).toDouble());
        if (!f.id.isEmpty())
            m_flags.append(f);
    }
    m_selected.clear();
    for (const QJsonValue &v : root.value(QStringLiteral("selectedFlagIds")).toArray())
        if (indexOf(v.toString()) >= 0)
            m_selected << v.toString();
    emit changed();
}

// ---------------------------------------------------------------
//  航线标尺
// ---------------------------------------------------------------
RulerState::RulerState(QObject *parent) : QObject(parent)
{
}

QVector<RulerSegment> RulerState::segments() const
{
    QVector<RulerSegment> out;
    for (int i = 0; i + 1 < m_points.size(); ++i) {
        const Waypoint &a = m_points[i], &b = m_points[i + 1];
        RulerSegment s;
        s.index = i;
        s.distanceKm = geocalc::vincentyKm(a.lat, a.lon, b.lat, b.lon);
        s.bearingDeg = geocalc::initialBearing(a.lat, a.lon, b.lat, b.lon);
        s.cardinal = geocalc::bearingToCardinal(s.bearingDeg);
        out.append(s);
    }
    return out;
}

double RulerState::totalKm() const
{
    double sum = 0.0;
    for (const RulerSegment &s : segments())
        sum += s.distanceKm;
    return sum;
}

bool RulerState::directBearing(double *deg, QString *cardinal) const
{
    if (m_points.size() < 2)
        return false;
    const Waypoint &a = m_points.first(), &b = m_points.last();
    *deg = geocalc::initialBearing(a.lat, a.lon, b.lat, b.lon);
    *cardinal = geocalc::bearingToCardinal(*deg);
    return true;
}

void RulerState::addWaypoint(double lat, double lon)
{
    Waypoint w;
    w.id = m_nextId++;
    w.lat = lat;
    w.lon = lon;
    m_points.append(w);
    emit changed();
}

void RulerState::removeWaypoint(int id)
{
    for (int i = 0; i < m_points.size(); ++i)
        if (m_points[i].id == id) {
            m_points.remove(i);
            emit changed();
            return;
        }
}

void RulerState::undo()
{
    if (!m_points.isEmpty()) {
        m_points.removeLast();
        emit changed();
    }
}

void RulerState::clearAll()
{
    m_points.clear();
    emit changed();
}

void RulerState::setMouse(bool valid, double lat, double lon)
{
    m_hasMouse = valid;
    m_mouseLat = lat;
    m_mouseLon = lon;
    emit mouseMoved();
}

void RulerState::activate()
{
    m_active = true;
    m_points.clear();
    emit changed();
}

void RulerState::deactivate()
{
    m_active = false;
    m_points.clear();
    m_hasMouse = false;
    emit changed();
}

void RulerState::toggle()
{
    if (m_active)
        deactivate();
    else
        activate();
}

QString formatRulerDistance(double km)
{
    return km >= 1.0 ? QString::number(km, 'f', 1) + QStringLiteral(" km")
                     : QString::number(km * 1000.0, 'f', 0) + QStringLiteral(" m");
}
