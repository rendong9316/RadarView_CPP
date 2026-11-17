#ifndef POINT3DCANVAS_H
#define POINT3DCANVAS_H

#include <QColor>
#include <QRectF>
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

    void setTrackLineWidth(double w);
    double trackLineWidth() const { return m_trackLineWidth; }

    void setCamera(double yaw, double pitch, double zoom);
    void camera(double *yaw, double *pitch, double *zoom) const;
    void resetCamera();

    int visiblePointCount() const { return m_lastVisibleCount; }
    int visibleFrameCount() const;
    QString statusText() const;               // 顶部统计文字

signals:
    void trackFocusChanged(int batchNo, TrackSource source, qint64 t0, qint64 t1);
    void trackActivated(int batchNo, TrackSource source, qint64 t0, qint64 t1);
    void cameraChanged();
    void hoverChanged();

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
    QRectF plotRect() const;
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
    Point3dTrackSource m_trackSource = Point3dTrackSource::Associated;
    bool m_showDetections = true;
    bool m_showTracks = true;
    bool m_showLabels = true;
    int m_highlightedBatch = -1;
    double m_trackLineWidth = 2.0;

    Camera m_cam;
    Basis m_basis;
    Limits m_limits[3];

    bool m_dragging = false;
    QPoint m_lastMouse;
    int m_hoverPoint = -1;            // m_projected 下标
    int m_hoverTrack = -1;            // m_tracks 下标
    int m_lastVisibleCount = 0;
    int m_lastVisibleFrames = 0;

    QVector<ProjectedPoint> m_projected;
    QVector<ProjectedTrack> m_tracks;
};

#endif // POINT3DCANVAS_H
