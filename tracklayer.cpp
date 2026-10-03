#include "tracklayer.h"
#include "track.h"
#include "geo.h"

#include <QOpenGLShaderProgram>
#include <algorithm>
#include <cmath>

const double TrackLayer::kAltitudeM = 10000.0;

namespace {

const int kQuadsPerBatch = 16384;            // 16384 * 4 = 65536 个顶点，正好用满 16 位索引
const int kLineStride = 9;                   // p0(3) p1(3) side end time
const int kDotStride = 5;                    // pos(3) corner(2)
const double kMaxPieceDeg = 0.5;             // 长线段按此角度细分，避免弦线钻进地面
const float kLineHalfWidthPx = 1.0f;         // 线宽 2px

// 线段四边形：每个顶点都带两个端点，着色器在屏幕空间算法线并沿 side 方向展开
const char *const kLineVs = R"(
attribute vec3 a_p0;
attribute vec3 a_p1;
attribute vec3 a_param;         // x: 侧向 ±1, y: 0 起点 / 1 终点, z: 线段结束时刻（秒）
uniform mat4 u_mvp;
uniform vec2 u_viewport;        // 视口像素尺寸
uniform vec2 u_halfWidth;       // x: 线宽一半, y: 含抗锯齿余量
uniform vec2 u_window;          // 显示的时间窗口
varying float v_edge;
varying float v_hw;             // 线宽一半，经 varying 传给片元（uniform 两段精度不同时 ES2 链接会失败）
void main()
{
    v_hw = u_halfWidth.x;
    vec4 c0 = u_mvp * vec4(a_p0, 1.0);
    vec4 c1 = u_mvp * vec4(a_p1, 1.0);
    v_edge = 0.0;
    if (a_param.z < u_window.x || a_param.z > u_window.y || c0.w < 1e-6 || c1.w < 1e-6) {
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

void pushQuad(QVector<float> &v, const QVector3D &a, const QVector3D &b, float t)
{
    for (int k = 0; k < 4; ++k) {
        v << a.x() << a.y() << a.z() << b.x() << b.y() << b.z()
          << ((k & 1) ? -1.0f : 1.0f) << (k >= 2 ? 1.0f : 0.0f) << t;
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
    m_builtVersion = ~quint64(0);
    m_dynamicDirty = true;
    return true;
}

void TrackLayer::cleanup()
{
    if (!m_gl)
        return;
    releaseBatches();
    GLuint bufs[3] = { m_quadIbo, m_headVbo, m_dotVbo };
    m_gl->glDeleteBuffers(3, bufs);
    m_quadIbo = m_headVbo = m_dotVbo = 0;
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
    m_segments = 0;
}

void TrackLayer::setStore(const TrackStore *store)
{
    m_store = store;
    m_builtVersion = ~quint64(0);
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

int TrackLayer::groupIndex(const QString &key) const
{
    for (int i = 0; i < m_groups.size(); ++i)
        if (m_groups[i].key == key)
            return i;
    return -1;
}

// 数据变化时重建静态线段：按导入文件分组，组内按 kQuadsPerBatch 切批上传
void TrackLayer::rebuildStatic()
{
    releaseBatches();
    m_groups.clear();
    m_trackGroup.clear();
    m_builtVersion = m_store->version();
    m_dynamicDirty = true;
    const QVector<Track> &tracks = m_store->tracks();
    m_baseTime = m_store->minTime();
    m_trackGroup.resize(tracks.size());

    QVector<QVector<int>> members;
    for (int i = 0; i < tracks.size(); ++i) {
        const Track &t = tracks[i];
        const QString key = trackSourceName(t.source) + QStringLiteral("::") + t.fileName;
        int g = groupIndex(key);
        if (g < 0) {
            Group grp;
            grp.key = key;
            grp.color = m_store->fileColor(t.source, t.fileName);
            grp.alpha = t.source == TrackSource::RadarRaw ? 0.75f : 0.88f;
            grp.radiusPx = t.source == TrackSource::RadarRaw ? 3.5f : 6.0f;
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
    auto flush = [&]() {
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
    };
    verts.reserve(kQuadsPerBatch * 4 * kLineStride);
    for (group = 0; group < m_groups.size(); ++group) {
        for (int ti : qAsConst(members[group])) {
            const QVector<TrackPoint> &pts = tracks[ti].points;
            for (int j = 1; j < pts.size(); ++j) {
                const TrackPoint &a = pts[j - 1], &b = pts[j];
                if (a.lat == b.lat && a.lon == b.lon)
                    continue;   // 原地重复点：不生成退化线段（RadarView 的“鬼影”来源之一）
                // 细分出的每一小段都用终点时刻，回放时整段走完才出现，进行中的部分由头部段绘制
                const float t = float((b.t - m_baseTime) / 1000.0);
                forEachPiece(a.lon, a.lat, b.lon, b.lat, [&](const QVector3D &p0, const QVector3D &p1) {
                    pushQuad(verts, p0, p1, t);
                    if (++quads == kQuadsPerBatch)
                        flush();
                });
            }
        }
        flush();
    }
    m_gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// 每帧（回放中）或显示状态变化时重建：头部插值段 + 端点
void TrackLayer::rebuildDynamic()
{
    m_dynamicDirty = false;
    const QVector<Track> &tracks = m_store->tracks();
    QVector<float> heads, dots;
    m_headRanges = QVector<GroupRange>(m_groups.size());
    m_dotRanges = QVector<GroupRange>(m_groups.size());
    m_visibleTracks = 0;
    const float nowRel = float((m_replayTime - m_baseTime) / 1000.0);

    for (int g = 0; g < m_groups.size(); ++g) {
        m_headRanges[g].first = heads.size() / (4 * kLineStride);
        m_dotRanges[g].first = dots.size() / (4 * kDotStride);
        if (m_groups[g].visible) {
            for (int i = 0; i < tracks.size(); ++i) {
                if (m_trackGroup[i] != g)
                    continue;
                const QVector<TrackPoint> &pts = tracks[i].points;
                if (!m_replay) {
                    const TrackPoint &last = pts.last();
                    pushDot(dots, geo::ecefDeg(last.lon, last.lat, kAltitudeM));
                    ++m_visibleTracks;
                    continue;
                }
                auto hi = std::upper_bound(pts.begin(), pts.end(), m_replayTime,
                                           [](qint64 t, const TrackPoint &p) { return t < p.t; });
                if (hi == pts.begin())
                    continue;   // 还没开始
                const TrackPoint &a = *(hi - 1);
                double lon = a.lon, lat = a.lat;
                if (hi != pts.end()) {
                    const TrackPoint &b = *hi;
                    const double f = b.t > a.t ? double(m_replayTime - a.t) / double(b.t - a.t) : 0.0;
                    lon = a.lon + wrapLon(b.lon - a.lon) * f;
                    lat = a.lat + (b.lat - a.lat) * f;
                    if (lon > 180.0) lon -= 360.0;
                    if (lon < -180.0) lon += 360.0;
                    if (f > 0.0)
                        forEachPiece(a.lon, a.lat, lon, lat, [&](const QVector3D &p0, const QVector3D &p1) {
                            pushQuad(heads, p0, p1, nowRel);
                        });
                }
                pushDot(dots, geo::ecefDeg(lon, lat, kAltitudeM));
                ++m_visibleTracks;
            }
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
    if (!isReady() || !m_store)
        return;
    if (m_store->version() != m_builtVersion)
        rebuildStatic();
    if (m_groups.isEmpty())
        return;
    if (m_dynamicDirty || m_replay)
        rebuildDynamic();

    QOpenGLFunctions *gl = m_gl;
    const QVector2D vp(float(qMax(1, viewportPx.width())), float(qMax(1, viewportPx.height())));
    const float hw = kLineHalfWidthPx * dpr;

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
    m_lineProg->setUniformValue(m_uLineWindow, window);
    for (int a = 0; a < 3; ++a)
        gl->glEnableVertexAttribArray(GLuint(a));

    auto setLineAttribs = [gl](qintptr byteOffset) {
        const GLsizei stride = kLineStride * sizeof(float);
        gl->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void *>(byteOffset));
        gl->glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void *>(byteOffset + 3 * sizeof(float)));
        gl->glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void *>(byteOffset + 6 * sizeof(float)));
    };
    auto groupColor = [this](int g) {
        const Group &grp = m_groups[g];
        return QVector4D(float(grp.color.redF()), float(grp.color.greenF()), float(grp.color.blueF()), grp.alpha);
    };
    for (const Batch &b : qAsConst(m_batches)) {
        if (!m_groups[b.group].visible)
            continue;
        m_lineProg->setUniformValue(m_uLineColor, groupColor(b.group));
        gl->glBindBuffer(GL_ARRAY_BUFFER, b.vbo);
        setLineAttribs(0);
        gl->glDrawElements(GL_TRIANGLES, b.indexCount, GL_UNSIGNED_SHORT, nullptr);
    }
    // 动态缓冲按组连续存放；超过一批的部分移动属性指针偏移继续画（共用同一份索引）
    auto drawRanges = [&](GLuint vbo, const QVector<GroupRange> &ranges, int stride,
                          const std::function<void(qintptr)> &setAttribs,
                          const std::function<void(int)> &setGroup) {
        gl->glBindBuffer(GL_ARRAY_BUFFER, vbo);
        for (int g = 0; g < ranges.size(); ++g) {
            if (!m_groups[g].visible || ranges[g].count == 0)
                continue;
            setGroup(g);
            for (int done = 0; done < ranges[g].count; done += kQuadsPerBatch) {
                const int n = qMin(kQuadsPerBatch, ranges[g].count - done);
                setAttribs(qintptr(ranges[g].first + done) * 4 * stride * qintptr(sizeof(float)));
                gl->glDrawElements(GL_TRIANGLES, n * 6, GL_UNSIGNED_SHORT, nullptr);
            }
        }
    };
    if (m_replay) {
        m_lineProg->setUniformValue(m_uLineWindow, QVector2D(-1e30f, 1e30f));
        drawRanges(m_headVbo, m_headRanges, kLineStride, setLineAttribs,
                   [&](int g) { m_lineProg->setUniformValue(m_uLineColor, groupColor(g)); });
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
    drawRanges(m_dotVbo, m_dotRanges, kDotStride,
               [gl](qintptr off) {
                   const GLsizei stride = kDotStride * sizeof(float);
                   gl->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void *>(off));
                   gl->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void *>(off + 3 * sizeof(float)));
               },
               [&](int g) {
                   m_dotProg->setUniformValue(m_uDotColor, QVector4D(groupColor(g).toVector3D(), 1.0f));
                   m_dotProg->setUniformValue(m_uDotRadius, m_groups[g].radiusPx * dpr);
               });
    gl->glDisableVertexAttribArray(0);
    gl->glDisableVertexAttribArray(1);
    m_dotProg->release();

    gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
    gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    gl->glDepthMask(GL_TRUE);
    gl->glDisable(GL_BLEND);
}
