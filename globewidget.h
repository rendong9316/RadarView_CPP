#ifndef GLOBEWIDGET_H
#define GLOBEWIDGET_H

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLBuffer>
#include <QMatrix4x4>
#include <QVector3D>
#include <QVector4D>
#include <QVector>
#include <QHash>
#include <QSet>
#include <QPoint>
#include <memory>

#include "tilesource.h"

class QOpenGLShaderProgram;
class QOpenGLTexture;

// 3D 地球：WGS84 椭球 + 本地 mbtiles（Web 墨卡托 XYZ）瓦片
// 左键拖拽旋转，滚轮缩放，按视距自动选择瓦片层级
// 着色器只用 GLSL ES 2.0 子集，桌面 OpenGL / ANGLE / 软件渲染都能跑（兼容 Win7）
class GlobeWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT
public:
    explicit GlobeWidget(QWidget *parent = nullptr);
    ~GlobeWidget() override;

    bool openTiles(const QString &path, QString *error = nullptr);
    QString tileName() const;

    void resetView();
    void zoomBy(double steps);                          // 正数拉近，负数拉远
    void panPixels(const QPoint &from, const QPoint &to); // 把 from 处的地面拖到 to
    QString debugInfo() const;

    // 屏幕坐标 -> 经纬度（度），未落在地球上时返回 false
    bool geoAt(const QPointF &pos, double *lonDeg, double *latDeg);
    bool isLoading() const { return m_pending > 0; }
    int drawnMaxZoom() const { return m_lastMaxZ; }
    int tileMaxZoom() const { return m_tiles ? m_tiles->maxZoom() : -1; }
    int drawnTileCount() const { return m_lastDrawn; }
    int glErrorCount() const { return m_glErrors; }
    bool isGlReady() const { return m_prog != nullptr; }
    double altitudeKm() const;

signals:
    void statusChanged(const QString &text);

protected:
    void initializeGL() override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;

private:
    struct TileId { int z, x, y; };
    struct CachedTexture { QOpenGLTexture *tex; quint64 lastUsed; };

    void cleanupGL();
    void clearTextures();
    void updateMatrices();
    void collectTiles(int z, int x, int y, QVector<TileId> &out) const;
    QOpenGLTexture *textureFor(const TileId &t, QVector4D *xform);
    void loadTexture(int z, int x, int y);
    void evictTextures();
    void updateCapColors();
    bool pick(const QPointF &pos, double *lon, double *lat);
    void drawPatch(const QVector4D &range);
    void emitStatus();

    std::unique_ptr<TileSource> m_tiles;

    QOpenGLShaderProgram *m_prog = nullptr;
    QOpenGLBuffer m_vbo{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_ibo{QOpenGLBuffer::IndexBuffer};
    int m_indexCount = 0;
    bool m_glReady = false;
    QString m_glInfo;
    int m_glErrors = 0;
    int m_uMvp = -1, m_uEye = -1, m_uRange = -1, m_uLinearLat = -1, m_uSkirt = -1;
    int m_uTexXform = -1, m_uUseTex = -1, m_uColor = -1, m_uTex = -1;

    QHash<quint64, CachedTexture> m_textures;
    QSet<quint64> m_missing;   // mbtiles 里不存在的瓦片，不再重复查询
    quint64 m_frame = 0;
    int m_lastDrawn = 0;
    int m_lastMaxZ = 0;
    int m_pending = 0;

    QVector4D m_northCap, m_southCap, m_baseColor;

    // 相机：星下点经纬度（弧度）+ 离地高度（单位：赤道半径），始终俯视星下点
    double m_camLon = 0.0, m_camLat = 0.0, m_alt = 2.5;
    QMatrix4x4 m_view, m_proj, m_mvp;
    QVector3D m_eye;
    QVector4D m_planes[6];
    float m_focalPx = 1.0f;

    bool m_dragging = false;
    QPoint m_lastPos;
    bool m_cursorValid = false;
    double m_cursorLon = 0.0, m_cursorLat = 0.0;
};

#endif // GLOBEWIDGET_H
