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

#include <QPixmap>
#include <QPolygonF>

#include "tilesource.h"
#include "tracklayer.h"

class QOpenGLShaderProgram;
class QPainter;
class NetTileSource;
class FlagStore;
class RulerState;
class SpatialLasso;
struct Track;

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
    // 在线底图（天地图）：tiles 为空时也能用；本地 mbtiles 仍保留作为兜底
    bool openOnlineMap(const QString &token, bool useAmap = false, QString *error = nullptr);
    void closeOnlineMap();
    bool onlineMap() const { return m_online != nullptr; }
    // 离线瓦片是否「缺块留空」。true（默认）：只画该层级真实存在的瓦片，
    // 缺的地方就空着，方便看清离线包到底覆盖到哪；false：退回老行为，
    // 用低层级父瓦片放大铺满（看起来连续但是糊的）。
    // 只对离线源生效；在线源需要父级兜底来做渐进填充。
    void setOfflineStrict(bool on) { m_offlineStrict = on; update(); }
    bool offlineStrict() const { return m_offlineStrict; }
    bool onlineBusy() const;
    QString mapSourceName() const;
    QString tileName() const;

    // ---- 坐标系 ----
    // 场景（球面经纬度）用的坐标系由当前底图决定：高德瓦片是 GCJ-02，其余是 WGS84。
    // 数据（航迹/旗标/标尺/数据库/导出）永远是 WGS84，只有往球面上摆的时候才转换。
    bool sceneIsGcj() const;
    QString coordSystemName() const { return m_sceneCrs ? QStringLiteral("GCJ-02") : QStringLiteral("WGS84"); }
    void toScene(double lonWgs, double latWgs, double *lonScene, double *latScene) const;
    void fromScene(double lonScene, double latScene, double *lonWgs, double *latWgs) const;

    void resetView();
    void zoomBy(double steps);                          // 正数拉近，负数拉远
    void panPixels(const QPoint &from, const QPoint &to); // 把 from 处的地面拖到 to
    QString debugInfo() const;
    // 相机对准某经纬度（度），altKm 为离地高度
    void lookAt(double lonDeg, double latDeg, double altKm);

    // 航迹图层：数据由外部 TrackStore 持有，变化后调用 update() 即可
    void setTrackStore(const TrackStore *store);
    TrackLayer *trackLayer() { return &m_trackLayer; }

    // 旗标 / 标尺（数据由主窗口持有）
    void setMapTools(FlagStore *flags, RulerState *ruler);
    // 空间套索（交互状态由主窗口持有；激活后左键逐点/自由手绘，双击或右键闭合）
    void setSpatialLasso(SpatialLasso *lasso);
    // 航迹标签（RadarView Ctrl+T）
    void setShowLabels(bool on);
    bool showLabels() const { return m_showLabels; }
    // 旗标整体缩放（RadarView useFlagScale，默认 1.2，范围 0.5–3.0）
    void setFlagScale(double s) { m_flagScale = s; m_flagIcon = QPixmap(); update(); }
    double flagScale() const { return m_flagScale; }

    // 经纬度（WGS84）在当前视角下的屏幕位置；在地球背面或相机后方返回 false
    bool visiblePos(double lonDeg, double latDeg, double altM, QPointF *pos);
    QString flagAt(const QPointF &pos);          // 屏幕位置处的旗标 id
    // ---- 自检用 ----
    int labelsDrawn() const { return m_labelsDrawn; }
    int flagsDrawn() const { return m_flagsDrawn; }
    int rulerMarkersDrawn() const { return m_rulerMarkersDrawn; }
    int lassoVerticesDrawn() const { return m_lassoVerticesDrawn; }
    bool pointHover(int *track, int *point) const;
    // 航迹下标即将失效（删除 / 重新加载）时清掉悬停状态
    void resetInteraction();
    static QString trackLabel(const Track &t);   // "航班号 | 机型"，都没有时为 id

    // 屏幕坐标 -> WGS84 经纬度（度），未落在地球上时返回 false
    bool geoAt(const QPointF &pos, double *lonDeg, double *latDeg);
    // WGS84 经纬度（度）+ 高度（米）-> 屏幕坐标；在相机后方返回 false
    bool screenPos(double lonDeg, double latDeg, double altM, QPointF *pos);
    // 屏幕坐标处的航迹（TrackStore::tracks() 下标），没有返回 -1
    int trackAt(const QPointF &pos);
    // 套索边界采样：两点间按经纬度平面线性插值再投影（与 pointInPolygon 的多边形定义一致，
    // 保证「看到的边界」就是「查询用的边界」）。pts/vis 一一对应，vis=false 表示该点在地球背面。
    void sampleLonLatLine(double lat1, double lon1, double lat2, double lon2,
                          QVector<QPointF> *pts, QVector<bool> *vis);
    bool isLoading() const { return m_pending > 0 || onlineBusy(); }
    int drawnMaxZoom() const { return m_lastMaxZ; }
    int tileMaxZoom() const;
    int drawnTileCount() const { return m_lastDrawn; }
    int glErrorCount() const { return m_glErrors; }
    quint64 frameCount() const { return m_frame; }
    bool isGlReady() const { return m_prog != nullptr && m_trackLayer.isReady(); }
    double altitudeKm() const;
    int fps() const;                  // 与 RadarView setupFpsTracking 相同的平滑帧率

signals:
    // 状态栏：相机离地高度（km）、鼠标经纬度（度，不在地球上时为 0）、帧率（0 表示暂无）
    // hasCoord=false 表示鼠标没落在地球上（此时经纬度无意义）
    void viewStatusChanged(double heightKm, double lonDeg, double latDeg, int fps, bool hasCoord);
    void trackClicked(int index);                              // 左键单击（未拖动），空白处为 -1
    void trackContextMenuRequested(int index, const QPoint &globalPos);   // 右键，空白处为 -1
    void flagContextMenuRequested(const QString &flagId, const QPoint &globalPos);

protected:
    void initializeGL() override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
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
    void uploadTexture(quint64 key, const QImage &img);
    int mapMinZoom() const;
    int mapMaxZoom() const;
    void uploadPendingTiles();
    void evictTextures();
    void updateCapColors();
    bool pick(const QPointF &pos, double *lon, double *lat);   // 场景坐标（弧度）
    // 场景经纬度（度）→ 屏幕，不做任何坐标转换
    bool sceneVisiblePos(double lonDeg, double latDeg, double altM, QPointF *pos);
    bool sceneScreenPos(double lonDeg, double latDeg, double altM, QPointF *pos);
    // 底图换了以后同步场景坐标系（会重建航迹几何）
    void applySceneCrs();
    void drawPatch(const QVector4D &range);
    void emitStatus();
    void trackFps();
    void updateHover(const QPoint &pos, const QPoint &globalPos);
    QString trackTooltip(int index) const;
    friend class GlobeOverlay;
    void paintOverlay(QPainter &p);
    void drawRuler(QPainter &p);
    void drawFlags(QPainter &p);
    void drawLabels(QPainter &p);
    void drawLasso(QPainter &p);
    void drawPointTip(QPainter &p);
    // 两点间沿大圆插值的屏幕折线（背面部分断开）
    QVector<QPolygonF> geodesicLines(double lat1, double lon1, double lat2, double lon2);
    QRectF flagIconRect(const QPointF &anchor) const;

    QWidget *m_overlay = nullptr;            // 叠加层（透明子控件）
    bool m_overlayWanted = false;
    FlagStore *m_flags = nullptr;
    RulerState *m_ruler = nullptr;
    SpatialLasso *m_lasso = nullptr;
    bool m_showLabels = false;
    int m_labelsDrawn = 0, m_flagsDrawn = 0, m_rulerMarkersDrawn = 0, m_lassoVerticesDrawn = 0;
    int m_tipTrack = -1, m_tipPoint = -1;
    QHash<QString, QPixmap> m_labelCache;          // 标签文字 + 颜色 -> 带描边的位图
    QPixmap m_flagIcon;

    std::unique_ptr<TileSource> m_tiles;
    bool m_sceneCrs = false;        // 当前场景是否 GCJ-02（已生效的那个，不是配置值）
    bool m_offlineStrict = true;    // 离线源缺块留空（不拿低层级父瓦片铺满）
    NetTileSource *m_online = nullptr;      // 在线底图（天地图），为空表示离线模式
    QHash<quint64, QImage> m_pendingTiles;  // 网络已到、等着在 GL 线程上传
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
    double m_flagScale = 1.2;     // 旗标整体缩放（RadarView useFlagScale）

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
    bool m_lassoPressValid = false;  // 套索按下时命中了地面（用于自动切换逐点/手绘）
    double m_lassoPressLat = 0.0, m_lassoPressLon = 0.0;
    bool m_cursorValid = false;
    double m_cursorLon = 0.0, m_cursorLat = 0.0;   // 弧度

    QTimer m_statusTimer;              // 状态合并发送，避免在 paintGL 里改动其他控件
    QElapsedTimer m_fpsClock;
    qint64 m_fpsLastSample = -1;
    int m_fpsFrames = 0;
    double m_fpsSmoothed = 0.0;
};

#endif // GLOBEWIDGET_H
