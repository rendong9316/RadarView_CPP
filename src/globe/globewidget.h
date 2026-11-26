#ifndef GLOBEWIDGET_H
#define GLOBEWIDGET_H

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QColor>
#include <QPoint>
#include <QPointF>
#include <QElapsedTimer>
#include <QTimer>
#include <QVector>
#include <memory>

#include "drawingmanager.h"
#include "globecamera.h"
#include "globedrawcontroller.h"
#include "globeoverlay.h"
#include "globesurface.h"
#include "tilesource.h"
#include "trackanalysis.h"
#include "tracklayer.h"

class GlobeOverlay;
class NetTileSource;
class FlagStore;
class RadarSiteStore;
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
    bool onlineBusy() const;
    QString mapSourceName() const;
    QString tileName() const;

    // ---- 坐标系 ----
    // 场景（球面经纬度）用的坐标系由当前底图决定：高德瓦片是 GCJ-02，其余是 WGS84。
    // 数据（航迹/旗标/标尺/数据库/导出）永远是 WGS84，只有往球面上摆的时候才转换。
    bool sceneIsGcj() const;
    QString coordSystemName() const { return m_camera.coordSystemName(); }
    void toScene(double lonWgs, double latWgs, double *lonScene, double *latScene) const;
    void fromScene(double lonScene, double latScene, double *lonWgs, double *latWgs) const;

    void resetView();
    void zoomBy(double steps);                            // 正数拉近，负数拉远
    void panPixels(const QPoint &from, const QPoint &to); // 把 from 处的地面拖到 to
    QString debugInfo() const;
    // 相机对准某经纬度（度），altKm 为离地高度
    void lookAt(double lonDeg, double latDeg, double altKm);
    // 相机自动缩放到指定 WGS84 经纬度包围盒（右键「缩放至该航迹」等）
    void fitGeoBounds(double minLon, double maxLon, double minLat, double maxLat);
    // 地图定位高亮准星（点迹表双击定位 / 时序曲线光标定位）
    void setFocusPoint(double lonDeg, double latDeg, const QString &label = QString());
    void clearFocusPoint();
    bool hasFocusPoint() const { return m_focusValid; }
    // 同航迹滤波叠加对比（关联点 / 预测点 / 新息残差线）
    void setFilterOverlay(const GlobeOverlay::FilterOverlayState &st);
    const GlobeOverlay::FilterOverlayState &filterOverlay() const { return m_filterOverlay; }
    // 两航迹误差对比连线叠加
    void setCompareLinks(const QVector<trackanalysis::TrackDiffSample> &samples, int maxErrIdx, bool visible);
    int filterOverlayItemsDrawn() const;
    int compareLinksDrawn() const;
    // 当前相机（WGS84 经纬度 + 离地高度 km），用于退出前保存 / 启动时恢复视角
    void cameraView(double *lonDeg, double *latDeg, double *altKm) const;
    void setCameraView(double lonDeg, double latDeg, double altKm);

    // 航迹图层：数据由外部 TrackStore 持有，变化后调用 update() 即可
    void setTrackStore(const TrackStore *store);
    TrackLayer *trackLayer() { return &m_trackLayer; }

    // 雷达站址与 PPI 极坐标网格（数据由主窗口持有）
    void setRadarSites(RadarSiteStore *sites);
    int radarPpiPrimitivesDrawn() const;
    // 旗标 / 标尺（数据由主窗口持有）
    void setMapTools(FlagStore *flags, RulerState *ruler);
    // 空间套索（交互状态由主窗口持有；激活后左键逐点/自由手绘，双击或右键闭合）
    void setSpatialLasso(SpatialLasso *lasso);
    // 贴地绘图（旗标距离圆、自由曲线等）：数据由主窗口的 DrawingManager 持有，
    // 这里只负责把顶点投影到屏幕画到叠加层上（与标尺/套索同一套投影口径）
    void setDrawingManager(DrawingManager *drawing);

    // ---- 绘图状态机 ----
    // 工具在工具栏切换；左键归画笔，右键拖拽旋转，滚轮始终缩放
    void setDrawTool(DrawTool t);
    DrawTool drawTool() const { return m_drawCtrl.tool(); }
    void setDrawColor(const QColor &c) { m_drawCtrl.setColor(c); }
    QColor drawColor() const { return m_drawCtrl.color(); }
    bool cancelDrawing();                    // 丢弃未完成的笔画/选框；返回是否有东西被丢弃
    // 选择工具：选中一个要素后高亮，Delete 删除、双击改属性
    int selectedFeature() const { return m_drawCtrl.selectedFeature(); }
    void setSelectedFeature(int id);         // 0 = 取消选择
    bool deleteSelected();                   // 删除选中的要素（Delete 键 / 菜单）
    int hoverFeature() const { return m_drawCtrl.hoverFeature(); }
    int strokePointCount() const { return m_drawCtrl.strokePointCount(); }
    bool boxSelecting() const { return m_drawCtrl.boxSelecting(); }
    int drawPreviewSegments() const;
    // 航迹标签（RadarView Ctrl+T）
    void setShowLabels(bool on);
    bool showLabels() const;
    // 旗标整体缩放（RadarView useFlagScale，默认 1.2，范围 0.5–3.0）
    void setFlagScale(double s);
    double flagScale() const;
    // 空间套索顶点小圆球的半径（px @1x，默认 3；设置面板「套索顶点」可调）
    void setLassoVertexPx(double px);
    double lassoVertexPx() const;

    // 经纬度（WGS84）在当前视角下的屏幕位置；在地球背面或相机后方返回 false
    bool visiblePos(double lonDeg, double latDeg, double altM, QPointF *pos);
    QString flagAt(const QPointF &pos);          // 屏幕位置处的旗标 id
    // ---- 自检用 ----
    int labelsDrawn() const;
    int flagsDrawn() const;
    int rulerMarkersDrawn() const;
    int lassoVerticesDrawn() const;
    int featuresDrawn() const;   // 叠加层实际画出的绘图要素数
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
    bool isLoading() const { return m_surface.pendingCount() > 0 || onlineBusy(); }
    int drawnMaxZoom() const { return m_surface.lastMaxZ(); }
    int tileMaxZoom() const;
    int drawnTileCount() const { return m_surface.lastDrawn(); }
    int glErrorCount() const { return m_glErrors; }
    quint64 frameCount() const { return m_frame; }
    bool isGlReady() const { return m_surface.isReady() && m_trackLayer.isReady(); }
    double altitudeKm() const;
    int fps() const;                  // 与 RadarView setupFpsTracking 相同的平滑帧率

signals:
    // 状态栏：相机离地高度（km）、鼠标经纬度（度，不在地球上时为 0）、帧率（0 表示暂无）
    // hasCoord=false 表示鼠标没落在地球上（此时经纬度无意义）
    void viewStatusChanged(double heightKm, double lonDeg, double latDeg, int fps, bool hasCoord);
    // 相机被用户改动（拖动结束 / 滚轮 / 复位 / 定位）：主窗口据此持久化视角
    void cameraChanged();
    void trackClicked(int index);                              // 左键单击（未拖动），空白处为 -1
    void trackMultiSelectClicked(int index);                   // Ctrl + 左键单击：多选对比隔离
    void trackHovered(int index);                              // 悬停的航迹变化（离开为 -1）
    void trackContextMenuRequested(int index, const QPoint &globalPos);   // 右键，空白处为 -1
    void flagContextMenuRequested(const QString &flagId, const QPoint &globalPos);
    // 绘图：文本框/油漆桶需要弹窗，交给主窗口处理（GlobeWidget 不持有对话框）
    void drawTextRequested(double lat, double lon, const QPoint &globalPos);
    void drawFillRequested(int featureId);      // 0 = 该位置没有封闭图形
    void drawFeatureAdded(int featureId);
    void drawFeatureEditRequested(int featureId);   // 选择工具双击：主窗口弹属性窗
    void drawSelectionChanged(int featureId);       // 0 = 取消选择
    void drawMessage(const QString &text);          // 打断/裁剪的结果提示（空串 = 清除）

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
    void cleanupGL();
    void applySceneCrs();
    void syncOverlayContext();
    void emitStatus();
    void trackFps();
    void updateHover(const QPoint &pos, const QPoint &globalPos);
    QString trackTooltip(int index) const;

    GlobeCamera m_camera;
    GlobeSurface m_surface;
    GlobeDrawController m_drawCtrl;
    GlobeOverlay *m_overlay = nullptr;       // 叠加层（透明子控件）

    FlagStore *m_flags = nullptr;
    RadarSiteStore *m_radarSites = nullptr;
    RulerState *m_ruler = nullptr;
    SpatialLasso *m_lasso = nullptr;
    DrawingManager *m_draw = nullptr;
    int m_tipTrack = -1, m_tipPoint = -1;
    bool m_focusValid = false;
    double m_focusLon = 0.0, m_focusLat = 0.0;
    QString m_focusLabel;
    GlobeOverlay::FilterOverlayState m_filterOverlay;
    QVector<trackanalysis::TrackDiffSample> m_compareSamples;
    int m_compareMaxIdx = -1;
    bool m_compareLinksVisible = false;

    std::unique_ptr<TileSource> m_tiles;
    NetTileSource *m_online = nullptr;       // 在线底图（天地图），为空表示离线模式
    TrackLayer m_trackLayer;
    const TrackStore *m_trackStore = nullptr;

    bool m_glReady = false;
    QString m_glInfo;
    int m_glErrors = 0;
    quint64 m_frame = 0;

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
