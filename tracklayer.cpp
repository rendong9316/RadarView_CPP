#include "tracklayer.h"
#include "track.h"
#include "geo.h"

#include <QOpenGLShaderProgram>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <limits>

const double TrackLayer::kAltitudeM = 10000.0;

namespace {

const int kQuadsPerBatch = 16384;            // 16384 * 4 = 65536 个顶点，正好用满 16 位索引
const int kLineStride = 10;                  // p0(3) p1(3) side end endTime startTime
const int kDotStride = 5;                    // pos(3) corner(2)
const double kMaxPieceDeg = 0.5;             // 长线段按此角度细分，避免弦线钻进地面
const float kLineHalfWidthPx = 1.0f;         // 线宽 2px
const float kHoverHalfWidthPx = 2.5f;        // 悬停线宽 5px（与 RadarView HOVER_WIDTH 一致）
const float kHoverDotScale = 1.6f;           // 悬停端点放大倍数
const QVector4D kHoverColor(1.0f, 0.2f, 0.2f, 1.0f);   // #ff3333

// 线段四边形：每个顶点都带两个端点，着色器在屏幕空间算法线并沿 side 方向展开
const char *const kLineVs = R"(
attribute vec3 a_p0;
attribute vec3 a_p1;
attribute vec4 a_param;         // x: 侧向 ±1, y: 0 起点 / 1 终点, z: 线段结束时刻, w: 线段开始时刻（秒）
uniform mat4 u_mvp;
uniform vec2 u_viewport;        // 视口像素尺寸
uniform vec2 u_halfWidth;       // x: 线宽一半, y: 含抗锯齿余量
uniform vec2 u_window;          // 回放显示的时间窗口（按线段结束时刻）
uniform vec2 u_filter;          // 时间筛选：线段两端都要落在窗口内
varying float v_edge;
varying float v_hw;             // 线宽一半，经 varying 传给片元（uniform 两段精度不同时 ES2 链接会失败）
void main()
{
    v_hw = u_halfWidth.x;
    vec4 c0 = u_mvp * vec4(a_p0, 1.0);
    vec4 c1 = u_mvp * vec4(a_p1, 1.0);
    v_edge = 0.0;
    if (a_param.z < u_window.x || a_param.z > u_window.y || a_param.w < u_filter.x || a_param.z > u_filter.y
            || c0.w < 1e-6 || c1.w < 1e-6) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);    // 移出裁剪空间
        return;
    }
    vec2 half_vp = 0.5 * u_viewport;
    vec2 d = (c1.xy / c1.w - c0.xy / c0.w) * half_vp;
    float len = length(d);
    vec2 n = len > 1e-5 ? vec2(-d.y, d.x) / len : vec2(0.0, 1.0);
    vec4 c = a_param.y > 0.5 ? c1 : c0;
    vec2 off = n * (a_param.x * u_halfWidth.y) / half_vp;
    gl_Position = vec4(c.xy + off * c.w, c.z, c.w);
    v_edge = a_param.x * u_halfWidth.y;
}
)";

const char *const kLineFs = R"(
#ifdef GL_ES
precision mediump float;
#endif
uniform vec4 u_color;
varying float v_edge;
varying float v_hw;
void main()
{
    float a = clamp(v_hw + 0.5 - abs(v_edge), 0.0, 1.0);
    gl_FragColor = vec4(u_color.rgb, u_color.a * a);
}
)";

// 端点圆点：四边形 + 片元里画圆和深色描边
const char *const kDotVs = R"(
attribute vec3 a_pos;
attribute vec2 a_corner;
uniform mat4 u_mvp;
uniform vec2 u_viewport;
uniform float u_radius;
varying vec2 v_uv;
varying float v_r;
void main()
{
    v_r = u_radius;
    vec4 c = u_mvp * vec4(a_pos, 1.0);
    v_uv = a_corner * (u_radius + 1.0);
    if (c.w < 1e-6) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    gl_Position = vec4(c.xy + v_uv / (0.5 * u_viewport) * c.w, c.z, c.w);
}
)";

const char *const kDotFs = R"(
#ifdef GL_ES
precision mediump float;
#endif
uniform vec4 u_color;
varying vec2 v_uv;
varying float v_r;
void main()
{
    float r = length(v_uv);
    float a = clamp(v_r + 0.5 - r, 0.0, 1.0);
    if (a <= 0.0)
        discard;
    float ring = clamp(r - (v_r - 1.5), 0.0, 1.0);
    gl_FragColor = vec4(mix(u_color.rgb, vec3(0.0), ring * 0.85), a);
}
)";

void pushQuad(QVector<float> &v, const QVector3D &a, const QVector3D &b, float t, float t0)
{
    for (int k = 0; k < 4; ++k) {
        v << a.x() << a.y() << a.z() << b.x() << b.y() << b.z()
          << ((k & 1) ? -1.0f : 1.0f) << (k >= 2 ? 1.0f : 0.0f) << t << t0;
    }
}

void pushDot(QVector<float> &v, const QVector3D &p)
{
    static const float corners[4][2] = { {-1, -1}, {1, -1}, {-1, 1}, {1, 1} };
    for (const auto &c : corners)
        v << p.x() << p.y() << p.z() << c[0] << c[1];
}

double wrapLon(double d)
{
    if (d > 180.0) return d - 360.0;
    if (d < -180.0) return d + 360.0;
    return d;
}

// 两点间的线段（经纬度线性插值，与回放插值一致），过长时细分；回调每一小段
template <typename F>
void forEachPiece(double lon0, double lat0, double lon1, double lat1, F onPiece)
{
    const double dlon = wrapLon(lon1 - lon0);
    const double dlat = lat1 - lat0;
    const double ang = std::max(std::fabs(dlat),
                                std::fabs(dlon) * std::cos((lat0 + lat1) * 0.5 * M_PI / 180.0));
    const int n = qBound(1, int(std::ceil(ang / kMaxPieceDeg)), 256);
    QVector3D prev = geo::ecefDeg(lon0, lat0, TrackLayer::kAltitudeM);
    for (int i = 1; i <= n; ++i) {
        const double f = double(i) / n;
        const QVector3D cur = geo::ecefDeg(lon0 + dlon * f, lat0 + dlat * f, TrackLayer::kAltitudeM);
        onPiece(prev, cur);   // 注意：不能叫 emit，Qt 把 emit 定义成了空宏
        prev = cur;
    }
}

// 回放时刻 t 时航迹所在位置（经纬度线性插值），只在点下标 [lo, hi] 内找；还没开始返回 false。
// *last 为已经过的最后一个点的下标
bool replayPosition(const QVector<TrackPoint> &pts, int lo, int hi, qint64 t, double *lon, double *lat, int *last)
{
    const auto first = pts.begin() + lo, end = pts.begin() + hi + 1;
    auto up = std::upper_bound(first, end, t,
                               [](qint64 v, const TrackPoint &p) { return v < p.t; });
    if (up == first)
        return false;
    const TrackPoint &a = *(up - 1);
    *last = int(up - pts.begin()) - 1;
    *lon = a.lon;
    *lat = a.lat;
    if (up != end) {
        const TrackPoint &b = *up;
        const double f = b.t > a.t ? double(t - a.t) / double(b.t - a.t) : 0.0;
        *lon = a.lon + wrapLon(b.lon - a.lon) * f;
        *lat = a.lat + (b.lat - a.lat) * f;
        if (*lon > 180.0) *lon -= 360.0;
        if (*lon < -180.0) *lon += 360.0;
    }
    return true;
}

} // namespace

TrackLayer::TrackLayer() = default;

TrackLayer::~TrackLayer()
{
    // GL 资源由 cleanup() 在上下文当前时释放
}

bool TrackLayer::initialize(QOpenGLFunctions *gl)
{
    m_gl = gl;
    auto build = [this](QOpenGLShaderProgram *&prog, const char *vs, const char *fs,
                        const QVector<QByteArray> &attrs) {
        prog = new QOpenGLShaderProgram;
        bool ok = prog->addShaderFromSourceCode(QOpenGLShader::Vertex, vs)
               && prog->addShaderFromSourceCode(QOpenGLShader::Fragment, fs);
        for (int i = 0; ok && i < attrs.size(); ++i)
            prog->bindAttributeLocation(attrs[i].constData(), i);
        ok = ok && prog->link();
        if (!ok) {
            m_log += prog->log();
            delete prog;
            prog = nullptr;
        }
        return ok;
    };
    if (!build(m_lineProg, kLineVs, kLineFs, { "a_p0", "a_p1", "a_param" })
            || !build(m_dotProg, kDotVs, kDotFs, { "a_pos", "a_corner" })) {
        qWarning("TrackLayer shader error: %s", qPrintable(m_log));
        return false;
    }
    m_uLineMvp = m_lineProg->uniformLocation("u_mvp");
    m_uLineViewport = m_lineProg->uniformLocation("u_viewport");
    m_uLineHalfWidth = m_lineProg->uniformLocation("u_halfWidth");
    m_uLineWindow = m_lineProg->uniformLocation("u_window");
    m_uLineColor = m_lineProg->uniformLocation("u_color");
    m_uLineFilter = m_lineProg->uniformLocation("u_filter");
    m_uDotMvp = m_dotProg->uniformLocation("u_mvp");
    m_uDotViewport = m_dotProg->uniformLocation("u_viewport");
    m_uDotRadius = m_dotProg->uniformLocation("u_radius");
    m_uDotColor = m_dotProg->uniformLocation("u_color");

    // 所有四边形共用一份索引：(0,1,2) (1,3,2)
    QVector<GLushort> idx;
    idx.reserve(kQuadsPerBatch * 6);
    for (int q = 0; q < kQuadsPerBatch; ++q) {
        const GLushort b = GLushort(q * 4);
        idx << b << GLushort(b + 1) << GLushort(b + 2) << GLushort(b + 1) << GLushort(b + 3) << GLushort(b + 2);
    }
    m_gl->glGenBuffers(1, &m_quadIbo);
    m_gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_quadIbo);
    m_gl->glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * int(sizeof(GLushort)), idx.constData(), GL_STATIC_DRAW);
    m_gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    m_gl->glGenBuffers(1, &m_headVbo);
    m_gl->glGenBuffers(1, &m_dotVbo);
    m_gl->glGenBuffers(1, &m_ptDotVbo);
    m_builtVersion = ~quint64(0);
    m_dynamicDirty = true;
    m_ptDotsDirty = true;
    return true;
}

void TrackLayer::cleanup()
{
    if (!m_gl)
        return;
    releaseBatches();
    GLuint bufs[4] = { m_quadIbo, m_headVbo, m_dotVbo, m_ptDotVbo };
    m_gl->glDeleteBuffers(4, bufs);
    m_quadIbo = m_headVbo = m_dotVbo = m_ptDotVbo = 0;
    delete m_lineProg;
    delete m_dotProg;
    m_lineProg = m_dotProg = nullptr;
    m_gl = nullptr;
}

void TrackLayer::releaseBatches()
{
    for (const Batch &b : qAsConst(m_batches))
        m_gl->glDeleteBuffers(1, &b.vbo);
    m_batches.clear();
    m_trackPieces.clear();
    m_segments = 0;
}

void TrackLayer::setStore(const TrackStore *store)
{
    m_store = store;
    m_builtVersion = ~quint64(0);
    m_pickVersion = ~quint64(0);
    m_dynamicDirty = true;
}

void TrackLayer::setGroupVisible(const QString &groupKey, bool visible)
{
    if (visible)
        m_hidden.remove(groupKey);
    else
        m_hidden.insert(groupKey, true);
    const int g = groupIndex(groupKey);
    if (g >= 0)
        m_groups[g].visible = visible;
    m_dynamicDirty = true;
}

bool TrackLayer::isGroupVisible(const QString &groupKey) const
{
    return !m_hidden.contains(groupKey);
}

void TrackLayer::setReplay(bool active, qint64 timeMs)
{
    m_replay = active;
    m_replayTime = timeMs;
    m_dynamicDirty = true;
}

void TrackLayer::setTrailSeconds(double seconds)
{
    m_trailSeconds = seconds;
    m_dynamicDirty = true;
}

void TrackLayer::setHoveredTrack(int index)
{
    if (index == m_hoverTrack)
        return;
    m_hoverTrack = index;
    m_dynamicDirty = true;
}

void TrackLayer::setIsolatedTrack(int index)
{
    if (index == m_isoTrack)
        return;
    m_isoTrack = index;
    m_dynamicDirty = true;
}

void TrackLayer::setTrackMask(const QVector<bool> &mask)
{
    if (mask == m_mask)
        return;
    m_mask = mask;
    m_rangesDirty = true;
    m_dynamicDirty = true;
}

// 每组：全部显示时直接画整批；否则把要显示的航迹片段按批次合并成尽量少的绘制调用
void TrackLayer::rebuildDrawRanges()
{
    m_rangesDirty = false;
    m_groupFull = QVector<bool>(m_groups.size(), true);
    m_groupPieces = QVector<QVector<Piece>>(m_groups.size());
    if (m_mask.isEmpty())
        return;
    for (int i = 0; i < m_trackGroup.size(); ++i)
        if (i >= m_mask.size() || !m_mask[i])
            m_groupFull[m_trackGroup[i]] = false;
    for (int i = 0; i < m_trackGroup.size(); ++i) {
        const int g = m_trackGroup[i];
        if (m_groupFull[g] || i >= m_mask.size() || !m_mask[i])
            continue;
        QVector<Piece> &out = m_groupPieces[g];
        for (const Piece &p : qAsConst(m_trackPieces[i])) {
            if (!out.isEmpty() && out.last().batch == p.batch && out.last().first + out.last().count == p.first)
                out.last().count += p.count;
            else
                out.append(p);
        }
    }
}

int TrackLayer::groupIndex(const QString &key) const
{
    for (int i = 0; i < m_groups.size(); ++i)
        if (m_groups[i].key == key)
            return i;
    return -1;
}

// 单独显示时只显示那一条（不管它所在的组是否隐藏），否则按组显隐
bool TrackLayer::trackShown(int index) const
{
    if (index < 0 || index >= m_trackGroup.size())
        return false;
    if (!m_mask.isEmpty() && (index >= m_mask.size() || !m_mask[index]))
        return false;
    if (m_isoTrack >= 0)
        return index == m_isoTrack;
    return m_groups[m_trackGroup[index]].visible;
}

// 数据变化时重建静态线段：按导入文件分组，组内按 kQuadsPerBatch 切批上传；
// 同时记下每条航迹落在哪些批次的哪一段，悬停和单独显示时直接重画这几段
void TrackLayer::rebuildStatic()
{
    releaseBatches();
    m_groups.clear();
    m_trackGroup.clear();
    m_builtVersion = m_store->version();
    m_builtStyleVersion = m_styleVersion;
    m_dynamicDirty = true;
    m_rangesDirty = true;
    const QVector<Track> &tracks = m_store->tracks();
    m_baseTime = m_store->minTime();
    m_trackGroup.resize(tracks.size());
    m_trackPieces = QVector<QVector<Piece>>(tracks.size());
    if (m_hoverTrack >= tracks.size())
        m_hoverTrack = -1;
    if (m_isoTrack >= tracks.size())
        m_isoTrack = -1;

    QVector<QVector<int>> members;
    for (int i = 0; i < tracks.size(); ++i) {
        const Track &t = tracks[i];
        const QString key = trackSourceName(t.source) + QStringLiteral("::") + t.fileName;
        int g = groupIndex(key);
        if (g < 0) {
            Group grp;
            grp.key = key;
            grp.color = m_sourceColor.contains(int(t.source)) ? m_sourceColor.value(int(t.source))
                                                          : m_store->fileColor(t.source, t.fileName);
            grp.alpha = t.source == TrackSource::RadarRaw ? 0.75f : 0.88f;
            const double base = t.source == TrackSource::RadarRaw ? 3.5 : 6.0;
            grp.radiusPx = float(base * m_dotScale.value(int(t.source), 1.0));
            grp.visible = !m_hidden.contains(key);
            g = m_groups.size();
            m_groups.append(grp);
            members.append(QVector<int>());
        }
        m_trackGroup[i] = g;
        members[g].append(i);
    }

    QVector<float> verts;
    int quads = 0;
    int group = 0;
    int curTrack = -1;
    int pieceFirst = 0;
    // 当前航迹在当前（尚未上传的）批次里的那一段
    auto closePiece = [&]() {
        if (curTrack >= 0 && quads > pieceFirst)
            m_trackPieces[curTrack].append(Piece{m_batches.size(), pieceFirst, quads - pieceFirst});
        pieceFirst = quads;
    };
    auto flush = [&]() {
        closePiece();
        if (quads == 0)
            return;
        Batch b;
        b.group = group;
        b.indexCount = quads * 6;
        m_gl->glGenBuffers(1, &b.vbo);
        m_gl->glBindBuffer(GL_ARRAY_BUFFER, b.vbo);
        m_gl->glBufferData(GL_ARRAY_BUFFER, verts.size() * int(sizeof(float)), verts.constData(), GL_STATIC_DRAW);
        m_batches.append(b);
        m_segments += quads;
        verts.clear();
        quads = 0;
        pieceFirst = 0;
    };
    verts.reserve(kQuadsPerBatch * 4 * kLineStride);
    for (group = 0; group < m_groups.size(); ++group) {
        for (int ti : qAsConst(members[group])) {
            curTrack = ti;
            pieceFirst = quads;
            const QVector<TrackPoint> &pts = tracks[ti].points;
            for (int j = 1; j < pts.size(); ++j) {
                const TrackPoint &a = pts[j - 1], &b = pts[j];
                if (a.lat == b.lat && a.lon == b.lon)
                    continue;   // 原地重复点：不生成退化线段（RadarView 的“鬼影”来源之一）
                // 细分出的每一小段都用终点时刻，回放时整段走完才出现，进行中的部分由头部段绘制；
                // 另带起点时刻，时间筛选时两端都在窗口内的线段才画（与 RadarView 裁剪点后连线一致）
                const float t = relTime(b.t);
                const float t0 = relTime(a.t);
                forEachPiece(a.lon, a.lat, b.lon, b.lat, [&](const QVector3D &p0, const QVector3D &p1) {
                    pushQuad(verts, p0, p1, t, t0);
                    if (++quads == kQuadsPerBatch)
                        flush();
                });
            }
            closePiece();
        }
        curTrack = -1;
        flush();
    }
    m_gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
}

float TrackLayer::relTime(qint64 ms) const
{
    return float((ms - m_baseTime) / 1000.0);
}

void TrackLayer::setTimeFilter(bool active, qint64 minMs, qint64 maxMs)
{
    if (active == m_filterOn && (!active || (minMs == m_filterMin && maxMs == m_filterMax)))
        return;
    m_filterOn = active;
    m_filterMin = minMs;
    m_filterMax = maxMs;
    m_rangeVersion = ~quint64(0);
    m_dynamicDirty = true;
    m_ptDotsDirty = true;
}

// 每条航迹在时间筛选窗口内的点下标范围 [lo, hi]；窗口内没有点时 lo > hi
void TrackLayer::ensureRanges() const
{
    if (!m_store || m_rangeVersion == m_store->version())
        return;
    m_rangeVersion = m_store->version();
    const QVector<Track> &tracks = m_store->tracks();
    m_lo = QVector<int>(tracks.size(), 0);
    m_hi = QVector<int>(tracks.size(), 0);
    for (int i = 0; i < tracks.size(); ++i) {
        const QVector<TrackPoint> &pts = tracks[i].points;
        if (!m_filterOn) {
            m_hi[i] = pts.size() - 1;
            continue;
        }
        const auto lo = std::lower_bound(pts.begin(), pts.end(), m_filterMin,
                                         [](const TrackPoint &p, qint64 v) { return p.t < v; });
        const auto hi = std::upper_bound(pts.begin(), pts.end(), m_filterMax,
                                         [](qint64 v, const TrackPoint &p) { return v < p.t; });
        m_lo[i] = int(lo - pts.begin());
        m_hi[i] = int(hi - pts.begin()) - 1;
    }
}

void TrackLayer::setPointDotTracks(const QVector<int> &indices)
{
    if (indices == m_ptDotTracks)
        return;
    m_ptDotTracks = indices;
    m_ptDotsDirty = true;
}

// RadarView CesiumMap.vue contrastColor：色相转半圈，饱和度至少 0.75，亮度 0.55
QColor TrackLayer::contrastColor(const QColor &c)
{
    const QColor hsl = c.toHsl();
    double h = hsl.hslHueF(), s = hsl.hslSaturationF();
    if (h < 0.0)
        h = 0.0;
    h = std::fmod(h + 0.5, 1.0);
    s = qMax(s, 0.75);
    return QColor::fromHslF(h, s, 0.55);
}

// 点迹：要显示点迹的航迹，把（时间窗口内的）每个原始点各画成一个小圆点
void TrackLayer::rebuildPointDots()
{
    m_ptDotsDirty = false;
    m_ptDots.clear();
    m_ptDotsBuiltVersion = m_store->version();
    ensureRanges();
    const QVector<Track> &tracks = m_store->tracks();
    QVector<float> v;
    for (int ti : qAsConst(m_ptDotTracks)) {
        if (ti < 0 || ti >= tracks.size() || m_lo[ti] > m_hi[ti])
            continue;
        DotTrack d;
        d.track = ti;
        d.first = v.size() / (4 * kDotStride);
        const QVector<TrackPoint> &pts = tracks[ti].points;
        for (int k = m_lo[ti]; k <= m_hi[ti]; ++k) {
            pushDot(v, geo::ecefDeg(pts[k].lon, pts[k].lat, kAltitudeM));
            d.t.append(pts[k].t);
            d.pos.append(geo::ecefDeg(pts[k].lon, pts[k].lat, kAltitudeM));
        }
        d.count = d.t.size();
        d.color = contrastColor(m_groups[m_trackGroup[ti]].color);
        m_ptDots.append(d);
    }
    m_gl->glBindBuffer(GL_ARRAY_BUFFER, m_ptDotVbo);
    m_gl->glBufferData(GL_ARRAY_BUFFER, v.size() * int(sizeof(float)), v.constData(), GL_STATIC_DRAW);
    m_gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// 回放时只显示已经经过的点
int TrackLayer::dotShownCount(const DotTrack &d) const
{
    if (!m_replay)
        return d.count;
    return int(std::upper_bound(d.t.begin(), d.t.end(), m_replayTime) - d.t.begin());
}

// 每帧（回放中）或显示状态变化时重建：头部插值段 + 端点
void TrackLayer::rebuildDynamic()
{
    m_dynamicDirty = false;
    ensureRanges();
    const QVector<Track> &tracks = m_store->tracks();
    QVector<float> heads, dots;
    m_headRanges = QVector<GroupRange>(m_groups.size());
    m_dotRanges = QVector<GroupRange>(m_groups.size());
    m_hoverHead = GroupRange();
    m_hoverDot = -1;
    m_visibleTracks = 0;
    m_endpoints.clear();
    const float nowRel = relTime(m_replayTime);

    for (int g = 0; g < m_groups.size(); ++g) {
        m_headRanges[g].first = heads.size() / (4 * kLineStride);
        m_dotRanges[g].first = dots.size() / (4 * kDotStride);
        for (int i = 0; i < tracks.size(); ++i) {
            if (m_trackGroup[i] != g || !trackShown(i) || m_lo[i] > m_hi[i])
                continue;
            const QVector<TrackPoint> &pts = tracks[i].points;
            double lon = 0.0, lat = 0.0;
            int last = 0;
            if (!m_replay) {
                lon = pts[m_hi[i]].lon;
                lat = pts[m_hi[i]].lat;
            } else if (!replayPosition(pts, m_lo[i], m_hi[i], m_replayTime, &lon, &lat, &last)) {
                continue;   // 还没开始
            } else {
                const int headFirst = heads.size() / (4 * kLineStride);
                const TrackPoint &a = pts[last];
                const float t0 = relTime(a.t);
                if (lon != a.lon || lat != a.lat)
                    forEachPiece(a.lon, a.lat, lon, lat, [&](const QVector3D &p0, const QVector3D &p1) {
                        pushQuad(heads, p0, p1, nowRel, t0);
                    });
                if (i == m_hoverTrack)
                    m_hoverHead = GroupRange{headFirst, heads.size() / (4 * kLineStride) - headFirst};
            }
            if (i == m_hoverTrack)
                m_hoverDot = dots.size() / (4 * kDotStride);
            pushDot(dots, geo::ecefDeg(lon, lat, kAltitudeM));
            m_endpoints.append(EndPoint(i, lon, lat));
            ++m_visibleTracks;
        }
        m_headRanges[g].count = heads.size() / (4 * kLineStride) - m_headRanges[g].first;
        m_dotRanges[g].count = dots.size() / (4 * kDotStride) - m_dotRanges[g].first;
    }

    m_gl->glBindBuffer(GL_ARRAY_BUFFER, m_headVbo);
    m_gl->glBufferData(GL_ARRAY_BUFFER, heads.size() * int(sizeof(float)), heads.constData(), GL_STREAM_DRAW);
    m_gl->glBindBuffer(GL_ARRAY_BUFFER, m_dotVbo);
    m_gl->glBufferData(GL_ARRAY_BUFFER, dots.size() * int(sizeof(float)), dots.constData(), GL_STREAM_DRAW);
    m_gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void TrackLayer::draw(const QMatrix4x4 &mvp, const QSize &viewportPx, float dpr)
{
    m_hoverDrawn = false;
    if (!isReady() || !m_store)
        return;
    if (m_store->version() != m_builtVersion || m_styleVersion != m_builtStyleVersion)
        rebuildStatic();
    if (m_groups.isEmpty())
        return;
    if (m_dynamicDirty || m_replay)
        rebuildDynamic();
    if (m_rangesDirty)
        rebuildDrawRanges();
    if (m_ptDotsDirty || m_ptDotsBuiltVersion != m_store->version())
        rebuildPointDots();

    QOpenGLFunctions *gl = m_gl;
    const QVector2D vp(float(qMax(1, viewportPx.width())), float(qMax(1, viewportPx.height())));
    const float hw = m_lineWidthPx * 0.5f * dpr;
    const float hoverHw = kHoverHalfWidthPx * dpr;
    const bool hover = trackShown(m_hoverTrack);

    gl->glEnable(GL_BLEND);
    gl->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl->glDepthMask(GL_FALSE);
    gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_quadIbo);

    // ---- 线：静态批次 + 回放头部段 ----
    m_lineProg->bind();
    m_lineProg->setUniformValue(m_uLineMvp, mvp);
    m_lineProg->setUniformValue(m_uLineViewport, vp);
    m_lineProg->setUniformValue(m_uLineHalfWidth, QVector2D(hw, hw + 1.0f));
    const float nowRel = float((m_replayTime - m_baseTime) / 1000.0);
    QVector2D window(-1e30f, 1e30f);
    if (m_replay)
        window = QVector2D(m_trailSeconds > 0.0 ? float(nowRel - m_trailSeconds) : -1e30f, nowRel);
    const QVector2D allTime(-1e30f, 1e30f);
    m_lineProg->setUniformValue(m_uLineWindow, window);
    // 时间筛选窗口；回放头部段的起点是已经过的点，必在窗口内，结束时刻是当前时刻，也不超出（回放范围跟随筛选结果）
    m_lineProg->setUniformValue(m_uLineFilter, m_filterOn ? QVector2D(relTime(m_filterMin) - 0.001f, relTime(m_filterMax) + 0.001f)
                                                          : allTime);
    for (int a = 0; a < 3; ++a)
        gl->glEnableVertexAttribArray(GLuint(a));

    auto setLineAttribs = [gl](qintptr byteOffset) {
        const GLsizei stride = kLineStride * sizeof(float);
        gl->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void *>(byteOffset));
        gl->glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void *>(byteOffset + 3 * sizeof(float)));
        gl->glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void *>(byteOffset + 6 * sizeof(float)));
    };
    auto groupColor = [this](int g) {
        const Group &grp = m_groups[g];
        return QVector4D(float(grp.color.redF()), float(grp.color.greenF()), float(grp.color.blueF()), grp.alpha);
    };
    // 某条航迹的静态线段：属性指针移到它在批次里的起点，共用同一份索引
    auto drawTrackPieces = [&](int ti) {
        for (const Piece &p : m_trackPieces[ti]) {
            gl->glBindBuffer(GL_ARRAY_BUFFER, m_batches[p.batch].vbo);
            setLineAttribs(qintptr(p.first) * 4 * kLineStride * qintptr(sizeof(float)));
            gl->glDrawElements(GL_TRIANGLES, p.count * 6, GL_UNSIGNED_SHORT, nullptr);
        }
    };
    if (m_isoTrack >= 0) {
        if (trackShown(m_isoTrack)) {
            m_lineProg->setUniformValue(m_uLineColor, groupColor(m_trackGroup[m_isoTrack]));
            drawTrackPieces(m_isoTrack);
        }
    } else {
        for (const Batch &b : qAsConst(m_batches)) {
            if (!m_groups[b.group].visible || !m_groupFull[b.group])
                continue;
            m_lineProg->setUniformValue(m_uLineColor, groupColor(b.group));
            gl->glBindBuffer(GL_ARRAY_BUFFER, b.vbo);
            setLineAttribs(0);
            gl->glDrawElements(GL_TRIANGLES, b.indexCount, GL_UNSIGNED_SHORT, nullptr);
        }
        for (int g = 0; g < m_groups.size(); ++g) {
            if (!m_groups[g].visible || m_groupFull[g] || m_groupPieces[g].isEmpty())
                continue;
            m_lineProg->setUniformValue(m_uLineColor, groupColor(g));
            for (const Piece &p : qAsConst(m_groupPieces[g])) {
                gl->glBindBuffer(GL_ARRAY_BUFFER, m_batches[p.batch].vbo);
                setLineAttribs(qintptr(p.first) * 4 * kLineStride * qintptr(sizeof(float)));
                gl->glDrawElements(GL_TRIANGLES, p.count * 6, GL_UNSIGNED_SHORT, nullptr);
            }
        }
    }
    // 动态缓冲按组连续存放（只含要显示的航迹）；超过一批的部分移动属性指针偏移继续画
    auto drawRange = [&](const GroupRange &r, int stride, const std::function<void(qintptr)> &setAttribs) {
        for (int done = 0; done < r.count; done += kQuadsPerBatch) {
            const int n = qMin(kQuadsPerBatch, r.count - done);
            setAttribs(qintptr(r.first + done) * 4 * stride * qintptr(sizeof(float)));
            gl->glDrawElements(GL_TRIANGLES, n * 6, GL_UNSIGNED_SHORT, nullptr);
        }
    };
    auto drawRanges = [&](GLuint vbo, const QVector<GroupRange> &ranges, int stride,
                          const std::function<void(qintptr)> &setAttribs,
                          const std::function<void(int)> &setGroup) {
        gl->glBindBuffer(GL_ARRAY_BUFFER, vbo);
        for (int g = 0; g < ranges.size(); ++g) {
            if (ranges[g].count == 0)
                continue;
            setGroup(g);
            drawRange(ranges[g], stride, setAttribs);
        }
    };
    if (m_replay) {
        m_lineProg->setUniformValue(m_uLineWindow, allTime);
        drawRanges(m_headVbo, m_headRanges, kLineStride, setLineAttribs,
                   [&](int g) { m_lineProg->setUniformValue(m_uLineColor, groupColor(g)); });
    }
    // 悬停：同一条航迹用红色宽线再画一遍，压在其他航迹上面
    if (hover) {
        m_lineProg->setUniformValue(m_uLineHalfWidth, QVector2D(hoverHw, hoverHw + 1.0f));
        m_lineProg->setUniformValue(m_uLineColor, kHoverColor);
        m_lineProg->setUniformValue(m_uLineWindow, window);
        drawTrackPieces(m_hoverTrack);
        if (m_replay && m_hoverHead.count > 0) {
            m_lineProg->setUniformValue(m_uLineWindow, allTime);
            gl->glBindBuffer(GL_ARRAY_BUFFER, m_headVbo);
            drawRange(m_hoverHead, kLineStride, setLineAttribs);
        }
        m_hoverDrawn = true;
    }
    for (int a = 0; a < 3; ++a)
        gl->glDisableVertexAttribArray(GLuint(a));
    m_lineProg->release();

    // ---- 端点 ----
    m_dotProg->bind();
    m_dotProg->setUniformValue(m_uDotMvp, mvp);
    m_dotProg->setUniformValue(m_uDotViewport, vp);
    gl->glEnableVertexAttribArray(0);
    gl->glEnableVertexAttribArray(1);
    auto setDotAttribs = [gl](qintptr off) {
        const GLsizei stride = kDotStride * sizeof(float);
        gl->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void *>(off));
        gl->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void *>(off + 3 * sizeof(float)));
    };
    drawRanges(m_dotVbo, m_dotRanges, kDotStride, setDotAttribs,
               [&](int g) {
                   m_dotProg->setUniformValue(m_uDotColor, QVector4D(groupColor(g).toVector3D(), 1.0f));
                   m_dotProg->setUniformValue(m_uDotRadius, m_groups[g].radiusPx * dpr);
               });
    if (hover && m_hoverDot >= 0) {
        m_dotProg->setUniformValue(m_uDotColor, kHoverColor);
        m_dotProg->setUniformValue(m_uDotRadius, m_groups[m_trackGroup[m_hoverTrack]].radiusPx * kHoverDotScale * dpr);
        drawRange(GroupRange{m_hoverDot, 1}, kDotStride, setDotAttribs);
    }
    // 点迹：直径 m_ptDotPx（RadarView pixelSize 是直径），黑色描边
    m_ptDotsDrawn = 0;
    if (!m_ptDots.isEmpty()) {
        gl->glBindBuffer(GL_ARRAY_BUFFER, m_ptDotVbo);
        m_dotProg->setUniformValue(m_uDotRadius, m_ptDotPx * 0.5f * dpr + 1.0f);
        for (const DotTrack &d : qAsConst(m_ptDots)) {
            if (!trackShown(d.track))
                continue;
            const int n = dotShownCount(d);
            if (n <= 0)
                continue;
            m_dotProg->setUniformValue(m_uDotColor, QVector4D(float(d.color.redF()), float(d.color.greenF()),
                                                              float(d.color.blueF()), 1.0f));
            drawRange(GroupRange{d.first, n}, kDotStride, setDotAttribs);
            m_ptDotsDrawn += n;
        }
    }
    gl->glDisableVertexAttribArray(0);
    gl->glDisableVertexAttribArray(1);
    m_dotProg->release();

    gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
    gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    gl->glDepthMask(GL_TRUE);
    gl->glDisable(GL_BLEND);
}

// ---------------------------------------------------------------
//  拾取（纯 CPU）
// ---------------------------------------------------------------
// 与静态线段完全相同的折线（含细分点），每个顶点记下它所在线段的结束时刻
void TrackLayer::ensurePickCache() const
{
    if (m_pickVersion == m_store->version())
        return;
    m_pickVersion = m_store->version();
    const QVector<Track> &tracks = m_store->tracks();
    m_pick = QVector<PickTrack>(tracks.size());
    for (int i = 0; i < tracks.size(); ++i) {
        const QVector<TrackPoint> &pts = tracks[i].points;
        PickTrack &pk = m_pick[i];
        pk.pos.reserve(pts.size());
        pk.t.reserve(pts.size());
        pk.pos.append(geo::ecefDeg(pts[0].lon, pts[0].lat, kAltitudeM));
        pk.t.append(pts[0].t);
        pk.t0.append(pts[0].t);
        for (int j = 1; j < pts.size(); ++j) {
            const TrackPoint &a = pts[j - 1], &b = pts[j];
            if (a.lat == b.lat && a.lon == b.lon)
                continue;
            forEachPiece(a.lon, a.lat, b.lon, b.lat, [&](const QVector3D &, const QVector3D &p1) {
                pk.pos.append(p1);
                pk.t.append(b.t);
                pk.t0.append(a.t);
            });
        }
        QVector3D lo = pk.pos[0], hi = pk.pos[0];
        for (const QVector3D &p : qAsConst(pk.pos)) {
            lo = QVector3D(qMin(lo.x(), p.x()), qMin(lo.y(), p.y()), qMin(lo.z(), p.z()));
            hi = QVector3D(qMax(hi.x(), p.x()), qMax(hi.y(), p.y()), qMax(hi.z(), p.z()));
        }
        pk.bmin = lo;
        pk.bmax = hi;
    }
}

int TrackLayer::pick(const QMatrix4x4 &mvp, const QVector3D &eye, const QSizeF &viewport,
                     const QPointF &pos, float tolPx) const
{
    // 图层还没按最新数据重建过（尚未绘制）时不拾取，避免下标与分组对不上
    if (!m_store || m_builtVersion != m_store->version() || m_groups.isEmpty())
        return -1;
    ensurePickCache();
    ensureRanges();
    const QVector<Track> &tracks = m_store->tracks();
    const float w = float(qMax(1.0, viewport.width())), h = float(qMax(1.0, viewport.height()));
    const QVector2D mouse(float(pos.x()), float(pos.y()));

    const QVector4D r0 = mvp.row(0), r1 = mvp.row(1), r2 = mvp.row(2), r3 = mvp.row(3);
    QVector4D planes[6] = { r3 + r0, r3 - r0, r3 + r1, r3 - r1, r3 + r2, r3 - r2 };
    for (QVector4D &p : planes)
        p /= p.toVector3D().length();

    qint64 winLo = std::numeric_limits<qint64>::min(), winHi = std::numeric_limits<qint64>::max();
    if (m_replay) {
        winHi = m_replayTime;
        if (m_trailSeconds > 0.0)
            winLo = m_replayTime - qint64(m_trailSeconds * 1000.0);
    }

    // 屏幕坐标；在地球背面（被椭球挡住）或相机后方返回 false
    auto project = [&](const QVector3D &p, QVector2D *s) {
        if (QVector3D::dotProduct(p, eye - p) < 0.0f)
            return false;
        const QVector4D c = mvp * QVector4D(p, 1.0f);
        if (c.w() < 1e-6f)
            return false;
        *s = QVector2D((c.x() / c.w() + 1.0f) * 0.5f * w, (1.0f - c.y() / c.w()) * 0.5f * h);
        return true;
    };
    auto segDist = [&mouse](const QVector2D &a, const QVector2D &b) {
        const QVector2D ab = b - a;
        const float l2 = QVector2D::dotProduct(ab, ab);
        const float f = l2 > 0.0f ? qBound(0.0f, QVector2D::dotProduct(mouse - a, ab) / l2, 1.0f) : 0.0f;
        return (a + ab * f - mouse).length();
    };

    int best = -1;
    float bestScore = tolPx;
    for (int i = 0; i < m_pick.size(); ++i) {
        if (!trackShown(i))
            continue;
        const PickTrack &pk = m_pick[i];
        const QVector3D c = (pk.bmin + pk.bmax) * 0.5f;
        const float rad = (pk.bmax - pk.bmin).length() * 0.5f + 1e-4f;
        bool outside = false;
        for (const QVector4D &p : planes)
            if (QVector3D::dotProduct(p.toVector3D(), c) + p.w() < -rad) {
                outside = true;
                break;
            }
        if (outside)
            continue;

        // 线段：回放时只算已经走过（且在拖尾窗口内）的部分，与着色器裁剪一致
        QVector2D prev;
        bool prevOk = project(pk.pos[0], &prev);
        for (int k = 1; k < pk.pos.size(); ++k) {
            if (pk.t[k] > winHi)
                break;
            QVector2D cur;
            const bool curOk = project(pk.pos[k], &cur);
            const bool inFilter = !m_filterOn || (pk.t0[k] >= m_filterMin && pk.t[k] <= m_filterMax);
            if (prevOk && curOk && pk.t[k] >= winLo && inFilter) {
                const float d = segDist(prev, cur);
                if (d < bestScore) {
                    bestScore = d;
                    best = i;
                }
            }
            prev = cur;
            prevOk = curOk;
        }

        // 端点（回放时为插值位置）和回放头部段；端点按圆点半径算命中
        const QVector<TrackPoint> &pts = tracks[i].points;
        if (m_lo[i] > m_hi[i])
            continue;
        double lon = pts[m_hi[i]].lon, lat = pts[m_hi[i]].lat;
        int last = m_hi[i];
        if (m_replay && !replayPosition(pts, m_lo[i], m_hi[i], m_replayTime, &lon, &lat, &last))
            continue;
        QVector2D dot;
        if (!project(geo::ecefDeg(lon, lat, kAltitudeM), &dot))
            continue;
        const float radius = m_groups[m_trackGroup[i]].radiusPx;
        const float dd = qMax(0.0f, (dot - mouse).length() - qMax(0.0f, radius - tolPx));
        if (dd < bestScore) {
            bestScore = dd;
            best = i;
        }
        QVector2D a;
        if (m_replay && project(geo::ecefDeg(pts[last].lon, pts[last].lat, kAltitudeM), &a)) {
            const float d = segDist(a, dot);
            if (d < bestScore) {
                bestScore = d;
                best = i;
            }
        }
    }
    return best;
}

bool TrackLayer::isTrackShownIgnoringGroups(int index) const
{
    if (!m_store || index < 0 || index >= m_store->size())
        return false;
    if (!m_mask.isEmpty() && (index >= m_mask.size() || !m_mask[index]))
        return false;
    return m_isoTrack < 0 || index == m_isoTrack;
}

int TrackLayer::pointDotCount() const
{
    return m_ptDotsDrawn;
}

QColor TrackLayer::trackColor(int index) const
{
    if (index < 0 || index >= m_trackGroup.size())
        return QColor(Qt::white);
    return m_groups[m_trackGroup[index]].color;
}

// RadarView pointDotRenderer.ts checkPointDotHit：屏幕距离阈值 max(点径 + 30, 60)
bool TrackLayer::hasPointDots(int track) const
{
    return m_ptDotTracks.contains(track);
}

bool TrackLayer::pickPointDot(const QMatrix4x4 &mvp, const QVector3D &eye, const QSizeF &viewport,
                              const QPointF &pos, int track, int *pointIndex) const
{
    if (!m_store || m_ptDotsBuiltVersion != m_store->version())
        return false;
    const float w = float(qMax(1.0, viewport.width())), h = float(qMax(1.0, viewport.height()));
    const float threshold = qMax(m_ptDotPx + 30.0f, 60.0f);
    float best = threshold * threshold;
    bool found = false;
    for (const DotTrack &d : m_ptDots) {
        if (d.track != track || !trackShown(d.track))
            continue;
        const int n = dotShownCount(d);
        for (int k = 0; k < n; ++k) {
            const QVector3D &p = d.pos[k];
            if (QVector3D::dotProduct(p, eye - p) < 0.0f)
                continue;
            const QVector4D c = mvp * QVector4D(p, 1.0f);
            if (c.w() < 1e-6f)
                continue;
            const float sx = (c.x() / c.w() + 1.0f) * 0.5f * w, sy = (1.0f - c.y() / c.w()) * 0.5f * h;
            const float dx = sx - float(pos.x()), dy = sy - float(pos.y());
            const float d2 = dx * dx + dy * dy;
            if (d2 < best) {
                best = d2;
                *pointIndex = m_lo.value(d.track) + k;
                found = true;
            }
        }
    }
    return found;
}
