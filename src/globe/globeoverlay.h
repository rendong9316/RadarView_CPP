#ifndef GLOBEOVERLAY_H
#define GLOBEOVERLAY_H

#include <QHash>
#include <QPixmap>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>
#include <QWidget>

#include "trackanalysis.h"
#include "trackdiag.h"

class DrawingManager;
class FlagStore;
class GlobeCamera;
class GlobeDrawController;
class QPainter;
class RadarSiteStore;
class RulerState;
class SpatialLasso;
class TrackLayer;
class TrackStore;
struct Track;

// 地球 2D 叠加层（透明子控件，纯 QPainter 光栅绘制，不碰 GL）。
// QPainter 直接画在 QOpenGLWidget 上会走 GL 绘制引擎，ANGLE（D3D11/D3D9）下会产生 GL 错误；
// 子控件由 backing store 和 GL 画面合成，对鼠标事件透明。
class GlobeOverlay : public QWidget
{
public:
    // 同航迹平滑/关联/预测同屏滤波叠加选项
    struct FilterOverlayState {
        bool enabled = false;
        bool showAssociated = true;
        bool showPredicted = true;
        bool showResiduals = true;
    };

    struct DiagOrphanPoint {
        double rangeKm;
        double azDeg;
        float snr;
        int frameId;
        DiagOrphanPoint(double r = 0.0, double a = 0.0, float s = 0.0f, int f = 0)
            : rangeKm(r), azDeg(a), snr(s), frameId(f) {}
    };

    struct Context {
        const GlobeCamera *camera = nullptr;
        const TrackLayer *trackLayer = nullptr;
        const TrackStore *trackStore = nullptr;
        const FlagStore *flags = nullptr;
        const RulerState *ruler = nullptr;
        const SpatialLasso *lasso = nullptr;
        const DrawingManager *draw = nullptr;
        const GlobeDrawController *drawCtrl = nullptr;
        bool cursorValid = false;
        double cursorLonRad = 0.0;
        double cursorLatRad = 0.0;
        int tipTrack = -1;
        int tipPoint = -1;
        bool focusValid = false;
        double focusLon = 0.0;
        double focusLat = 0.0;
        QString focusLabel;
        FilterOverlayState filterOverlay;
        bool compareLinksVisible = false;
        int compareMaxIdx = -1;
        const QVector<trackanalysis::TrackDiffSample> *compareSamples = nullptr;
        const RadarSiteStore *radarSites = nullptr;
        // 航迹诊断叠加：断航波门尸检 / 断批重起桥接 / 未关联漏网点迹链
        bool gateAutopsyVisible = false;
        const trackdiag::GateAutopsyResult *gateAutopsy = nullptr;
        bool fragmentBridgeVisible = false;
        int fragmentIdxA = -1;
        int fragmentIdxB = -1;
        bool orphanClusterVisible = false;
        int orphanClusterId = 0;
        int orphanPointCount = 0;
        float orphanMeanSnr = 0.0f;
        const QVector<DiagOrphanPoint> *orphanPoints = nullptr;
    };

    explicit GlobeOverlay(QWidget *parent = nullptr);

    void setContext(const Context &ctx) { m_ctx = ctx; }
    void setWanted(bool wanted) { m_wanted = wanted; }
    bool isWanted() const { return m_wanted; }

    void setShowLabels(bool on) { m_showLabels = on; }
    bool showLabels() const { return m_showLabels; }

    void setFlagScale(double s) { m_flagScale = s; m_flagIcon = QPixmap(); }
    double flagScale() const { return m_flagScale; }

    void setLassoVertexPx(double px) { m_lassoVertexPx = qBound(1.0, px, 16.0); }
    double lassoVertexPx() const { return m_lassoVertexPx; }

    QRectF flagIconRect(const QPointF &anchor) const;
    QString flagAt(const QPointF &pos, const GlobeCamera &cam, const FlagStore *flags) const;

    static QString trackLabel(const Track &t);

    // ---- 自检统计 ----
    int labelsDrawn() const { return m_labelsDrawn; }
    int flagsDrawn() const { return m_flagsDrawn; }
    int rulerMarkersDrawn() const { return m_rulerMarkersDrawn; }
    int lassoVerticesDrawn() const { return m_lassoVerticesDrawn; }
    int featuresDrawn() const { return m_featuresDrawn; }
    int drawPreviewSegments() const { return m_drawPreviewSegments; }
    int filterOverlayItemsDrawn() const { return m_filterOverlayItems; }
    int compareLinksDrawn() const { return m_compareLinksDrawn; }
    int radarPpiPrimitivesDrawn() const { return m_radarPpiPrimitives; }
    int diagnosticsItemsDrawn() const { return m_diagnosticsItemsDrawn; }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    void drawRuler(QPainter &p);
    void drawRadarSites(QPainter &p);
    void drawFlags(QPainter &p);
    void drawLabels(QPainter &p);
    void drawLasso(QPainter &p);
    void drawFeatures(QPainter &p);
    void drawInProgress(QPainter &p);
    void drawBand(QPainter &p);
    void drawFilterOverlay(QPainter &p);
    void drawCompareLinks(QPainter &p);
    void drawDiagnosticsOverlay(QPainter &p);
    void drawFocusPoint(QPainter &p);
    void drawPointTip(QPainter &p);

    Context m_ctx;
    bool m_wanted = false;
    bool m_showLabels = false;
    double m_flagScale = 1.2;
    double m_lassoVertexPx = 3.0;

    int m_labelsDrawn = 0;
    int m_flagsDrawn = 0;
    int m_rulerMarkersDrawn = 0;
    int m_lassoVerticesDrawn = 0;
    int m_featuresDrawn = 0;
    int m_drawPreviewSegments = 0;
    int m_filterOverlayItems = 0;
    int m_compareLinksDrawn = 0;
    int m_radarPpiPrimitives = 0;
    int m_diagnosticsItemsDrawn = 0;

    QHash<QString, QPixmap> m_labelCache;   // 标签文字 + 颜色 -> 带描边的位图
    QPixmap m_flagIcon;
};

#endif // GLOBEOVERLAY_H
