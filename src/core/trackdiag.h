#ifndef TRACKDIAG_H
#define TRACKDIAG_H

#include <QHash>
#include <QMetaType>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>
#include <limits>

#include "point3dtypes.h"
#include "radarsite.h"
#include "track.h"

// 航迹诊断底层数据模型、MAT 诊断字段解析与双问题分析引擎：
//   1. 虚假航迹评估与门限试算沙盒（evaluateFalseTracks）
//   2. 消失/断航溯源（findBrokenTracks / runGateAutopsy / findFragmentPairs /
//      findOrphanDetClusters / traceTrackFunnel）
// 全部为纯 C++11 / QtCore 函数，线程安全，可与 SQLite schema v7 零侵入共存。
namespace trackdiag {

// ---------------------------------------------------------------------------
//  MAT 诊断元数据
// ---------------------------------------------------------------------------

// lifeList 逐帧生命周期记录（col0..5: frameID, assc, lost, succLost, prevQ, curQ）
struct TrackLifeRow {
    int frameId;
    int asscFlag;
    int lostCnt;
    int succLostCnt;
    double prevQuality;
    double curQuality;

    TrackLifeRow()
        : frameId(0), asscFlag(0), lostCnt(0), succLostCnt(0),
          prevQuality(0.0), curQuality(0.0)
    {}
};

// filterPara 关联波门参数（固定门限 + 浮动扩展系数）
struct TrackFilterGate {
    double fixedR;
    double fixedA;
    double fixedV;
    double floatR;
    double floatA;
    double floatV;
    bool valid;

    TrackFilterGate()
        : fixedR(0.0), fixedA(0.0), fixedV(0.0),
          floatR(0.0), floatA(0.0), floatV(0.0),
          valid(false)
    {}
};

// 单条雷达航迹的 MAT 扩展诊断字段
struct TrackDiagMeta {
    QString trackKey;
    int batchNo;
    int trackType;
    double quality;
    int totalPointCnt;
    int asscPointCnt;
    int totalLostCnt;
    int succLossCnt;
    double travelLenKm;
    int startFrame;
    int confirmFrame;
    int startAsscCnt;
    double startQuality;
    TrackFilterGate gate;
    QVector<TrackLifeRow> life;
    QVector<int> asscFlags;       // 与 Track::points (smoothPointList) 等长，1=实测关联，0=外推补点
    QVector<int> asscPointIds;    // 与 Track::rawPoints (asscPointList) 等长，关联点迹 pointID
    QVector<int> asscFrameIds;    // 与 Track::rawPoints (asscPointList) 等长，关联点迹 frameID
    QVector<int> smoothFrameIds;  // 与 Track::points 等长，平滑点帧号
    QVector<int> predFrameIds;    // 与 Track::predPoints 等长，预测点帧号
    bool valid;

    TrackDiagMeta()
        : batchNo(0), trackType(0), quality(0.0),
          totalPointCnt(0), asscPointCnt(0), totalLostCnt(0), succLossCnt(0),
          travelLenKm(0.0), startFrame(0), confirmFrame(0),
          startAsscCnt(0), startQuality(0.0), valid(false)
    {}
};

// tempTrackList 临时池点（L2 漏斗层）
struct TempTrackPoint {
    int frameId;
    int pointId;
    qint64 timeMs;
    double lat;
    double lon;
    float prange;
    float paz;
    float pvr;
    float snr;

    TempTrackPoint()
        : frameId(0), pointId(0), timeMs(0), lat(0.0), lon(0.0),
          prange(std::numeric_limits<float>::quiet_NaN()),
          paz(std::numeric_limits<float>::quiet_NaN()),
          pvr(std::numeric_limits<float>::quiet_NaN()),
          snr(std::numeric_limits<float>::quiet_NaN())
    {}
};

// 一个或多个雷达 MAT 文件解析出的诊断数据包
struct TrackDiagBundle {
    QString sourceFileName;
    QHash<QString, TrackDiagMeta> metas;
    QVector<TempTrackPoint> tempPoints;

    bool isEmpty() const { return metas.isEmpty() && tempPoints.isEmpty(); }
    void clear();
    // 合并另一份诊断包（多文件导入场景）
    void merge(const TrackDiagBundle &other);
    // 按 Track::key() 优先查找，回退按 BatchNo + 文件名匹配
    const TrackDiagMeta *findMeta(const Track &track) const;
};

// 从雷达 MAT 解析诊断扩展字段（trackList 生命周期/质量/波门/关联标志 + tempTrackList）
bool parseTrackDiagFromMat(const QString &matPath, const QString &displayName,
                           TrackSource src, TrackDiagBundle *out,
                           QString *error = nullptr);

// 航迹 Type 中文说明（1=稳定/正常, 2=维持/外推, 6=低质/待定, 7=消亡/终止）
QString trackTypeName(int trackType);

// ---------------------------------------------------------------------------
//  1. 虚假航迹评估与门限试算沙盒
// ---------------------------------------------------------------------------

struct FalseTrackConfig {
    double minQuality;
    double minAsscRatio;
    int minAsscPoints;
    int maxSuccLoss;
    double minMeanSnrDb;
    double minTravelKm;
    double maxMeanInnovKm;
    bool filterOnlyRadar;

    FalseTrackConfig()
        : minQuality(0.3), minAsscRatio(0.35), minAsscPoints(4),
          maxSuccLoss(5), minMeanSnrDb(10.0), minTravelKm(5.0),
          maxMeanInnovKm(30.0), filterOnlyRadar(true)
    {}
};

struct FalseTrackItem {
    int trackIndex;
    QString trackKey;
    QString displayId;
    double suspicionScore;    // 0..100，越高越可疑
    double quality;           // 0..1
    double asscRatio;         // 0..1
    int asscCount;
    int totalCount;
    int succLoss;
    double meanSnr;           // dB，缺失为 NaN
    double travelLenKm;       // km
    double meanInnovKm;       // 平均预测新息水平误差(km)，缺失为 NaN
    int trackType;
    bool filteredOut;         // 当前沙盒门限下是否拟滤除
    QStringList reasons;      // 触发的异常/滤除标签
    int multipathPeerIndex;   // 多径镜像配对航迹索引，无配对为 -1

    FalseTrackItem()
        : trackIndex(-1), suspicionScore(0.0), quality(0.0), asscRatio(0.0),
          asscCount(0), totalCount(0), succLoss(0),
          meanSnr(std::numeric_limits<double>::quiet_NaN()),
          travelLenKm(0.0),
          meanInnovKm(std::numeric_limits<double>::quiet_NaN()),
          trackType(0), filteredOut(false), multipathPeerIndex(-1)
    {}
};

// 评估所有符合条件的航迹，返回按 suspicionScore 降序排列的结果。
// 若 bundle 无元数据，也能基于 points / rawPoints / predPoints 优雅退化评估。
QVector<FalseTrackItem> evaluateFalseTracks(const TrackStore &store,
                                            const TrackDiagBundle &bundle,
                                            const FalseTrackConfig &cfg);

// ---------------------------------------------------------------------------
//  2. 消失/断航溯源引擎
// ---------------------------------------------------------------------------

struct BrokenTrackItem {
    int trackIndex;
    QString trackKey;
    QString displayId;
    qint64 endTimeMs;
    double endLat;
    double endLon;
    bool insideSector;
    int succLoss;
    double endQuality;
    QString likelyCause;

    BrokenTrackItem()
        : trackIndex(-1), endTimeMs(0), endLat(0.0), endLon(0.0),
          insideSector(true), succLoss(0), endQuality(0.0)
    {}
};

QVector<BrokenTrackItem> findBrokenTracks(const TrackStore &store,
                                          const TrackDiagBundle &bundle,
                                          const RadarSite *site = nullptr);

// 单帧波门尸检中的候选检测点
struct GateCandidate {
    int frameIndex;
    int frameId;
    qint64 timeMs;
    int pointId;
    float range;
    float az;
    float vr;
    float snr;
    double dR;
    double dAz;
    double dVr;
    bool insideGate;
    int ownerBatchNo;    // 0=未被确认航迹关联；>0=被该批号关联
    QString verdict;     // [门内-本批已关联] / [门内-被批号X抢占] / [门外超限] / [门内未关联]

    GateCandidate()
        : frameIndex(-1), frameId(0), timeMs(0), pointId(0),
          range(0.0f), az(0.0f), vr(0.0f), snr(0.0f),
          dR(0.0), dAz(0.0), dVr(0.0),
          insideGate(false), ownerBatchNo(0)
    {}
};

// 单帧波门尸检快照
struct GateAutopsyFrame {
    int frameIndex;
    int frameId;
    qint64 timeMs;
    double predLat;
    double predLon;
    float predR;
    float predAz;
    float predVr;
    float gateR;
    float gateAz;
    float gateVr;
    bool hasDetectionIn2xGate;
    QVector<GateCandidate> candidates;
    QString summary;

    GateAutopsyFrame()
        : frameIndex(-1), frameId(0), timeMs(0),
          predLat(0.0), predLon(0.0),
          predR(std::numeric_limits<float>::quiet_NaN()),
          predAz(std::numeric_limits<float>::quiet_NaN()),
          predVr(std::numeric_limits<float>::quiet_NaN()),
          gateR(0.0f), gateAz(0.0f), gateVr(0.0f),
          hasDetectionIn2xGate(false)
    {}
};

struct GateAutopsyResult {
    int trackIndex;
    QString trackKey;
    QVector<GateAutopsyFrame> frames;
    QString primaryCause;

    GateAutopsyResult()
        : trackIndex(-1)
    {}
};

GateAutopsyResult runGateAutopsy(int trackIndex,
                                 const TrackStore &store,
                                 const TrackDiagBundle &bundle,
                                 const Point3dDataset *detData);

// 疑似断批重起航迹对（A 结束 -> B 接续）
struct TrackFragmentPair {
    int firstIndex;
    int secondIndex;
    QString firstId;
    QString secondId;
    qint64 gapMs;
    double extrapDistKm;
    double dVr;
    double dAz;
    double score;    // 0..100

    TrackFragmentPair()
        : firstIndex(-1), secondIndex(-1), gapMs(0),
          extrapDistKm(0.0),
          dVr(std::numeric_limits<double>::quiet_NaN()),
          dAz(std::numeric_limits<double>::quiet_NaN()),
          score(0.0)
    {}
};

QVector<TrackFragmentPair> findFragmentPairs(const TrackStore &store,
                                             qint64 maxGapMs = 90000,
                                             double maxDistKm = 60.0);

// 未关联强连续检测点迹链（漏批挖掘）
struct OrphanDetCluster {
    int clusterId;
    int pointCount;
    int frameStart;
    int frameEnd;
    qint64 t0;
    qint64 t1;
    float meanRange;
    float meanAz;
    float meanVr;
    float meanSnr;
    float meanContinuity;
    QVector<QPair<int, int> > framePointIndices;  // (frameIndex, pointIndex) in detData.frames

    OrphanDetCluster()
        : clusterId(0), pointCount(0), frameStart(0), frameEnd(0),
          t0(0), t1(0), meanRange(0.0f), meanAz(0.0f), meanVr(0.0f),
          meanSnr(0.0f), meanContinuity(0.0f)
    {}
};

QVector<OrphanDetCluster> findOrphanDetClusters(const Point3dDataset &detData,
                                                const TrackStore &store,
                                                const TrackDiagBundle &bundle,
                                                double minContinuity = 0.65,
                                                int minPoints = 4);

// 四级漏斗单帧统计（L1 原始检测 -> L2 临时池 -> L3 关联点 -> L4 平滑输出）
struct FunnelFrameStat {
    int frameIndex;
    int frameId;
    qint64 timeMs;
    int l1DetCount;
    float bestDetSnr;
    double bestDetDistKm;
    int l2TempCount;
    int l3AsscCount;
    int l4SmoothCount;

    FunnelFrameStat()
        : frameIndex(-1), frameId(0), timeMs(0),
          l1DetCount(0),
          bestDetSnr(std::numeric_limits<float>::quiet_NaN()),
          bestDetDistKm(std::numeric_limits<double>::quiet_NaN()),
          l2TempCount(0), l3AsscCount(0), l4SmoothCount(0)
    {}
};

QVector<FunnelFrameStat> traceTrackFunnel(const Track &refTrack,
                                          const TrackStore &store,
                                          const TrackDiagBundle &bundle,
                                          const Point3dDataset *detData,
                                          double searchRadiusKm = 40.0);

} // namespace trackdiag

Q_DECLARE_METATYPE(trackdiag::TrackDiagBundle)
Q_DECLARE_METATYPE(trackdiag::GateAutopsyResult)
Q_DECLARE_METATYPE(trackdiag::OrphanDetCluster)

#endif // TRACKDIAG_H
