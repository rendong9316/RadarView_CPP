#include "globesurface.h"
#include "netmap.h"
#include "tilesource.h"

#include <QOpenGLContext>
#include <QOpenGLShaderProgram>
#include <QPair>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace {

const int kGridN = 16;          // 每块瓦片的网格细分数
const int kLoadPerFrame = 32;   // 每帧最多解码的瓦片数，避免卡顿
const int kMaxTextures = 512;   // 纹理 LRU 上限；一屏约 300 张，留一倍余量让回看不重解码

// 椭球长半轴归一化为 1；顶点位置在着色器里由经纬度算出，所有瓦片共用一套网格
const char *const kVertexSrc = R"(
attribute vec3 a_uv;            // xy: 瓦片内坐标 0..1，z: 1 表示裙边顶点
uniform mat4 u_mvp;
uniform vec3 u_eye;
uniform vec4 u_range;           // 经度起止，纬度起止（墨卡托 y 或纬度）
uniform float u_linearLat;      // 1: u_range.zw 是纬度；0: 是墨卡托 y
uniform float u_skirt;
uniform vec4 u_texXform;        // 纹理坐标偏移与缩放（使用父级瓦片时）
varying vec2 v_tex;
varying float v_light;
void main()
{
    float lon = mix(u_range.x, u_range.y, a_uv.x);
    float t = mix(u_range.z, u_range.w, a_uv.y);
    float lat = u_linearLat > 0.5 ? t : 2.0 * atan(exp(t)) - 1.5707963;
    float sl = sin(lat);
    float cl = cos(lat);
    float n = 1.0 / sqrt(1.0 - 0.00669438 * sl * sl);
    vec3 normal = vec3(cl * cos(lon), cl * sin(lon), sl);
    vec3 pos = vec3(n * cl * cos(lon), n * cl * sin(lon), n * 0.99330562 * sl);
    pos *= 1.0 - a_uv.z * u_skirt;
    v_tex = u_texXform.xy + a_uv.xy * u_texXform.zw;
    v_light = 0.55 + 0.45 * max(dot(normal, normalize(u_eye - pos)), 0.0);
    gl_Position = u_mvp * vec4(pos, 1.0);
}
)";

const char *const kFragmentSrc = R"(
#ifdef GL_ES
precision mediump float;
#endif
uniform sampler2D u_tex;
uniform float u_useTex;
uniform vec4 u_color;
varying vec2 v_tex;
varying float v_light;
void main()
{
    vec4 c = u_useTex > 0.5 ? texture2D(u_tex, v_tex) : u_color;
    gl_FragColor = vec4(c.rgb * v_light, 1.0);
}
)";

} // namespace

GlobeSurface::GlobeSurface()
    : m_northCap(0.80f, 0.85f, 0.90f, 1.0f),
      m_southCap(0.92f, 0.94f, 0.96f, 1.0f),
      m_baseColor(0.16f, 0.26f, 0.38f, 1.0f)
{
}

GlobeSurface::~GlobeSurface()
{
    delete m_prog;
    m_prog = nullptr;
}

bool GlobeSurface::initialize(QOpenGLFunctions *gl, QOpenGLContext *ctx)
{
    m_gl = gl;
    m_glInfo = QStringLiteral("%1 | %2%3")
                   .arg(QString::fromLatin1(reinterpret_cast<const char *>(m_gl->glGetString(GL_RENDERER))))
                   .arg(QString::fromLatin1(reinterpret_cast<const char *>(m_gl->glGetString(GL_VERSION))))
                   .arg(ctx && ctx->isOpenGLES() ? QStringLiteral(" (ES)") : QString());

    m_prog = new QOpenGLShaderProgram;
    bool ok = m_prog->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexSrc)
           && m_prog->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentSrc);
    if (ok) {
        m_prog->bindAttributeLocation("a_uv", 0);
        ok = m_prog->link();
    }
    if (!ok) {
        m_glInfo += QStringLiteral(" | shader error: ") + m_prog->log();
        qWarning("GlobeWidget shader error: %s", qPrintable(m_prog->log()));
        delete m_prog;
        m_prog = nullptr;
        return false;
    }
    m_uMvp = m_prog->uniformLocation("u_mvp");
    m_uEye = m_prog->uniformLocation("u_eye");
    m_uRange = m_prog->uniformLocation("u_range");
    m_uLinearLat = m_prog->uniformLocation("u_linearLat");
    m_uSkirt = m_prog->uniformLocation("u_skirt");
    m_uTexXform = m_prog->uniformLocation("u_texXform");
    m_uUseTex = m_prog->uniformLocation("u_useTex");
    m_uColor = m_prog->uniformLocation("u_color");
    m_uTex = m_prog->uniformLocation("u_tex");

    // 共用网格：(N+1)^2 个面顶点 + 四条边各 N+1 个裙边顶点（遮住相邻层级之间的缝）
    const int n = kGridN;
    auto gridIndex = [n](int i, int j) { return GLushort(j * (n + 1) + i); };
    auto edgePoint = [n](int e, int k, int *i, int *j) {
        switch (e) {
        case 0: *i = k; *j = 0; break;
        case 1: *i = k; *j = n; break;
        case 2: *i = 0; *j = k; break;
        default: *i = n; *j = k; break;
        }
    };

    QVector<GLfloat> verts;
    QVector<GLushort> idx;
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
            verts << GLfloat(i) / n << GLfloat(j) / n << 0.0f;
    const int skirtBase = (n + 1) * (n + 1);
    for (int e = 0; e < 4; ++e) {
        for (int k = 0; k <= n; ++k) {
            int i = 0, j = 0;
            edgePoint(e, k, &i, &j);
            verts << GLfloat(i) / n << GLfloat(j) / n << 1.0f;
        }
    }
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const GLushort a = gridIndex(i, j), b = gridIndex(i + 1, j);
            const GLushort c = gridIndex(i, j + 1), d = gridIndex(i + 1, j + 1);
            idx << a << c << b << b << c << d;
        }
    }
    for (int e = 0; e < 4; ++e) {
        for (int k = 0; k < n; ++k) {
            int i0 = 0, j0 = 0, i1 = 0, j1 = 0;
            edgePoint(e, k, &i0, &j0);
            edgePoint(e, k + 1, &i1, &j1);
            const GLushort a = gridIndex(i0, j0), b = gridIndex(i1, j1);
            const GLushort sa = GLushort(skirtBase + e * (n + 1) + k), sb = GLushort(sa + 1);
            idx << a << b << sb << a << sb << sa;
        }
    }

    m_vbo.create();
    m_vbo.bind();
    m_vbo.allocate(verts.constData(), int(verts.size() * sizeof(GLfloat)));
    m_vbo.release();
    m_ibo.create();
    m_ibo.bind();
    m_ibo.allocate(idx.constData(), int(idx.size() * sizeof(GLushort)));
    m_ibo.release();
    m_indexCount = idx.size();
    return true;
}

void GlobeSurface::cleanup()
{
    clearTextures();
    m_vbo.destroy();
    m_ibo.destroy();
    delete m_prog;
    m_prog = nullptr;
    m_gl = nullptr;
}

void GlobeSurface::clearTextures()
{
    if (m_gl) {
        for (const CachedTexture &c : qAsConst(m_textures))
            m_gl->glDeleteTextures(1, &c.tex);
    }
    m_textures.clear();
}

void GlobeSurface::queuePendingTile(int z, int x, int y, const QImage &img)
{
    m_pendingTiles.insert(GlobeCamera::tileKey(z, x, y), img);
}

void GlobeSurface::markMissing(int z, int x, int y)
{
    m_missing.insert(GlobeCamera::tileKey(z, x, y));
}

void GlobeSurface::clearTileState()
{
    m_missing.clear();
    m_pendingTiles.clear();
}

// 极地补洞的颜色取 0 级瓦片最上/最下一行的平均色，和瓦片边缘自然衔接
void GlobeSurface::updateCapColors(TileSource *tiles)
{
    m_northCap = QVector4D(0.80f, 0.85f, 0.90f, 1.0f);
    m_southCap = QVector4D(0.92f, 0.94f, 0.96f, 1.0f);
    if (!tiles || tiles->minZoom() > 0)
        return;
    const QImage img = tiles->tile(0, 0, 0).convertToFormat(QImage::Format_RGB32);
    if (img.isNull())
        return;
    auto rowAverage = [&img](int row) {
        double r = 0, g = 0, b = 0;
        for (int x = 0; x < img.width(); ++x) {
            const QRgb p = img.pixel(x, row);
            r += qRed(p);
            g += qGreen(p);
            b += qBlue(p);
        }
        const double s = 255.0 * img.width();
        return QVector4D(float(r / s), float(g / s), float(b / s), 1.0f);
    };
    m_northCap = rowAverage(0);
    m_southCap = rowAverage(img.height() - 1);
}

int GlobeSurface::mapMinZoom(const TileSource *tiles, const NetTileSource *online) const
{
    if (online)
        return online->minZoom();
    return tiles ? tiles->minZoom() : 0;
}

int GlobeSurface::mapMaxZoom(const TileSource *tiles, const NetTileSource *online) const
{
    if (online)
        return online->maxZoom();
    return tiles ? tiles->maxZoom() : 3;
}

// 自身纹理未加载时向上找最近的父级纹理，并算出对应的子区域纹理坐标
GLuint GlobeSurface::textureFor(const GlobeCamera::TileId &t, int minZ, int maxZ,
                                quint64 frame, QVector4D *xform)
{
    // 缺本层瓦片时向上找父级纹理，用子区域贴过去——低清但连续，
    // 比露底色好；这也是缩放开合时不闪白的关键。
    for (int az = qMin(t.z, maxZ); az >= minZ; --az) {
        const int dz = t.z - az;
        const int ax = t.x >> dz, ay = t.y >> dz;
        auto it = m_textures.find(GlobeCamera::tileKey(az, ax, ay));
        if (it == m_textures.end())
            continue;
        it->lastUsed = frame;
        const float s = 1.0f / float(1 << dz);
        *xform = QVector4D((t.x - (ax << dz)) * s, (t.y - (ay << dz)) * s, s, s);
        return it->tex;
    }
    return 0;
}

void GlobeSurface::loadTexture(int z, int x, int y, TileSource *tiles, NetTileSource *online,
                               QOpenGLContext *ctx, quint64 frame)
{
    const quint64 key = GlobeCamera::tileKey(z, x, y);
    if (online) {
        // 在线模式：瓦片异步到手（线程池解码 / 网络下载），到了走 tileReady。
        // 这里不用本地 mbtiles 兜底——wanted 里的层级都在在线源范围内，
        // 拿低清离线瓦片顶上去，这个 key 就永久停在模糊图上，在线瓦片再也加载不进来。
        online->startLoad(z, x, y);
        return;
    }
    const QImage img = tiles ? tiles->tile(z, x, y) : QImage();
    if (img.isNull()) {
        m_missing.insert(key);
        return;
    }
    uploadTexture(key, img, ctx, frame);
}

void GlobeSurface::uploadTexture(quint64 key, const QImage &img, QOpenGLContext *ctx, quint64 frame)
{
    if (!m_gl || img.isNull() || m_textures.contains(key))
        return;
    // 只用 OpenGL ES 2.0 子集（GL_RGBA + glGenerateMipmap），ANGLE/D3D 下不报错
    // QOpenGLTexture 会设置 ES2 没有的 GL_TEXTURE_MAX_LEVEL 等参数，Win7 走 ANGLE 时会产生 GL_INVALID_ENUM
    // 在线瓦片合成后是 Format_RGB32（0xffRRGGBB，小端内存序即 B,G,R,A），正好能直接喂 GL_BGRA，
    // 省掉一次整图 convertToFormat 的 256KB 内存拷贝。其余格式仍走 RGBA8888。
    QImage rgba;
    bool bgra = false;
    if (img.format() == QImage::Format_RGB32 && ctx && !ctx->isOpenGLES()) {
        rgba = img;
        bgra = true;
    } else {
        rgba = img.convertToFormat(QImage::Format_RGBA8888);
    }
    const bool pot = (rgba.width() & (rgba.width() - 1)) == 0 && (rgba.height() & (rgba.height() - 1)) == 0;
    GLuint tex = 0;
    m_gl->glGenTextures(1, &tex);
    m_gl->glBindTexture(GL_TEXTURE_2D, tex);
    m_gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (bgra) {
        m_gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, rgba.width(), rgba.height(), 0,
                           GL_BGRA, GL_UNSIGNED_BYTE, rgba.constBits());
    } else {
        m_gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba.width(), rgba.height(), 0,
                           GL_RGBA, GL_UNSIGNED_BYTE, rgba.constBits());
    }
    // ES2 只允许 2 的幂尺寸纹理生成 mipmap
    if (pot)
        m_gl->glGenerateMipmap(GL_TEXTURE_2D);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, pot ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    m_gl->glBindTexture(GL_TEXTURE_2D, 0);
    m_textures.insert(key, CachedTexture{tex, frame});
}

// 网络瓦片到货后在这里统一上传：paintGL 里 GL 上下文才是当前的
void GlobeSurface::uploadPendingTiles(QOpenGLContext *ctx, quint64 frame)
{
    if (m_pendingTiles.isEmpty())
        return;
    int n = 0;
    for (auto it = m_pendingTiles.begin(); it != m_pendingTiles.end() && n < kLoadPerFrame;) {
        uploadTexture(it.key(), it.value(), ctx, frame);
        it = m_pendingTiles.erase(it);
        ++n;
    }
}

// 超出上限时释放最久未用的纹理；0、1 级常驻，作为兜底
void GlobeSurface::evictTextures(quint64 frame)
{
    int excess = m_textures.size() - kMaxTextures;
    if (excess <= 0 || !m_gl)
        return;
    QVector<QPair<quint64, quint64>> candidates;   // (lastUsed, key)
    for (auto it = m_textures.constBegin(); it != m_textures.constEnd(); ++it) {
        if ((it.key() >> 48) > 1 && it->lastUsed + 1 < frame)
            candidates.append(qMakePair(it->lastUsed, it.key()));
    }
    std::sort(candidates.begin(), candidates.end());
    for (int i = 0; i < candidates.size() && excess > 0; ++i, --excess) {
        const GLuint tex = m_textures.value(candidates[i].second).tex;
        m_gl->glDeleteTextures(1, &tex);
        m_textures.remove(candidates[i].second);
    }
}

void GlobeSurface::drawPatch(const QVector4D &range)
{
    m_prog->setUniformValue(m_uRange, range);
    m_gl->glDrawElements(GL_TRIANGLES, m_indexCount, GL_UNSIGNED_SHORT, nullptr);
}

void GlobeSurface::draw(const GlobeCamera &cam, TileSource *tiles, NetTileSource *online,
                        QOpenGLContext *ctx, quint64 frame)
{
    if (!m_prog || !m_gl)
        return;

    uploadPendingTiles(ctx, frame);

    const int minZ = mapMinZoom(tiles, online);
    const int maxZ = mapMaxZoom(tiles, online);

    // 预载最低两级，保证任何位置都有父级纹理可用
    if (minZ <= 1) {
        for (int z = minZ; z <= qMin(1, maxZ); ++z) {
            for (int y = 0; y < (1 << z); ++y) {
                for (int x = 0; x < (1 << z); ++x) {
                    const quint64 key = GlobeCamera::tileKey(z, x, y);
                    if (!m_textures.contains(key) && !m_missing.contains(key))
                        loadTexture(z, x, y, tiles, online, ctx, frame);
                }
            }
        }
    }

    QVector<GlobeCamera::TileId> visibleTiles;
    visibleTiles.reserve(256);
    cam.collectTiles(0, 0, 0, maxZ, visibleTiles);

    // 加载缺失纹理：低层级优先，每帧有上限，剩下的下一帧继续
    QVector<GlobeCamera::TileId> wanted;
    if (tiles || online) {
        for (const GlobeCamera::TileId &t : qAsConst(visibleTiles)) {
            const quint64 key = GlobeCamera::tileKey(t.z, t.x, t.y);
            if (t.z >= minZ && t.z <= maxZ
                    && !m_textures.contains(key) && !m_missing.contains(key))
                wanted.append(t);
        }
        std::sort(wanted.begin(), wanted.end(),
                  [](const GlobeCamera::TileId &a, const GlobeCamera::TileId &b) { return a.z < b.z; });
    }
    const int loadCount = qMin(wanted.size(), kLoadPerFrame);
    for (int i = 0; i < loadCount; ++i)
        loadTexture(wanted[i].z, wanted[i].x, wanted[i].y, tiles, online, ctx, frame);
    m_pending = wanted.size() - loadCount;

    m_prog->bind();
    m_vbo.bind();
    m_prog->enableAttributeArray(0);
    m_prog->setAttributeBuffer(0, GL_FLOAT, 0, 3, 3 * sizeof(GLfloat));
    m_ibo.bind();
    m_prog->setUniformValue(m_uMvp, cam.mvp());
    m_prog->setUniformValue(m_uEye, cam.eye());
    m_prog->setUniformValue(m_uTex, 0);
    m_gl->glActiveTexture(GL_TEXTURE0);

    // 极地：墨卡托瓦片覆盖不到 ±85.05° 以外，用纯色补齐
    m_prog->setUniformValue(m_uUseTex, 0.0f);
    m_prog->setUniformValue(m_uLinearLat, 1.0f);
    m_prog->setUniformValue(m_uSkirt, 0.01f);
    for (int q = 0; q < 4; ++q) {
        const float lon0 = float(-M_PI + q * M_PI / 2.0), lon1 = float(lon0 + M_PI / 2.0);
        m_prog->setUniformValue(m_uColor, m_northCap);
        drawPatch(QVector4D(lon0, lon1, float(M_PI / 2.0), float(GlobeCamera::kMercMaxLat)));
        m_prog->setUniformValue(m_uColor, m_southCap);
        drawPatch(QVector4D(lon0, lon1, float(-GlobeCamera::kMercMaxLat), float(-M_PI / 2.0)));
    }

    m_prog->setUniformValue(m_uLinearLat, 0.0f);
    int drawnMaxZ = 0;
    for (const GlobeCamera::TileId &t : qAsConst(visibleTiles)) {
        QVector4D xform;
        if (const GLuint tex = textureFor(t, minZ, maxZ, frame, &xform)) {
            m_gl->glBindTexture(GL_TEXTURE_2D, tex);
            m_prog->setUniformValue(m_uUseTex, 1.0f);
            m_prog->setUniformValue(m_uTexXform, xform);
        } else {
            m_prog->setUniformValue(m_uUseTex, 0.0f);
            m_prog->setUniformValue(m_uColor, m_baseColor);
        }
        // 裙边深度取粗两级网格的弦高，足以盖住相邻层级之间的裂缝
        const double angle = 2.0 * M_PI / double(1 << t.z) / kGridN;
        m_prog->setUniformValue(m_uSkirt, float(2.0 * angle * angle + 1e-5));
        drawPatch(GlobeCamera::tileRange(t.z, t.x, t.y));
        drawnMaxZ = qMax(drawnMaxZ, t.z);
    }

    m_ibo.release();
    m_prog->disableAttributeArray(0);
    m_vbo.release();
    m_prog->release();

    m_lastDrawn = visibleTiles.size();
    m_lastMaxZ = drawnMaxZ;
    evictTextures(frame);
}
