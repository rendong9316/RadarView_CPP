#include "radarsite.h"
#include "apppaths.h"
#include "geocalc.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
#include <cmath>

bool RadarSite::anchorLatLon(double *lat, double *lon) const
{
    if (!hasRx && !hasTx)
        return false;
    if (refAnchor == 1 && hasTx) {
        *lat = txLat;
        *lon = txLon;
        return true;
    }
    if (refAnchor == 2 && hasTx && hasRx) {
        *lat = 0.5 * (txLat + rxLat);
        *lon = 0.5 * (txLon + rxLon);
        return true;
    }
    if (hasRx) {
        *lat = rxLat;
        *lon = rxLon;
        return true;
    }
    *lat = txLat;
    *lon = txLon;
    return true;
}

QString RadarSite::anchorLabel() const
{
    if (refAnchor == 1 && hasTx)
        return QStringLiteral("Tx");
    if (refAnchor == 2 && hasTx && hasRx)
        return QStringLiteral("中点");
    return hasRx ? QStringLiteral("Rx") : QStringLiteral("Tx");
}

double RadarSite::effectiveRingStepKm() const
{
    if (ringStepKm > 0.0 && std::isfinite(ringStepKm))
        return ringStepKm;
    const double r = maxRangeKm > 0.0 ? maxRangeKm : 500.0;
    if (r >= 2500.0) return 500.0;
    if (r >= 1000.0) return 200.0;
    if (r >= 400.0)  return 100.0;
    if (r >= 150.0)  return 50.0;
    return 20.0;
}

QJsonObject RadarSite::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), id);
    o.insert(QStringLiteral("name"), name);
    o.insert(QStringLiteral("visible"), visible);
    o.insert(QStringLiteral("has_tx"), hasTx);
    o.insert(QStringLiteral("tx_lat"), txLat);
    o.insert(QStringLiteral("tx_lon"), txLon);
    o.insert(QStringLiteral("tx_alt_m"), txAltM);
    o.insert(QStringLiteral("has_rx"), hasRx);
    o.insert(QStringLiteral("rx_lat"), rxLat);
    o.insert(QStringLiteral("rx_lon"), rxLon);
    o.insert(QStringLiteral("rx_alt_m"), rxAltM);
    o.insert(QStringLiteral("has_sector"), hasSector);
    o.insert(QStringLiteral("min_range_km"), minRangeKm);
    o.insert(QStringLiteral("max_range_km"), maxRangeKm);
    o.insert(QStringLiteral("start_az_deg"), startAzDeg);
    o.insert(QStringLiteral("end_az_deg"), endAzDeg);
    if (std::isfinite(beamDirDeg))
        o.insert(QStringLiteral("beam_dir_deg"), beamDirDeg);
    o.insert(QStringLiteral("frame_interval_sec"), frameIntervalSec);
    o.insert(QStringLiteral("ring_step_km"), ringStepKm);
    o.insert(QStringLiteral("az_step_deg"), azStepDeg);
    o.insert(QStringLiteral("show_full_rings"), showFullRings);
    o.insert(QStringLiteral("ref_anchor"), refAnchor);
    o.insert(QStringLiteral("color"), color.name());
    return o;
}

RadarSite RadarSite::fromJson(const QJsonObject &o)
{
    RadarSite s;
    s.id = o.value(QStringLiteral("id")).toString();
    s.name = o.value(QStringLiteral("name")).toString();
    s.visible = o.value(QStringLiteral("visible")).toBool(true);
    s.hasTx = o.value(QStringLiteral("has_tx")).toBool(false);
    s.txLat = o.value(QStringLiteral("tx_lat")).toDouble(0.0);
    s.txLon = o.value(QStringLiteral("tx_lon")).toDouble(0.0);
    s.txAltM = o.value(QStringLiteral("tx_alt_m")).toDouble(0.0);
    s.hasRx = o.value(QStringLiteral("has_rx")).toBool(false);
    s.rxLat = o.value(QStringLiteral("rx_lat")).toDouble(0.0);
    s.rxLon = o.value(QStringLiteral("rx_lon")).toDouble(0.0);
    s.rxAltM = o.value(QStringLiteral("rx_alt_m")).toDouble(0.0);
    s.hasSector = o.value(QStringLiteral("has_sector")).toBool(false);
    s.minRangeKm = o.value(QStringLiteral("min_range_km")).toDouble(0.0);
    s.maxRangeKm = o.value(QStringLiteral("max_range_km")).toDouble(0.0);
    s.startAzDeg = o.value(QStringLiteral("start_az_deg")).toDouble(0.0);
    s.endAzDeg = o.value(QStringLiteral("end_az_deg")).toDouble(360.0);
    if (o.contains(QStringLiteral("beam_dir_deg")))
        s.beamDirDeg = o.value(QStringLiteral("beam_dir_deg")).toDouble(std::numeric_limits<double>::quiet_NaN());
    s.frameIntervalSec = o.value(QStringLiteral("frame_interval_sec")).toDouble(0.0);
    s.ringStepKm = o.value(QStringLiteral("ring_step_km")).toDouble(0.0);
    s.azStepDeg = qBound(1.0, o.value(QStringLiteral("az_step_deg")).toDouble(10.0), 90.0);
    s.showFullRings = o.value(QStringLiteral("show_full_rings")).toBool(false);
    s.refAnchor = qBound(0, o.value(QStringLiteral("ref_anchor")).toInt(0), 2);
    const QColor c(o.value(QStringLiteral("color")).toString());
    if (c.isValid())
        s.color = c;
    return s;
}

RadarSiteStore::RadarSiteStore(QObject *parent)
    : QObject(parent)
{
}

QString RadarSiteStore::generateId() const
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

int RadarSiteStore::indexOf(const QString &id) const
{
    for (int i = 0; i < m_sites.size(); ++i)
        if (m_sites[i].id == id)
            return i;
    return -1;
}

const RadarSite *RadarSiteStore::site(const QString &id) const
{
    const int i = indexOf(id);
    return i >= 0 ? &m_sites[i] : nullptr;
}

const RadarSite *RadarSiteStore::activeSite() const
{
    const RadarSite *s = site(m_activeId);
    if (s && s->isValid())
        return s;
    for (const RadarSite &cand : m_sites)
        if (cand.visible && cand.isValid())
            return &cand;
    return nullptr;
}

void RadarSiteStore::setActiveSiteId(const QString &id)
{
    if (m_activeId == id)
        return;
    m_activeId = id;
    save();
    emit changed();
}

void RadarSiteStore::setPpiVisible(bool on)
{
    if (m_ppiVisible == on)
        return;
    m_ppiVisible = on;
    save();
    emit changed();
}

QString RadarSiteStore::upsertFromSysPara(const RadarSite &incoming)
{
    if (!incoming.isValid())
        return QString();
    double inLat = 0.0, inLon = 0.0;
    incoming.anchorLatLon(&inLat, &inLon);

    for (RadarSite &ex : m_sites) {
        double exLat = 0.0, exLon = 0.0;
        if (!ex.anchorLatLon(&exLat, &exLon))
            continue;
        if (geocalc::vincentyKm(inLat, inLon, exLat, exLon) < 0.5) {
            // 同站址：合并探测扇区与收发坐标，保留用户自定义的显示名/颜色/网格习惯
            if (incoming.hasTx) {
                ex.hasTx = true;
                ex.txLat = incoming.txLat;
                ex.txLon = incoming.txLon;
                ex.txAltM = incoming.txAltM;
            }
            if (incoming.hasRx) {
                ex.hasRx = true;
                ex.rxLat = incoming.rxLat;
                ex.rxLon = incoming.rxLon;
                ex.rxAltM = incoming.rxAltM;
            }
            if (incoming.hasSector) {
                ex.hasSector = true;
                ex.minRangeKm = incoming.minRangeKm;
                ex.maxRangeKm = incoming.maxRangeKm;
                ex.startAzDeg = incoming.startAzDeg;
                ex.endAzDeg = incoming.endAzDeg;
                if (std::isfinite(incoming.beamDirDeg))
                    ex.beamDirDeg = incoming.beamDirDeg;
            }
            if (incoming.frameIntervalSec > 0.0)
                ex.frameIntervalSec = incoming.frameIntervalSec;
            if (m_activeId.isEmpty())
                m_activeId = ex.id;
            save();
            emit changed();
            return ex.id;
        }
    }
    return addSite(incoming);
}

QString RadarSiteStore::addSite(RadarSite site)
{
    if (!site.isValid())
        return QString();
    if (site.id.isEmpty())
        site.id = generateId();
    if (site.name.trimmed().isEmpty())
        site.name = QStringLiteral("雷达站 %1").arg(m_sites.size() + 1);
    m_sites.append(site);
    if (m_activeId.isEmpty())
        m_activeId = site.id;
    save();
    emit changed();
    return site.id;
}

bool RadarSiteStore::updateSite(const RadarSite &site)
{
    const int i = indexOf(site.id);
    if (i < 0 || !site.isValid())
        return false;
    m_sites[i] = site;
    save();
    emit changed();
    return true;
}

bool RadarSiteStore::removeSite(const QString &id)
{
    const int i = indexOf(id);
    if (i < 0)
        return false;
    m_sites.remove(i);
    if (m_activeId == id)
        m_activeId = m_sites.isEmpty() ? QString() : m_sites.first().id;
    save();
    emit changed();
    return true;
}

void RadarSiteStore::clear()
{
    if (m_sites.isEmpty() && m_activeId.isEmpty())
        return;
    m_sites.clear();
    m_activeId.clear();
    save();
    emit changed();
}

void RadarSiteStore::load()
{
    app::Settings &s = app::settings();
    m_sites.clear();
    const QJsonArray arr = QJsonDocument::fromJson(
        s.value(QStringLiteral("radar.sites")).toString().toUtf8()).array();
    for (const QJsonValue &v : arr) {
        const RadarSite rs = RadarSite::fromJson(v.toObject());
        if (rs.isValid() && !rs.id.isEmpty())
            m_sites.append(rs);
    }
    m_activeId = s.value(QStringLiteral("radar.active_site")).toString();
    if (indexOf(m_activeId) < 0)
        m_activeId = m_sites.isEmpty() ? QString() : m_sites.first().id;
    m_ppiVisible = s.value(QStringLiteral("radar.ppi_visible"), true).toBool();
    emit changed();
}

void RadarSiteStore::save() const
{
    QJsonArray arr;
    for (const RadarSite &rs : m_sites)
        arr.append(rs.toJson());
    app::setValue(QStringLiteral("radar.sites"),
                  QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    app::setValue(QStringLiteral("radar.active_site"), m_activeId);
    app::setValue(QStringLiteral("radar.ppi_visible"), m_ppiVisible);
}
