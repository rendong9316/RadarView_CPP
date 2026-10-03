#ifndef MAPTOOLS_H
#define MAPTOOLS_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

// 旗标与航线标尺的数据，对应 RadarView src/composables/useFlags.ts、useRuler.ts。
// 地图叠加层负责绘制，侧栏旗标面板负责列表编辑

struct MapFlag {
    QString id;
    double lat = 0.0, lon = 0.0;
    QString label;
    qint64 createdAt = 0;
};

class FlagStore : public QObject
{
    Q_OBJECT
public:
    explicit FlagStore(QObject *parent = nullptr);

    const QVector<MapFlag> &flags() const { return m_flags; }
    const QStringList &selectedIds() const { return m_selected; }
    int indexOf(const QString &id) const;
    QString nextLabel() const;                 // 「旗标 N」，N 取最小未用的正整数

    // 超出经纬度范围时不添加，返回空 id
    QString addFlag(double lat, double lon, const QString &label = QString());
    void removeFlag(const QString &id);
    void renameFlag(const QString &id, const QString &label);
    void toggleSelect(const QString &id);      // 最多选两个，超出时丢掉最早选的
    void clearAll();
    // 选中两个时返回 true 和这两个旗标
    bool selectedPair(MapFlag *a, MapFlag *b) const;

    void load();                               // 从设置 flags.data 读取
signals:
    void changed();

private:
    void save();
    QVector<MapFlag> m_flags;
    QStringList m_selected;
};

struct RulerSegment {
    int index;
    double distanceKm, bearingDeg;
    QString cardinal;
};

class RulerState : public QObject
{
    Q_OBJECT
public:
    struct Waypoint { int id; double lat, lon; };
    explicit RulerState(QObject *parent = nullptr);

    bool isActive() const { return m_active; }
    const QVector<Waypoint> &waypoints() const { return m_points; }
    QVector<RulerSegment> segments() const;
    double totalKm() const;
    bool directBearing(double *deg, QString *cardinal) const;
    bool hasMouse() const { return m_hasMouse; }
    double mouseLat() const { return m_mouseLat; }
    double mouseLon() const { return m_mouseLon; }

    void addWaypoint(double lat, double lon);
    void removeWaypoint(int id);
    void undo();
    void clearAll();
    void setMouse(bool valid, double lat, double lon);
    void activate();
    void deactivate();
    void toggle();

signals:
    void changed();
    void mouseMoved();

private:
    bool m_active = false;
    QVector<Waypoint> m_points;
    int m_nextId = 0;
    bool m_hasMouse = false;
    double m_mouseLat = 0.0, m_mouseLon = 0.0;
};

// 距离显示：>= 1 km 保留 1 位小数，否则按米取整（FlagPanel.vue）
QString formatRulerDistance(double km);

#endif // MAPTOOLS_H
