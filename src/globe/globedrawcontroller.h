#ifndef GLOBEDRAWCONTROLLER_H
#define GLOBEDRAWCONTROLLER_H

#include <QColor>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QPolygonF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QVector>
#include <functional>

#include "drawingmanager.h"

class GlobeCamera;
class QMouseEvent;

// 地球绘图状态机与屏幕空间命中/吸附控制器。
// 管理画笔工具、进行中的笔画/橡皮筋框、选中/悬停要素以及顶点吸附逻辑，
// 鼠标事件在绘图模式激活时由 GlobeWidget 委托到这里处理。
class GlobeDrawController : public QObject
{
    Q_OBJECT
public:
    struct Host {
        std::function<QSize()> viewport;
        std::function<void()> requestUpdate;
        std::function<void()> requestStatus;
        std::function<void(const QPoint &, const QPoint &)> panPixels;
        std::function<void(bool, double, double)> setCursorGeo;  // valid, lonRad, latRad
        std::function<void(Qt::CursorShape)> setCursorShape;
        std::function<void()> unsetCursorShape;
    };

    explicit GlobeDrawController(const GlobeCamera *camera, QObject *parent = nullptr);

    void setHost(const Host &host) { m_host = host; }
    void setDrawingManager(DrawingManager *drawing);
    DrawingManager *drawingManager() const { return m_draw; }

    void setTool(DrawTool t);
    DrawTool tool() const { return m_tool; }
    bool isActive() const { return m_tool != DrawTool::None; }

    void setColor(const QColor &c) { m_drawColor = c; }
    QColor color() const { return m_drawColor; }

    bool cancel();
    int selectedFeature() const { return m_selectedFeature; }
    void setSelectedFeature(int id);
    bool deleteSelected();
    int hoverFeature() const { return m_hoverFeature; }

    // 供叠加层绘制进行中的预览与吸附提示
    const QVector<QPointF> &stroke() const { return m_stroke; }
    int strokePointCount() const { return m_stroke.size(); }
    bool strokeActive() const { return m_strokeActive; }
    double pressLat() const { return m_pressLat; }
    double pressLon() const { return m_pressLon; }
    bool boxSelecting() const { return m_bandActive; }
    const QRect &bandRect() const { return m_bandRect; }
    bool snapActive() const { return m_snapActive; }
    const QPointF &snapPos() const { return m_snapPos; }
    const QColor &snapColor() const { return m_snapColor; }
    bool hasOverlayContent() const;

    // 绘图模式下的鼠标事件处理（仅在 isActive() 为 true 时调用）
    void mousePress(QMouseEvent *e);
    void mouseMove(QMouseEvent *e);
    void mouseRelease(QMouseEvent *e);
    void mouseDoubleClick(QMouseEvent *e);

    // 供叠加层与命中测试共用的闭合环投影
    static QPolygonF projectedRing(const GlobeCamera &cam, const QSize &viewport,
                                   const DrawingFeature &f, bool *complete);

signals:
    void drawTextRequested(double lat, double lon, const QPoint &globalPos);
    void drawFillRequested(int featureId);
    void drawFeatureAdded(int featureId);
    void drawFeatureEditRequested(int featureId);
    void drawSelectionChanged(int featureId);
    void drawMessage(const QString &text);

private:
    bool snappingTool() const;
    bool snapToFeature(const QPointF &pos, double *lonDeg, double *latDeg, QPointF *screen) const;
    void finishDrawStroke(double lat, double lon);
    void closeDrawPolygon();
    void applyBoxSelect();
    int featureAtScreen(const QPointF &pos, double tolPx) const;
    int closedFeatureAt(const QPointF &pos) const;
    bool featureHitsRect(const DrawingFeature &f, const QRectF &rect) const;

    const GlobeCamera *m_camera = nullptr;
    DrawingManager *m_draw = nullptr;
    Host m_host;

    DrawTool m_tool = DrawTool::None;
    QColor m_drawColor = QColor(0xf5, 0x9e, 0x0b);
    QVector<QPointF> m_stroke;              // 正在画的顶点（WGS84 度）
    bool m_strokeActive = false;
    double m_pressLat = 0.0, m_pressLon = 0.0;
    bool m_bandActive = false;
    QRect m_bandRect;
    int m_hoverFeature = 0;
    int m_selectedFeature = 0;
    bool m_snapActive = false;              // 当前光标点吸附到了已有顶点
    QPointF m_snapPos;                      // 吸附到的屏幕位置（画小方块提示）
    QColor m_snapColor = QColor(0x10, 0xb9, 0x81);

    // 绘图模式下右键拖拽旋转的状态
    bool m_dragging = false;
    bool m_clickCandidate = false;
    QPoint m_lastPos;
    QPoint m_pressPos;
};

#endif // GLOBEDRAWCONTROLLER_H
