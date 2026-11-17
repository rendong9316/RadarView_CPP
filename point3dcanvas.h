#ifndef POINT3DCANVAS_H
#define POINT3DCANVAS_H

#include <QColor>
#include <QRectF>
#include <QSet>
#include <QVector>
#include <QWidget>

#include "point3dtypes.h"

class QMouseEvent;
class QWheelEvent;

// 点迹三维画布：纯 QPainter 正交投影散点 + 航迹叠加 + 色标条 + 悬停提示。
// 数据由外部持有（setDataset 只保存指针），每次重绘实时取。
class Point3dCanvas : public QWidget
{
    Q_OBJECT
public:
    explicit Point3dCanvas(QWidget *parent = nullptr);

    void setDataset(const Point3dDataset *data);
    const Point3dDataset *dataset() const { return m_data; }
    // 数据集内容变化（如航迹叠加变化）后重算坐标范围，不重置当前帧
    void refreshLimits();

    void setCurrentFrame(int index);          // 0 基
    int currentFrame() const { return m_current; }

    void setWindowLength(int len);            // 0 = 全部
    int windowLength() const { return m_windowLength; }

    void setViewMode(Point3dViewMode mode);
    Point3dViewMode viewMode() const { return m_viewMode; }

    void setTrackSource(Point3dTrackSource source);
    Point3dTrackSource trackSource() const { return m_trackSource; }

    void setShowDetections(bool on);
    bool showDetections() const { return m_showDetections; }
    void setShowTracks(bool on);
    bool showTracks() const { return m_showTracks; }
    void setShowLabels(bool on);
    bool showLabels() const { return m_showLabels; }

    void setHighlightedBatch(int batchNo);    // -1 = 无高亮
    int highlightedBatch() const { return m_highlightedBatch; }

    // 只显示这些批次号的叠加航迹（地图单独显示/筛选联动）；空集合 = 全部显示
    void setIsolatedBatches(const QSet<int> &batches);
    QSet<int> isolatedBatches() const { return m_isolatedBatches; }

    void setTrackLineWidth(double w);
    double trackLineWidth() const { return m_trackLineWidth; }
    void setPointScale(double s);             // 点迹尺寸倍数
    double pointScale() const { return m_pointScale; }
    void setLabelScale(double s);             // 航迹数字标签字号倍数
    double labelScale() const { return m_labelScale; }

    // 鼠标左键拖拽的行为：旋转视角 / 平移视图（平移时鼠标显示手爪）
    enum class DragMode { Rotate, Pan };
    void setDragMode(DragMode mode);
    DragMode dragMode() const { return m_dragMode; }

    void setCamera(double yaw, double pitch, double zoom,
                   double panX = 0.0, double panY = 0.0);
    void camera(double *yaw, double *pitch, double *zoom,
                double *panX = nullptr, double *panY = nullptr) const;
    void resetCamera();

    int visiblePointCount() const { return m_lastVisibleCount; }
    int visibleFrameCount() const;
    int visibleTrackCount() const { return m_lastVisibleTracks; }   // 已绘制（点源过滤后）的叠加航迹数
    QRectF plotRect() const;                  // 三维绘图区（色标条按它定位）
    QString statusText() const;               // 顶部统计文字

signals:
    void trackFocusChanged(int batchNo, TrackSource source, qint64 t0, qint64 t1);
    void trackActivated(int batchNo, TrackSource source, qint64 t0, qint64 t1);
    void cameraChanged();
    void hoverChanged();
    void statsChanged();          // 重绘后统计文字变化（供窗口状态栏刷新）

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    struct ProjectedPoint {
        QPointF screen;
        float radius = 0.0f;
        int frame = 0;
        int point = 0;
        double depth = 0.0;
    };
    struct ProjectedTrack {
        int track = -1;
        int batchNo = 0;
        TrackSource source = TrackSource::Radar;
        qint64 t0 = 0, t1 = 0;
        QVector<QPointF> polyline;
        QPointF head;
        QRectF labelRect;
    };
    struct Limits {
        double xmin = 0.0, xmax = 1.0, ymin = 0.0, ymax = 1.0;
    };
    struct Camera {
        double yaw = 42.0, pitch = 23.0, zoom = 1.0;
    };
    struct Basis {
        double ex = 0.0, ey = 0.0, ez = 0.0;   // 相机方向
        double rx = 1.0, ry = 0.0, rz = 0.0;   // 屏幕右
        double ux = 0.0, uy = 1.0, uz = 0.0;   // 屏幕上
        double scale = 100.0, cx = 0.0, cy = 0.0;
    };

    void rebuildLimits();
    void updateBasis(const QRectF &plot);
    void zRange(double *lo, double *hi) const;
    double normalizedX(double x) const;
    double normalizedY(double y) const;
    double normalizedZ(double z, double zlo, double zhi) const;
    QPointF project(double nx, double ny, double nz, double *depth = nullptr) const;
    void drawGridAndAxes(QPainter &p, const QRectF &plot, double zlo, double zhi);
    void drawDetections(QPainter &p, double zlo, double zhi);
    void drawTracks(QPainter &p, double zlo, double zhi);
    void drawHover(QPainter &p);
    void drawColorBar(QPainter &p, const QRectF &plot);
    void drawTitle(QPainter &p, const QRectF &plot, double zlo, double zhi);
    int hitTrack(const QPointF &pos) const;

    const Point3dDataset *m_data = nullptr;
    int m_current = 0;
    int m_windowLength = 0;           // 0 = 全部
    Point3dViewMode m_viewMode = Point3dViewMode::RVF;
    Point3dTrackSource m_trackSource = Point3dTrackSource::Auto;
    bool m_showDetections = true;
    bool m_showTracks = true;
    bool m_showLabels = true;
    int m_highlightedBatch = -1;
    QSet<int> m_isolatedBatches;
    double m_trackLineWidth = 2.0;
    double m_pointScale = 1.0;
    double m_labelScale = 1.0;

    Camera m_cam;
    DragMode m_dragMode = DragMode::Rotate;
    double m_panX = 0.0, m_panY = 0.0;   // 平移偏移（屏幕像素，叠加到投影中心）
    Basis m_basis;
    Limits m_limits[3];

    bool m_dragging = false;
    QPoint m_lastMouse;
    int m_hoverPoint = -1;            // m_projected 下标
    int m_hoverTrack = -1;            // m_tracks 下标
    int m_lastVisibleCount = 0;
    int m_lastVisibleTracks = 0;

    QVector<ProjectedPoint> m_projected;
    QVector<ProjectedTrack> m_tracks;
    QString m_lastStatusText;
};

#endif // POINT3DCANVAS_H
