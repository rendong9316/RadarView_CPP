#ifndef GLOBESURFACE_H
#define GLOBESURFACE_H

#include <QHash>
#include <QImage>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QSet>
#include <QString>
#include <QVector4D>

#include "globecamera.h"

class QOpenGLContext;
class QOpenGLShaderProgram;
class NetTileSource;
class TileSource;

// 3D 椭球地表瓦片渲染器与纹理 LRU 缓存（OpenGL ES 2.0 / GLSL ES 1.00 子集）。
// 不使用 VAO / QOpenGLTexture / GL3+ 接口，所有 GL 调用均通过 QOpenGLFunctions 执行。
class GlobeSurface
{
public:
    GlobeSurface();
    ~GlobeSurface();

    // 需在 GL 上下文当前时调用
    bool initialize(QOpenGLFunctions *gl, QOpenGLContext *ctx);
    void cleanup();
    void clearTextures();

    void queuePendingTile(int z, int x, int y, const QImage &img);
    void markMissing(int z, int x, int y);
    void clearTileState();
    void updateCapColors(TileSource *tiles);

    // 绘制极地补洞 + 可见瓦片并驱动异步/增量瓦片加载与纹理淘汰
    void draw(const GlobeCamera &cam, TileSource *tiles, NetTileSource *online,
              QOpenGLContext *ctx, quint64 frame);

    bool isReady() const { return m_prog != nullptr; }
    QString glInfo() const { return m_glInfo; }
    int lastDrawn() const { return m_lastDrawn; }
    int lastMaxZ() const { return m_lastMaxZ; }
    int pendingCount() const { return m_pending; }
    int textureCount() const { return m_textures.size(); }
    int missingCount() const { return m_missing.size(); }

private:
    struct CachedTexture {
        GLuint tex;
        quint64 lastUsed;
    };

    int mapMinZoom(const TileSource *tiles, const NetTileSource *online) const;
    int mapMaxZoom(const TileSource *tiles, const NetTileSource *online) const;
    GLuint textureFor(const GlobeCamera::TileId &t, int minZ, int maxZ, quint64 frame, QVector4D *xform);
    void loadTexture(int z, int x, int y, TileSource *tiles, NetTileSource *online,
                     QOpenGLContext *ctx, quint64 frame);
    void uploadTexture(quint64 key, const QImage &img, QOpenGLContext *ctx, quint64 frame);
    void uploadPendingTiles(QOpenGLContext *ctx, quint64 frame);
    void evictTextures(quint64 frame);
    void drawPatch(const QVector4D &range);

    QOpenGLFunctions *m_gl = nullptr;
    QOpenGLShaderProgram *m_prog = nullptr;
    QOpenGLBuffer m_vbo{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_ibo{QOpenGLBuffer::IndexBuffer};
    int m_indexCount = 0;
    QString m_glInfo;
    int m_uMvp = -1, m_uEye = -1, m_uRange = -1, m_uLinearLat = -1, m_uSkirt = -1;
    int m_uTexXform = -1, m_uUseTex = -1, m_uColor = -1, m_uTex = -1;

    QHash<quint64, CachedTexture> m_textures;
    QSet<quint64> m_missing;               // mbtiles 或网络里不存在的瓦片，不再重复查询
    QHash<quint64, QImage> m_pendingTiles; // 网络已到、等着在 GL 线程上传
    int m_lastDrawn = 0;
    int m_lastMaxZ = 0;
    int m_pending = 0;

    QVector4D m_northCap;
    QVector4D m_southCap;
    QVector4D m_baseColor;
};

#endif // GLOBESURFACE_H
