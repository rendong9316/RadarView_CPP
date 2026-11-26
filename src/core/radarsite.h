#ifndef RADARSITE_H
#define RADARSITE_H

#include <QColor>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVector>
#include <limits>

// 雷达站址与 PPI 极坐标态势网格配置（支持单基地及双基地天波雷达 sysPara 自动提取）
struct RadarSite {
    QString id;
    QString name;
    bool visible = true;

    bool hasTx = false;
    double txLat = 0.0;
    double txLon = 0.0;
    double txAltM = 0.0;

    bool hasRx = false;
    double rxLat = 0.0;
    double rxLon = 0.0;
    double rxAltM = 0.0;

    // 探测扇区（来自 sysPara: rIndex[Rbin_start..end] 与 aIndex）
    bool hasSector = false;
    double minRangeKm = 0.0;
    double maxRangeKm = 0.0;
    double startAzDeg = 0.0;
    double endAzDeg = 360.0;
    double beamDirDeg = std::numeric_limits<double>::quiet_NaN();
    double frameIntervalSec = 0.0;

    // PPI 网格显示参数
    double ringStepKm = 0.0;      // <= 0 表示按 maxRangeKm 自动选取整百/整五十公里步长
    double azStepDeg = 10.0;      // 方位射线间隔（度）
    bool showFullRings = false;   // false = 仅探测扇区弧段；true = 360° 整圆
    int refAnchor = 0;            // 0 = Rx（默认），1 = Tx，2 = 收发中点
    QColor color = QColor(0x38, 0xbd, 0xf8);

    // 取得 PPI 网格与状态栏测距所用的参考站坐标（WGS84 度）
    bool anchorLatLon(double *lat, double *lon) const;
    QString anchorLabel() const;
    double effectiveRingStepKm() const;
    bool isValid() const { return hasRx || hasTx; }

    QJsonObject toJson() const;
    static RadarSite fromJson(const QJsonObject &obj);
};

class RadarSiteStore : public QObject
{
    Q_OBJECT
public:
    explicit RadarSiteStore(QObject *parent = nullptr);

    const QVector<RadarSite> &sites() const { return m_sites; }
    int indexOf(const QString &id) const;
    const RadarSite *site(const QString &id) const;
    const RadarSite *activeSite() const;
    QString activeSiteId() const { return m_activeId; }
    void setActiveSiteId(const QString &id);

    bool ppiVisible() const { return m_ppiVisible; }
    void setPpiVisible(bool on);

    // 自动导入 MAT sysPara 时调用：若已存在同站址（< 0.5 km）则原地更新扇区参数，否则新增
    QString upsertFromSysPara(const RadarSite &site);
    QString addSite(RadarSite site);
    bool updateSite(const RadarSite &site);
    bool removeSite(const QString &id);
    void clear();

    void load();
    void save() const;

signals:
    void changed();

private:
    QString generateId() const;
    QVector<RadarSite> m_sites;
    QString m_activeId;
    bool m_ppiVisible = true;
};

#endif // RADARSITE_H
