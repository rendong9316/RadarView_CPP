#include "globewidget.h"
#include "geocalc.h"
#include "globeoverlay.h"
#include "radarsite.h"
#include "maptools.h"
#include "netmap.h"
#include "spatiallasso.h"
#include "track.h"

#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QToolTip>
#include <QWheelEvent>
#include <QtMath>
#include <cmath>

namespace {

const float kPickTolPx = 6.0f;    // 鼠标离航迹多近算命中
const int kClickSlopPx = 4;       // 按下到松开移动不超过它算单击

} // namespace

GlobeWidget::GlobeWidget(QWidget *parent)
    : QOpenGLWidget(parent),
      m_drawCtrl(&m_camera, this)
{
    setMouseTracking(true);
    setMinimumSize(200, 200);
    m_fpsClock.start();
    m_statusTimer.setSingleShot(true);
    m_statusTimer.setInterval(0);
    connect(&m_statusTimer, &QTimer::timeout, this, [this]() {
        emit viewStatusChanged(m_camera.altitudeKm(),
                               m_cursorValid ? qRadiansToDegrees(m_cursorLon) : 0.0,
                               m_cursorValid ? qRadiansToDegrees(m_cursorLat) : 0.0, fps(),
                               m_cursorValid);
    });

    GlobeDrawController::Host host;
    host.viewport = [this]() { return size(); };
    host.requestUpdate = [this]() { update(); };
    host.requestStatus = [this]() { emitStatus(); };
    host.panPixels = [this](const QPoint &from, const QPoint &to) { panPixels(from, to); };
    host.setCursorGeo = [this](bool valid, double lonRad, double latRad) {
        m_cursorValid = valid;
        if (valid) {
            m_cursorLon = lonRad;
            m_cursorLat = latRad;
        }
        syncOverlayContext();
    };
    host.setCursorShape = [this](Qt::CursorShape shape) { setCursor(shape); };
    host.unsetCursorShape = [this]() { unsetCursor(); };
    m_drawCtrl.setHost(host);

    connect(&m_drawCtrl, &GlobeDrawController::drawTextRequested, this, &GlobeWidget::drawTextRequested);
    connect(&m_drawCtrl, &GlobeDrawController::drawFillRequested, this, &GlobeWidget::drawFillRequested);
    connect(&m_drawCtrl, &GlobeDrawController::drawFeatureAdded, this, &GlobeWidget::drawFeatureAdded);
    connect(&m_drawCtrl, &GlobeDrawController::drawFeatureEditRequested, this, &GlobeWidget::drawFeatureEditRequested);
    connect(&m_drawCtrl, &GlobeDrawController::drawSelectionChanged, this, &GlobeWidget::drawSelectionChanged);
    connect(&m_drawCtrl, &GlobeDrawController::drawMessage, this, &GlobeWidget::drawMessage);

    m_overlay = new GlobeOverlay(this);
    m_overlay->setGeometry(rect());
    syncOverlayContext();
    m_overlay->show();
}

GlobeWidget::~GlobeWidget()
{
    cleanupGL();
}

// 场景坐标系由底图决定：高德瓦片网格是 GCJ-02；天地图和离线 mbtiles 是 WGS84。
// 离线包可以在 metadata 里写 crs=GCJ-02 声明（有些用高德瓦片抓的包就是加密坐标）。
bool GlobeWidget::sceneIsGcj() const
{
    if (m_online)
        return m_online->source() == NetTileSource::Source::Amap;
    return m_tiles && m_tiles->isGcj();
}

void GlobeWidget::toScene(double lonWgs, double latWgs, double *lonScene, double *latScene) const
{
    m_camera.toScene(lonWgs, latWgs, lonScene, latScene);
}

void GlobeWidget::fromScene(double lonScene, double latScene, double *lonWgs, double *latWgs) const
{
    m_camera.fromScene(lonScene, latScene, lonWgs, latWgs);
}

void GlobeWidget::applySceneCrs()
{
    const bool gcj = sceneIsGcj();
    if (!m_camera.setSceneGcj(gcj))
        return;
    m_trackLayer.setSceneGcj(gcj);      // 重建航迹几何：坐标系变了，旧顶点全部失效
    update();
    emitStatus();
}

bool GlobeWidget::openTiles(const QString &path, QString *error)
{
    std::unique_ptr<TileSource> src(new TileSource);
    if (!src->open(path, error))
        return false;

    // 打开本地包 = 只要这份离线数据：把在线源整个关掉。
    // 否则 m_online 还活着，loadTexture() 会走网络分支，屏幕上看到的是
    // 在线瓦片或它的磁盘缓存，本地包反而没被用上。
    if (m_online) {
        delete m_online;
        m_online = nullptr;
    }

    if (m_glReady) {
        makeCurrent();
        m_surface.clearTextures();
        doneCurrent();
    }
    m_surface.clearTileState();     // 上一个源还在路上的瓦片不能漏进新包
    m_tiles = std::move(src);
    applySceneCrs();
    m_surface.updateCapColors(m_tiles.get());
    update();
    emitStatus();
    return true;
}

QString GlobeWidget::tileName() const
{
    if (m_online)
        return m_online->name();
    return m_tiles ? m_tiles->name() : QString();
}

QString GlobeWidget::mapSourceName() const
{
    return tileName();
}

bool GlobeWidget::onlineBusy() const
{
    return m_online && m_online->isBusy();
}

// 打开在线底图：本地 mbtiles 不释放，网络瓦片缺失时还能拿它兜底
bool GlobeWidget::openOnlineMap(const QString &token, bool useAmap, QString *error)
{
    if (token.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("没有填写天地图 key");
        return false;
    }
    if (!m_online) {
        m_online = new NetTileSource(this);
        connect(m_online, &NetTileSource::tileReady, this,
                [this](int z, int x, int y, const QImage &img) {
                    // 网络回调在 GUI 线程，纹理上传留到 paintGL 里做
                    m_surface.queuePendingTile(z, x, y, img);
                    update();
                });
        connect(m_online, &NetTileSource::tileFailed, this, [this](int z, int x, int y) {
            m_surface.markMissing(z, x, y);
            update();
        });
    }
    m_online->setSource(useAmap ? NetTileSource::Source::Amap : NetTileSource::Source::Tianditu);
    m_online->setKey(token);

    if (m_glReady) {
        makeCurrent();
        m_surface.clearTextures();
        doneCurrent();
    }
    m_surface.clearTileState();
    applySceneCrs();
    m_surface.updateCapColors(m_tiles.get());
    update();
    emitStatus();
    return true;
}

void GlobeWidget::closeOnlineMap()
{
    if (!m_online)
        return;
    delete m_online;
    m_online = nullptr;
    m_surface.clearTileState();
    if (m_glReady) {
        makeCurrent();
        m_surface.clearTextures();
        doneCurrent();
    }
    applySceneCrs();
    m_surface.updateCapColors(m_tiles.get());
    update();
    emitStatus();
}

void GlobeWidget::resetView()
{
    m_camera.reset();
    update();
    emit cameraChanged();
}

void GlobeWidget::cameraView(double *lonDeg, double *latDeg, double *altKm) const
{
    m_camera.view(lonDeg, latDeg, altKm);
}

void GlobeWidget::setCameraView(double lonDeg, double latDeg, double altKm)
{
    lookAt(lonDeg, latDeg, altKm);
}

void GlobeWidget::zoomBy(double steps)
{
    m_camera.zoomBy(steps);
    update();
    emit cameraChanged();
}

void GlobeWidget::panPixels(const QPoint &from, const QPoint &to)
{
    m_camera.panPixels(from, to, size());
    update();
}

bool GlobeWidget::geoAt(const QPointF &pos, double *lonDeg, double *latDeg)
{
    return m_camera.geoAt(pos, size(), lonDeg, latDeg);
}

bool GlobeWidget::screenPos(double lonDeg, double latDeg, double altM, QPointF *pos)
{
    return m_camera.screenPos(lonDeg, latDeg, altM, size(), pos);
}

bool GlobeWidget::visiblePos(double lonDeg, double latDeg, double altM, QPointF *pos)
{
    return m_camera.visiblePos(lonDeg, latDeg, altM, size(), pos);
}

void GlobeWidget::sampleLonLatLine(double lat1, double lon1, double lat2, double lon2,
                                   QVector<QPointF> *pts, QVector<bool> *vis)
{
    m_camera.sampleLonLatLine(lat1, lon1, lat2, lon2, size(), pts, vis);
}

int GlobeWidget::trackAt(const QPointF &pos)
{
    m_camera.updateMatrices(size());
    return m_trackLayer.pick(m_camera.mvp(), m_camera.eye(), QSizeF(width(), height()), pos, kPickTolPx);
}

void GlobeWidget::lookAt(double lonDeg, double latDeg, double altKm)
{
    m_camera.lookAt(lonDeg, latDeg, altKm);
    update();
    emit cameraChanged();
}

void GlobeWidget::fitGeoBounds(double minLon, double maxLon, double minLat, double maxLat)
{
    if (!std::isfinite(minLon) || !std::isfinite(maxLon)
            || !std::isfinite(minLat) || !std::isfinite(maxLat))
        return;
    const double cLon = 0.5 * (minLon + maxLon);
    const double cLat = 0.5 * (minLat + maxLat);
    const double spanKm = geocalc::vincentyKm(minLat, minLon, maxLat, maxLon);
    const double aspect = height() > 0 ? qMax(0.5, double(width()) / double(height())) : 1.5;
    const double halfFovTan = std::tan(qDegreesToRadians(double(GlobeCamera::kFovDeg) * 0.5));
    const double needed = spanKm > 1e-3 ? (0.5 * spanKm / qMax(0.1, halfFovTan * qMin(1.0, aspect))) * 1.45 : 30.0;
    lookAt(cLon, cLat, qBound(8.0, needed, 8000.0));
}

void GlobeWidget::setFocusPoint(double lonDeg, double latDeg, const QString &label)
{
    m_focusValid = std::isfinite(lonDeg) && std::isfinite(latDeg);
    m_focusLon = lonDeg;
    m_focusLat = latDeg;
    m_focusLabel = label;
    syncOverlayContext();
    update();
}

void GlobeWidget::clearFocusPoint()
{
    if (!m_focusValid)
        return;
    m_focusValid = false;
    m_focusLabel.clear();
    syncOverlayContext();
    update();
}

void GlobeWidget::setFilterOverlay(const GlobeOverlay::FilterOverlayState &st)
{
    m_filterOverlay = st;
    syncOverlayContext();
    update();
}

void GlobeWidget::setCompareLinks(const QVector<trackanalysis::TrackDiffSample> &samples, int maxErrIdx, bool visible)
{
    m_compareSamples = samples;
    m_compareMaxIdx = maxErrIdx;
    m_compareLinksVisible = visible && !m_compareSamples.isEmpty();
    syncOverlayContext();
    update();
}

void GlobeWidget::setGateAutopsyOverlay(const trackdiag::GateAutopsyResult &res, bool visible)
{
    m_gateAutopsy = res;
    m_gateAutopsyVisible = visible && !m_gateAutopsy.frames.isEmpty();
    syncOverlayContext();
    update();
}

void GlobeWidget::setFragmentBridgeOverlay(int idxA, int idxB, bool visible)
{
    m_fragmentIdxA = idxA;
    m_fragmentIdxB = idxB;
    m_fragmentBridgeVisible = visible && idxA >= 0 && idxB >= 0;
    syncOverlayContext();
    update();
}

void GlobeWidget::setOrphanClusterOverlay(const trackdiag::OrphanDetCluster &cluster,
                                          const Point3dDataset *detData, bool visible)
{
    m_orphanPoints.clear();
    m_orphanClusterId = cluster.clusterId;
    m_orphanPointCount = cluster.pointCount;
    m_orphanMeanSnr = cluster.meanSnr;
    auto toKm = [](double r) -> double {
        return r > 20000.0 ? (r / 1000.0) : r;
    };
    if (visible) {
        if (detData && !cluster.framePointIndices.isEmpty()) {
            m_orphanPoints.reserve(cluster.framePointIndices.size());
            for (const QPair<int, int> &fp : cluster.framePointIndices) {
                if (fp.first < 0 || fp.first >= detData->frames.size())
                    continue;
                const DetPointFrame &df = detData->frames.at(fp.first);
                if (fp.second < 0 || fp.second >= df.points.size())
                    continue;
                const DetPointItem &dp = df.points.at(fp.second);
                m_orphanPoints.append(GlobeOverlay::DiagOrphanPoint(
                    toKm(double(dp.range)), double(dp.az), dp.snr, int(std::lround(df.frameId))));
            }
        }
        if (m_orphanPoints.isEmpty() && std::isfinite(cluster.meanRange) && std::isfinite(cluster.meanAz)
                && cluster.meanRange > 0.0f) {
            m_orphanPoints.append(GlobeOverlay::DiagOrphanPoint(
                toKm(double(cluster.meanRange)), double(cluster.meanAz), cluster.meanSnr, cluster.frameStart));
        }
    }
    m_orphanClusterVisible = visible && !m_orphanPoints.isEmpty();
    syncOverlayContext();
    update();
}

void GlobeWidget::clearDiagnosticsOverlay()
{
    m_gateAutopsy = trackdiag::GateAutopsyResult();
    m_gateAutopsyVisible = false;
    m_fragmentIdxA = -1;
    m_fragmentIdxB = -1;
    m_fragmentBridgeVisible = false;
    m_orphanPoints.clear();
    m_orphanClusterVisible = false;
    syncOverlayContext();
    update();
}

int GlobeWidget::filterOverlayItemsDrawn() const
{
    return m_overlay ? m_overlay->filterOverlayItemsDrawn() : 0;
}

int GlobeWidget::compareLinksDrawn() const
{
    return m_overlay ? m_overlay->compareLinksDrawn() : 0;
}

int GlobeWidget::diagnosticsItemsDrawn() const
{
    return m_overlay ? m_overlay->diagnosticsItemsDrawn() : 0;
}

void GlobeWidget::setTrackStore(const TrackStore *store)
{
    m_trackStore = store;
    m_trackLayer.setStore(store);
    syncOverlayContext();
    update();
}

void GlobeWidget::setRadarSites(RadarSiteStore *sites)
{
    m_radarSites = sites;
    if (m_radarSites)
        connect(m_radarSites, &RadarSiteStore::changed, this, [this]() {
            syncOverlayContext();
            update();
        });
    syncOverlayContext();
    update();
}

int GlobeWidget::radarPpiPrimitivesDrawn() const
{
    return m_overlay ? m_overlay->radarPpiPrimitivesDrawn() : 0;
}

void GlobeWidget::setMapTools(FlagStore *flags, RulerState *ruler)
{
    m_flags = flags;
    m_ruler = ruler;
    connect(flags, &FlagStore::changed, this, [this]() { update(); });
    connect(ruler, &RulerState::changed, this, [this]() { update(); });
    connect(ruler, &RulerState::mouseMoved, this, [this]() { update(); });
    syncOverlayContext();
}

void GlobeWidget::setSpatialLasso(SpatialLasso *lasso)
{
    m_lasso = lasso;
    if (m_lasso) {
        connect(m_lasso, &SpatialLasso::changed, this, [this]() {
            syncOverlayContext();
            m_overlay->update();
            update();
        });
        connect(m_lasso, &SpatialLasso::mouseMoved, this, [this]() {
            syncOverlayContext();
            m_overlay->update();
        });
    }
    syncOverlayContext();
}

void GlobeWidget::setDrawingManager(DrawingManager *drawing)
{
    m_draw = drawing;
    m_drawCtrl.setDrawingManager(drawing);
    syncOverlayContext();
}

void GlobeWidget::setDrawTool(DrawTool t)
{
    m_drawCtrl.setTool(t);
    m_dragging = false;
    m_clickCandidate = false;
}

bool GlobeWidget::cancelDrawing()
{
    m_dragging = false;
    m_clickCandidate = false;
    return m_drawCtrl.cancel();
}

void GlobeWidget::setSelectedFeature(int id)
{
    m_drawCtrl.setSelectedFeature(id);
}

bool GlobeWidget::deleteSelected()
{
    return m_drawCtrl.deleteSelected();
}

int GlobeWidget::drawPreviewSegments() const
{
    return m_overlay ? m_overlay->drawPreviewSegments() : 0;
}

void GlobeWidget::setShowLabels(bool on)
{
    if (m_overlay)
        m_overlay->setShowLabels(on);
    update();
}

bool GlobeWidget::showLabels() const
{
    return m_overlay && m_overlay->showLabels();
}

void GlobeWidget::setFlagScale(double s)
{
    if (m_overlay)
        m_overlay->setFlagScale(s);
    update();
}

double GlobeWidget::flagScale() const
{
    return m_overlay ? m_overlay->flagScale() : 1.2;
}

void GlobeWidget::setLassoVertexPx(double px)
{
    if (m_overlay)
        m_overlay->setLassoVertexPx(px);
    update();
}

double GlobeWidget::lassoVertexPx() const
{
    return m_overlay ? m_overlay->lassoVertexPx() : 3.0;
}

int GlobeWidget::labelsDrawn() const
{
    return m_overlay ? m_overlay->labelsDrawn() : 0;
}

int GlobeWidget::flagsDrawn() const
{
    return m_overlay ? m_overlay->flagsDrawn() : 0;
}

int GlobeWidget::rulerMarkersDrawn() const
{
    return m_overlay ? m_overlay->rulerMarkersDrawn() : 0;
}

int GlobeWidget::lassoVerticesDrawn() const
{
    return m_overlay ? m_overlay->lassoVerticesDrawn() : 0;
}

int GlobeWidget::featuresDrawn() const
{
    return m_overlay ? m_overlay->featuresDrawn() : 0;
}

QString GlobeWidget::flagAt(const QPointF &pos)
{
    return m_overlay ? m_overlay->flagAt(pos, m_camera, m_flags) : QString();
}

void GlobeWidget::resetInteraction()
{
    m_tipTrack = m_tipPoint = -1;
    m_trackLayer.setHoveredTrack(-1);
    QToolTip::hideText();
    unsetCursor();
    syncOverlayContext();
    update();
}

bool GlobeWidget::pointHover(int *track, int *point) const
{
    *track = m_tipTrack;
    *point = m_tipPoint;
    return m_tipTrack >= 0;
}

QString GlobeWidget::trackLabel(const Track &t)
{
    return GlobeOverlay::trackLabel(t);
}

void GlobeWidget::syncOverlayContext()
{
    if (!m_overlay)
        return;
    GlobeOverlay::Context ctx;
    ctx.camera = &m_camera;
    ctx.trackLayer = &m_trackLayer;
    ctx.trackStore = m_trackStore;
    ctx.flags = m_flags;
    ctx.ruler = m_ruler;
    ctx.lasso = m_lasso;
    ctx.draw = m_draw;
    ctx.drawCtrl = &m_drawCtrl;
    ctx.cursorValid = m_cursorValid;
    ctx.cursorLonRad = m_cursorLon;
    ctx.cursorLatRad = m_cursorLat;
    ctx.tipTrack = m_tipTrack;
    ctx.tipPoint = m_tipPoint;
    ctx.focusValid = m_focusValid;
    ctx.focusLon = m_focusLon;
    ctx.focusLat = m_focusLat;
    ctx.focusLabel = m_focusLabel;
    ctx.filterOverlay = m_filterOverlay;
    ctx.compareLinksVisible = m_compareLinksVisible;
    ctx.compareMaxIdx = m_compareMaxIdx;
    ctx.compareSamples = &m_compareSamples;
    ctx.radarSites = m_radarSites;
    ctx.gateAutopsyVisible = m_gateAutopsyVisible;
    ctx.gateAutopsy = &m_gateAutopsy;
    ctx.fragmentBridgeVisible = m_fragmentBridgeVisible;
    ctx.fragmentIdxA = m_fragmentIdxA;
    ctx.fragmentIdxB = m_fragmentIdxB;
    ctx.orphanClusterVisible = m_orphanClusterVisible;
    ctx.orphanClusterId = m_orphanClusterId;
    ctx.orphanPointCount = m_orphanPointCount;
    ctx.orphanMeanSnr = m_orphanMeanSnr;
    ctx.orphanPoints = &m_orphanPoints;
    m_overlay->setContext(ctx);
}

void GlobeWidget::resizeEvent(QResizeEvent *e)
{
    QOpenGLWidget::resizeEvent(e);
    if (m_overlay)
        m_overlay->setGeometry(rect());
}

double GlobeWidget::altitudeKm() const
{
    return m_camera.altitudeKm();
}

int GlobeWidget::tileMaxZoom() const
{
    if (m_online)
        return m_online->maxZoom();
    return m_tiles ? m_tiles->maxZoom() : -1;
}

QString GlobeWidget::debugInfo() const
{
    return QStringLiteral("gl=[%1] tiles=[%2 z%3-%4] drawn=%5 drawnMaxZ=%6 textures=%7 missing=%8 pending=%9")
               .arg(m_glInfo)
               .arg(m_tiles ? m_tiles->name() : QStringLiteral("none"))
               .arg(m_tiles ? m_tiles->minZoom() : -1)
               .arg(m_tiles ? m_tiles->maxZoom() : -1)
               .arg(m_surface.lastDrawn()).arg(m_surface.lastMaxZ())
               .arg(m_surface.textureCount()).arg(m_surface.missingCount()).arg(m_surface.pendingCount())
         + QStringLiteral(" glErrors=%1 alt=%2km lon=%3 lat=%4 crs=%5")
               .arg(m_glErrors)
               .arg(m_camera.altitudeKm(), 0, 'f', 1)
               .arg(qRadiansToDegrees(m_camera.camLonRad()), 0, 'f', 3)
               .arg(qRadiansToDegrees(m_camera.camLatRad()), 0, 'f', 3)
               .arg(coordSystemName());
}

// ---------------------------------------------------------------
//  OpenGL 资源与绘制
// ---------------------------------------------------------------
void GlobeWidget::initializeGL()
{
    initializeOpenGLFunctions();
    connect(context(), &QOpenGLContext::aboutToBeDestroyed,
            this, &GlobeWidget::cleanupGL, Qt::UniqueConnection);

    const bool ok = m_surface.initialize(this, context());
    m_glInfo = m_surface.glInfo();
    if (!ok)
        return;

    if (!m_trackLayer.initialize(this))
        m_glInfo += QStringLiteral(" | track shader error: ") + m_trackLayer.shaderLog();
    m_trackLayer.setStore(m_trackStore);

    m_glReady = true;
}

void GlobeWidget::cleanupGL()
{
    if (!m_glReady)
        return;
    makeCurrent();
    m_surface.cleanup();
    m_trackLayer.cleanup();
    m_glReady = false;
    doneCurrent();
}

void GlobeWidget::paintGL()
{
    ++m_frame;
    // 上一帧 QPainter 叠加层可能留下的状态
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, int(width() * devicePixelRatioF()), int(height() * devicePixelRatioF()));
    glClearColor(0.118f, 0.118f, 0.118f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!m_surface.isReady() || width() <= 0 || height() <= 0)
        return;

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE);
    m_camera.updateMatrices(size());
    m_surface.draw(m_camera, m_tiles.get(), m_online, context(), m_frame);

    // 航迹贴地绘制：关掉深度测试（否则与瓦片同深度会 z-fighting），
    // 地球背面的遮挡由着色器按相机位置做地平线剔除
    m_trackLayer.setEye(m_camera.eye());
    m_trackLayer.draw(m_camera.mvp(),
                      QSize(int(width() * devicePixelRatioF()), int(height() * devicePixelRatioF())),
                      float(devicePixelRatioF()));

    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i)
        ++m_glErrors;

    // 文字和图标由叠加层控件画（有内容时，或刚从有内容变为没有、需要擦掉时才重画）
    const bool overlay = (showLabels() && m_trackLayer.visibleTrackCount() > 0)
                         || (m_flags && !m_flags->flags().isEmpty())
                         || (m_ruler && m_ruler->isActive() && !m_ruler->waypoints().isEmpty())
                         || (m_lasso && !m_lasso->vertices().isEmpty())
                         || (m_draw && !m_draw->features().isEmpty())
                         || m_drawCtrl.hasOverlayContent()
                         || m_focusValid
                         || m_filterOverlay.enabled
                         || m_compareLinksVisible
                         || m_gateAutopsyVisible
                         || m_fragmentBridgeVisible
                         || m_orphanClusterVisible
                         || (m_radarSites && m_radarSites->ppiVisible() && !m_radarSites->sites().isEmpty())
                         || m_tipTrack >= 0;
    if (overlay || (m_overlay && m_overlay->isWanted())) {
        syncOverlayContext();
        m_overlay->setWanted(overlay);
        m_overlay->update();
    }

    trackFps();
    emitStatus();
    if (m_surface.pendingCount() > 0)
        update();
}

// ---------------------------------------------------------------
//  交互
// ---------------------------------------------------------------
void GlobeWidget::mousePressEvent(QMouseEvent *e)
{
    if (m_drawCtrl.isActive()) {
        m_drawCtrl.mousePress(e);
        return;
    }
    // 套索绘制中：左键不旋转地球。按下先只记录，松开时若是单击就放一个顶点；
    // 按住移动超过阈值则自动转为自由手绘（这样逐点框选和手绘都不需要切模式）。
    if (m_lasso && m_lasso->isActive() && e->button() == Qt::LeftButton) {
        double lon = 0.0, lat = 0.0;
        m_dragging = false;
        m_clickCandidate = false;
        m_lassoPressValid = geoAt(e->pos(), &lon, &lat);
        if (m_lassoPressValid) {
            m_lassoPressLat = lat;
            m_lassoPressLon = lon;
            m_clickCandidate = true;
            m_pressPos = e->pos();
        }
        return;
    }
    if (e->button() == Qt::LeftButton) {
        m_dragging = true;
        m_clickCandidate = true;
        m_lastPos = m_pressPos = e->pos();
    }
}

void GlobeWidget::mouseMoveEvent(QMouseEvent *e)
{
    if (m_drawCtrl.isActive()) {
        m_drawCtrl.mouseMove(e);
        return;
    }
    // 套索绘制中：只更新预览线和自由手绘轨迹，不拖拽地球
    if (m_lasso && m_lasso->isActive()) {
        double lon = 0.0, lat = 0.0;
        const bool hit = geoAt(e->pos(), &lon, &lat);
        m_lasso->setMouse(hit, lat, lon);
        if (m_lasso->isDrawingFreehand()) {
            if (hit)
                m_lasso->addFreehandPoint(lat, lon);
        } else if (m_clickCandidate && m_lassoPressValid &&
                   (e->pos() - m_pressPos).manhattanLength() > kClickSlopPx) {
            // 从空白开始拖动 → 自由手绘；已有顶点时拖动不改变已画内容
            m_clickCandidate = false;
            if (m_lasso->vertices().isEmpty()) {
                m_lasso->beginFreehand(m_lassoPressLat, m_lassoPressLon);
                if (hit)
                    m_lasso->addFreehandPoint(lat, lon);
            }
        }
        m_camera.updateMatrices(size());
        m_cursorValid = hit;
        if (hit) {
            m_cursorLon = qDegreesToRadians(lon);
            m_cursorLat = qDegreesToRadians(lat);
        }
        emitStatus();
        return;
    }
    if (m_dragging) {
        if (m_clickCandidate && (e->pos() - m_pressPos).manhattanLength() > kClickSlopPx) {
            m_clickCandidate = false;
            setCursor(Qt::ClosedHandCursor);
            QToolTip::hideText();
            m_trackLayer.setHoveredTrack(-1);
        }
        if (!m_clickCandidate) {
            panPixels(m_lastPos, e->pos());
            m_lastPos = e->pos();
        }
    } else {
        updateHover(e->pos(), e->globalPos());
    }
    m_camera.updateMatrices(size());
    double clon = 0.0, clat = 0.0;
    m_cursorValid = m_camera.pick(e->pos(), size(), &clon, &clat);
    if (m_cursorValid) {              // 场景坐标 → WGS84，供状态栏/标尺使用
        double wlon = 0.0, wlat = 0.0;
        fromScene(qRadiansToDegrees(clon), qRadiansToDegrees(clat), &wlon, &wlat);
        m_cursorLon = qDegreesToRadians(wlon);
        m_cursorLat = qDegreesToRadians(wlat);
    }
    if (m_ruler && m_ruler->isActive())
        m_ruler->setMouse(m_cursorValid, qRadiansToDegrees(m_cursorLat), qRadiansToDegrees(m_cursorLon));
    emitStatus();
}

void GlobeWidget::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_drawCtrl.isActive()) {
        m_drawCtrl.mouseRelease(e);
        return;
    }
    // 套索绘制中：自由手绘松手即闭合；顶点模式单击放一个顶点
    if (m_lasso && m_lasso->isActive() && e->button() == Qt::LeftButton) {
        if (m_lasso->isDrawingFreehand()) {
            m_lasso->endFreehand();
        } else if (m_clickCandidate) {
            double lon = 0.0, lat = 0.0;
            if (geoAt(e->pos(), &lon, &lat))
                m_lasso->addVertex(lat, lon);
        }
        m_clickCandidate = false;
        m_lassoPressValid = false;
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    const bool click = m_clickCandidate;
    const bool wasDragging = m_dragging;
    m_dragging = false;
    m_clickCandidate = false;
    unsetCursor();
    if (click) {
        if (m_ruler && m_ruler->isActive()) {
            // 标尺模式：单击只放航点，不选航迹（RadarView interactionHandler LEFT_CLICK）
            double lon = 0.0, lat = 0.0;
            if (geoAt(e->pos(), &lon, &lat))
                m_ruler->addWaypoint(lat, lon);
        } else {
            const int hit = trackAt(e->pos());
            if (e->modifiers() & Qt::ControlModifier)
                emit trackMultiSelectClicked(hit);
            else
                emit trackClicked(hit);
        }
    }
    updateHover(e->pos(), e->globalPos());
    if (wasDragging)
        emit cameraChanged();       // 拖动结束才存视角，拖动过程中不写设置
}

// 双击：在旗标上删除，否则在该处放旗标（RadarView LEFT_DOUBLE_CLICK）
void GlobeWidget::mouseDoubleClickEvent(QMouseEvent *e)
{
    if (m_drawCtrl.isActive()) {
        m_drawCtrl.mouseDoubleClick(e);
        return;
    }
    // 套索绘制中：双击闭合多边形
    if (m_lasso && m_lasso->isActive() && e->button() == Qt::LeftButton) {
        m_lasso->closePolygon();
        return;
    }
    if (e->button() != Qt::LeftButton || !m_flags)
        return;
    const QString id = flagAt(e->pos());
    if (!id.isEmpty()) {
        m_flags->removeFlag(id);
        return;
    }
    double lon = 0.0, lat = 0.0;
    if (geoAt(e->pos(), &lon, &lat))
        m_flags->addFlag(lat, lon);
}

void GlobeWidget::contextMenuEvent(QContextMenuEvent *e)
{
    QToolTip::hideText();
    e->accept();
    // 绘图模式：右键用于拖拽旋转/闭合图形，不弹菜单
    if (m_drawCtrl.isActive())
        return;
    // 套索绘制中：右键闭合（顶点足够时），否则取消
    if (m_lasso && m_lasso->isActive()) {
        if (m_lasso->vertices().size() >= 3)
            m_lasso->closePolygon();
        else
            m_lasso->clearAll();
        return;
    }
    if (m_ruler && m_ruler->isActive())
        return;   // 标尺模式下右键不弹菜单
    const QString flag = flagAt(e->pos());
    if (!flag.isEmpty()) {
        emit flagContextMenuRequested(flag, e->globalPos());
        return;
    }
    emit trackContextMenuRequested(trackAt(e->pos()), e->globalPos());
}

void GlobeWidget::leaveEvent(QEvent *e)
{
    QOpenGLWidget::leaveEvent(e);
    m_cursorValid = false;     // 与 RadarView 一致：鼠标离开地图后经纬度归零
    syncOverlayContext();
    emitStatus();
    if (m_ruler && m_ruler->isActive())
        m_ruler->setMouse(false, 0.0, 0.0);
    if (m_tipTrack >= 0) {
        m_tipTrack = m_tipPoint = -1;
        syncOverlayContext();
        update();
    }
    if (m_trackLayer.hoveredTrack() >= 0) {
        m_trackLayer.setHoveredTrack(-1);
        QToolTip::hideText();
        unsetCursor();
        update();
    }
}

// 悬停：红色高亮 + 提示框（航班号、来源、点数、时间范围）
void GlobeWidget::updateHover(const QPoint &pos, const QPoint &globalPos)
{
    const int idx = trackAt(pos);
    // 点迹悬停：拾取到的航迹若显示了点迹，找离鼠标最近的点，在点旁边显示该点数据，不再弹航迹提示框
    int tipTrack = -1, tipPoint = -1;
    if (idx >= 0 && m_trackLayer.hasPointDots(idx)) {
        m_camera.updateMatrices(size());
        if (m_trackLayer.pickPointDot(m_camera.mvp(), m_camera.eye(), QSizeF(width(), height()), pos, idx, &tipPoint))
            tipTrack = idx;
    }
    if (tipTrack != m_tipTrack || tipPoint != m_tipPoint) {
        m_tipTrack = tipTrack;
        m_tipPoint = tipPoint;
        syncOverlayContext();
        update();
    }
    if (idx == m_trackLayer.hoveredTrack()) {
        if (idx >= 0 && tipTrack < 0)
            QToolTip::showText(globalPos, trackTooltip(idx), this);   // 跟随鼠标
        else if (tipTrack >= 0)
            QToolTip::hideText();
        return;
    }
    m_trackLayer.setHoveredTrack(idx);
    emit trackHovered(idx);             // 供点迹窗口做"同一条航迹同时高亮"的联动
    if (idx >= 0) {
        setCursor(Qt::PointingHandCursor);
        if (tipTrack < 0)
            QToolTip::showText(globalPos, trackTooltip(idx), this);
        else
            QToolTip::hideText();
    } else {
        unsetCursor();
        QToolTip::hideText();
    }
    update();
}

QString GlobeWidget::trackTooltip(int index) const
{
    if (!m_trackStore || index < 0 || index >= m_trackStore->size())
        return QString();
    const Track &t = m_trackStore->tracks().at(index);
    QString title = t.flightNo.isEmpty() ? t.id : t.flightNo;
    QStringList lines;
    lines << QStringLiteral("<b>%1</b>").arg(title.toHtmlEscaped());
    if (!t.flightNo.isEmpty() && t.flightNo != t.id)
        lines << tr("ICAO：%1").arg(t.id.toHtmlEscaped());
    lines << tr("来源：%1（%2）").arg(trackSourceName(t.source), t.fileName.toHtmlEscaped());
    if (!t.aircraftType.isEmpty())
        lines << tr("机型：%1").arg(t.aircraftType.toHtmlEscaped());
    if (!t.origin.isEmpty() || !t.destination.isEmpty())
        lines << tr("航线：%1 → %2").arg(t.origin.toHtmlEscaped(), t.destination.toHtmlEscaped());
    const RadarPointMode mode = m_trackLayer.radarPointMode();
    const QVector<TrackPoint> &pts = t.pointsForMode(mode);
    if (t.source == TrackSource::Radar && (t.hasRawPoints() || t.hasPredPoints())) {
        lines << tr("点数：%1（当前：%2 | 平滑 %3 / 关联 %4 / 预测 %5）")
                     .arg(pts.size()).arg(radarPointModeName(mode))
                     .arg(t.points.size()).arg(t.rawPoints.size()).arg(t.predPoints.size());
    } else {
        lines << tr("点数：%1").arg(pts.size());
    }
    const qint64 tMin = pts.isEmpty() ? t.minTime() : pts.first().t;
    const qint64 tMax = pts.isEmpty() ? t.maxTime() : pts.last().t;
    lines << tr("时间：%1 ~ %2").arg(formatBeijingTime(tMin), formatBeijingTime(tMax));
    return QStringLiteral("<qt><div style='font-size:14px'>") + lines.join(QStringLiteral("<br>"))
         + QStringLiteral("</div></qt>");
}

void GlobeWidget::wheelEvent(QWheelEvent *e)
{
    zoomBy(e->angleDelta().y() / 120.0);
    e->accept();
}

// 只排队：真正的信号在事件循环下一轮发出。paintGL 里会调到这里，绘制过程中直接改状态栏文字
// 会触发其他控件的重绘和重新布局，可能让 QOpenGLWidget 的合成拖慢甚至错过更新
void GlobeWidget::emitStatus()
{
    if (!m_statusTimer.isActive())
        m_statusTimer.start();
}

int GlobeWidget::fps() const
{
    return int(std::lround(m_fpsSmoothed));
}

// 与 RadarView viewerCore.ts setupFpsTracking 相同：每 500 ms（且至少 5 帧）采样一次，
// 与上次结果对半平滑；超过 1.5 s 没有新帧（画面静止）则归零，状态栏显示 "--"
void GlobeWidget::trackFps()
{
    const qint64 now = m_fpsClock.elapsed();
    if (m_fpsLastSample < 0) {
        m_fpsLastSample = now;
        m_fpsFrames = 1;
        return;
    }
    ++m_fpsFrames;
    const qint64 elapsed = now - m_fpsLastSample;
    if (elapsed >= 500 && m_fpsFrames >= 5) {
        const double instant = m_fpsFrames / (elapsed / 1000.0);
        m_fpsSmoothed = m_fpsSmoothed == 0.0 ? instant : m_fpsSmoothed * 0.5 + instant * 0.5;
        m_fpsFrames = 0;
        m_fpsLastSample = now;
    }
    if (elapsed > 1500) {
        m_fpsSmoothed = 0.0;
        m_fpsFrames = 0;
        m_fpsLastSample = now;
    }
}
