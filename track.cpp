#include "track.h"

#include <QSet>
#include <algorithm>
#include <cmath>

namespace {

const qint64 kBeijingOffsetMs = 8LL * 3600 * 1000;

// 同一数据源的多个文件按导入顺序轮换的颜色，第一个文件用该数据源的默认色。
// 雷达沿用 RadarView RADAR_IMPORT_COLORS；原始量测、ADS-B 各用一组与之错开的颜色
const char *const kRadarFileColors[] = {
    "#00ff88", "#ffcc00", "#3ba7ff", "#ff5f8f", "#9bff3b", "#ff8a3b", "#c17dff", "#38f2ff"
};
const char *const kRawFileColors[] = {
    "#ff8800", "#e040fb", "#40c4ff", "#eeff41", "#ff5252", "#64ffda", "#ffab40", "#b388ff"
};
const char *const kAdsbFileColors[] = {
    "#00d4ff", "#ff7eb6", "#ffe066", "#7cff6b", "#c08bff", "#ff9e57", "#5cffd6", "#ff5c5c"
};
const int kPaletteSize = 8;

// 调色板用完后按黄金角旋转色相继续生成，仍与已用颜色错开
QColor rotatedColor(TrackSource s, int n)
{
    const double base = s == TrackSource::Adsb ? 190.0 : s == TrackSource::Radar ? 150.0 : 30.0;
    const double hue = std::fmod(base + n * 137.508, 360.0);
    return QColor::fromHslF(hue / 360.0, 0.85, 0.6);
}

// 公历日期 -> 1970-01-01 起的天数（Howard Hinnant 算法）
qint64 daysFromCivil(int y, int m, int d)
{
    y -= m <= 2;
    const qint64 era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = int(y - era * 400);
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void civilFromDays(qint64 z, int *y, int *m, int *d)
{
    z += 719468;
    const qint64 era = (z >= 0 ? z : z - 146096) / 146097;
    const int doe = int(z - era * 146097);
    const int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3 : -9);
    *y = int(yoe + era * 400) + (*m <= 2);
}

bool readDigits(const char *s, int n, int *out)
{
    int v = 0;
    for (int i = 0; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return true;
}

} // namespace

QString trackSourceName(TrackSource s)
{
    switch (s) {
    case TrackSource::Adsb: return QStringLiteral("ADS-B");
    case TrackSource::Radar: return QStringLiteral("Radar");
    case TrackSource::RadarRaw: return QStringLiteral("RadarRaw");
    }
    return QString();
}

QColor trackSourceColor(TrackSource s)
{
    switch (s) {
    case TrackSource::Adsb: return QColor(QStringLiteral("#00d4ff"));
    case TrackSource::Radar: return QColor(QStringLiteral("#00ff88"));
    case TrackSource::RadarRaw: return QColor(QStringLiteral("#ff8800"));
    }
    return QColor(Qt::white);
}

bool trackSourceFromName(const QString &name, TrackSource *out)
{
    for (TrackSource s : { TrackSource::Adsb, TrackSource::Radar, TrackSource::RadarRaw })
        if (trackSourceName(s) == name) {
            *out = s;
            return true;
        }
    return false;
}

QString trackSourceKey(TrackSource s)
{
    switch (s) {
    case TrackSource::Adsb: return QStringLiteral("adsb");
    case TrackSource::Radar: return QStringLiteral("radar");
    case TrackSource::RadarRaw: return QStringLiteral("radar_raw");
    }
    return QString();
}

bool trackSourceFromKey(const QString &key, TrackSource *out)
{
    for (TrackSource s : { TrackSource::Adsb, TrackSource::Radar, TrackSource::RadarRaw })
        if (trackSourceKey(s) == key) {
            *out = s;
            return true;
        }
    return false;
}

QString Track::key() const
{
    return id + QStringLiteral("::") + trackSourceName(source) + QStringLiteral("::") + fileName;
}

void Track::normalize()
{
    std::stable_sort(points.begin(), points.end(),
                     [](const TrackPoint &a, const TrackPoint &b) { return a.t < b.t; });
    auto last = std::unique(points.begin(), points.end(),
                            [](const TrackPoint &a, const TrackPoint &b) { return a.t == b.t; });
    points.erase(last, points.end());
}

bool parseBeijingTime(const char *s, int len, qint64 *ms)
{
    if (len < 19)
        return false;
    int y, mo, d, h, mi, se;
    if (!readDigits(s, 4, &y) || !readDigits(s + 5, 2, &mo) || !readDigits(s + 8, 2, &d)
            || !readDigits(s + 11, 2, &h) || !readDigits(s + 14, 2, &mi) || !readDigits(s + 17, 2, &se))
        return false;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60)
        return false;
    int frac = 0;
    if (len > 20 && s[19] == '.') {
        int scale = 100;
        for (int i = 20; i < len && i < 23 && s[i] >= '0' && s[i] <= '9'; ++i, scale /= 10)
            frac += (s[i] - '0') * scale;
    }
    *ms = ((daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se) * 1000 + frac) - kBeijingOffsetMs;
    return true;
}

QString formatBeijingTime(qint64 ms)
{
    const qint64 local = ms + kBeijingOffsetMs;
    qint64 days = local / 86400000;
    qint64 rem = local % 86400000;
    if (rem < 0) {
        rem += 86400000;
        --days;
    }
    int y, m, d;
    civilFromDays(days, &y, &m, &d);
    const int sec = int(rem / 1000);
    return QStringLiteral("%1 %2 %3 %4:%5:%6")
        .arg(y, 4, 10, QLatin1Char('0'))
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(d, 2, 10, QLatin1Char('0'))
        .arg(sec / 3600, 2, 10, QLatin1Char('0'))
        .arg(sec / 60 % 60, 2, 10, QLatin1Char('0'))
        .arg(sec % 60, 2, 10, QLatin1Char('0'));
}

// ---------------------------------------------------------------
//  TrackStore
// ---------------------------------------------------------------
int TrackStore::addTracks(QVector<Track> tracks)
{
    int added = 0;
    for (Track &t : tracks) {
        t.normalize();
        if (t.points.isEmpty())
            continue;

        const QString fileKey = trackSourceName(t.source) + QStringLiteral("::") + t.fileName;
        if (!m_fileColors.contains(fileKey)) {
            // 同一数据源已用过的颜色（含已清空但记住颜色的文件），新文件取第一个没用过的
            const QString prefix = trackSourceName(t.source) + QStringLiteral("::");
            QSet<QRgb> used;
            for (auto it = m_fileColors.constBegin(); it != m_fileColors.constEnd(); ++it)
                if (it.key().startsWith(prefix))
                    used.insert(it.value().rgb());
            const char *const *palette = t.source == TrackSource::Adsb ? kAdsbFileColors
                                       : t.source == TrackSource::Radar ? kRadarFileColors : kRawFileColors;
            QColor c;
            for (int n = 0; !c.isValid(); ++n) {
                const QColor cand = n < kPaletteSize ? QColor(QLatin1String(palette[n])) : rotatedColor(t.source, n);
                if (!used.contains(cand.rgb()))
                    c = cand;
            }
            m_fileColors.insert(fileKey, c);
        }

        const QString k = t.key();
        auto it = m_index.find(k);
        if (it == m_index.end()) {
            m_index.insert(k, m_tracks.size());
            m_tracks.append(std::move(t));
            ++added;
            continue;
        }
        // 同 key：只合并新时间点，空的元数据字段用新数据补齐
        Track &dst = m_tracks[it.value()];
        QSet<qint64> seen;
        seen.reserve(dst.points.size());
        for (const TrackPoint &p : qAsConst(dst.points))
            seen.insert(p.t);
        for (const TrackPoint &p : qAsConst(t.points))
            if (!seen.contains(p.t))
                dst.points.append(p);
        dst.normalize();
        QString *fields[] = { &dst.flightNo, &dst.icaoFlightNo, &dst.aircraftType, &dst.registration,
                              &dst.airline, &dst.origin, &dst.destination };
        const QString src[] = { t.flightNo, t.icaoFlightNo, t.aircraftType, t.registration,
                                t.airline, t.origin, t.destination };
        for (int i = 0; i < 7; ++i)
            if (fields[i]->isEmpty())
                *fields[i] = src[i];
    }
    ++m_version;
    return added;
}

int TrackStore::removeTracks(const QSet<QString> &keys)
{
    if (keys.isEmpty())
        return 0;
    QVector<Track> kept;
    kept.reserve(m_tracks.size());
    for (Track &t : m_tracks)
        if (!keys.contains(t.key()))
            kept.append(std::move(t));
    const int removed = m_tracks.size() - kept.size();
    m_tracks = std::move(kept);
    m_index.clear();
    for (int i = 0; i < m_tracks.size(); ++i)
        m_index.insert(m_tracks[i].key(), i);
    ++m_version;
    return removed;
}

void TrackStore::clear()
{
    m_tracks.clear();
    m_index.clear();
    ++m_version;
}

qint64 TrackStore::pointCount() const
{
    qint64 n = 0;
    for (const Track &t : m_tracks)
        n += t.points.size();
    return n;
}

qint64 TrackStore::minTime() const
{
    qint64 v = 0;
    bool first = true;
    for (const Track &t : m_tracks) {
        if (first || t.minTime() < v)
            v = t.minTime();
        first = false;
    }
    return v;
}

qint64 TrackStore::maxTime() const
{
    qint64 v = 0;
    for (const Track &t : m_tracks)
        v = qMax(v, t.maxTime());
    return v;
}

QColor TrackStore::fileColor(TrackSource s, const QString &fileName) const
{
    return m_fileColors.value(trackSourceName(s) + QStringLiteral("::") + fileName, trackSourceColor(s));
}

QString TrackStore::uniqueFileName(TrackSource s, const QSet<QString> &extraUsed) const
{
    const QString base = s == TrackSource::RadarRaw ? QStringLiteral("RadarRaw") : QStringLiteral("Radar");
    QSet<QString> used = extraUsed;
    for (const Track &t : m_tracks)
        if (t.source == s)
            used.insert(t.fileName);
    if (!used.contains(base))
        return base;
    for (int i = 2;; ++i) {
        const QString c = base + QString::number(i);
        if (!used.contains(c))
            return c;
    }
}
