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
#include <QElapsedTimer>
#include <QTimer>
#include <memory>

#include "tilesource.h"
#include "tracklayer.h"

class QOpenGLShaderProgram;

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
    // 相机对准某经纬度（度），altKm 为离地高度
    void lookAt(double lonDeg, double latDeg, double altKm);

    // 航迹图层：数据由外部 TrackStore 持有，变化后调用 update() 即可
    void setTrackStore(const TrackStore *store);
    TrackLayer *trackLayer() { return &m_trackLayer; }

    // 屏幕坐标 -> 经纬度（度），未落在地球上时返回 false
    bool geoAt(const QPointF &pos, double *lonDeg, double *latDeg);
    // 经纬度（度）+ 高度（米）-> 屏幕坐标；在相机后方返回 false
    bool screenPos(double lonDeg, double latDeg, double altM, QPointF *pos);
    // 屏幕坐标处的航迹（TrackStore::tracks() 下标），没有返回 -1
    int trackAt(const QPointF &pos);
    bool isLoading() const { return m_pending > 0; }
    int drawnMaxZoom() const { return m_lastMaxZ; }
    int tileMaxZoom() const { return m_tiles ? m_tiles->maxZoom() : -1; }
    int drawnTileCount() const { return m_lastDrawn; }
    int glErrorCount() const { return m_glErrors; }
    quint64 frameCount() const { return m_frame; }
    bool isGlReady() const { return m_prog != nullptr && m_trackLayer.isReady(); }
    double altitudeKm() const;
    int fps() const;                  // 与 RadarView setupFpsTracking 相同的平滑帧率

signals:
    // 状态栏：相机离地高度（km）、鼠标经纬度（度，不在地球上时为 0）、帧率（0 表示暂无）
    void viewStatusChanged(double heightKm, double lonDeg, double latDeg, int fps);
    void trackClicked(int index);                              // 左键单击（未拖动），空白处为 -1
    void trackContextMenuRequested(int index, const QPoint &globalPos);   // 右键，空白处为 -1

protected:
    void initializeGL() override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    struct TileId { int z, x, y; };
    struct CachedTexture { GLuint tex; quint64 lastUsed; };

    void cleanupGL();
    void clearTextures();
    void updateMatrices();
    void collectTiles(int z, int x, int y, QVector<TileId> &out) const;
    GLuint textureFor(const TileId &t, QVector4D *xform);   // 0 = 无可用纹理
    void loadTexture(int z, int x, int y);
    void evictTextures();
    void updateCapColors();
    bool pick(const QPointF &pos, double *lon, double *lat);
    void drawPatch(const QVector4D &range);
    void emitStatus();
    void trackFps();
    void updateHover(const QPoint &pos, const QPoint &globalPos);
    QString trackTooltip(int index) const;

    std::unique_ptr<TileSource> m_tiles;
    TrackLayer m_trackLayer;
    const TrackStore *m_trackStore = nullptr;

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
    QPoint m_pressPos;
    bool m_clickCandidate = false;   // 按下后移动不超过几个像素，松开时算单击
    bool m_cursorValid = false;
    double m_cursorLon = 0.0, m_cursorLat = 0.0;   // 弧度

    QTimer m_statusTimer;              // 状态合并发送，避免在 paintGL 里改动其他控件
    QElapsedTimer m_fpsClock;
    qint64 m_fpsLastSample = -1;
    int m_fpsFrames = 0;
    double m_fpsSmoothed = 0.0;
};

#endif // GLOBEWIDGET_H
