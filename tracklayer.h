#ifndef TRACKLAYER_H
#define TRACKLAYER_H

#include <QOpenGLFunctions>
#include <QMatrix4x4>
#include <QColor>
#include <QHash>
#include <QPointF>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QVector>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>
#include <functional>

class QOpenGLShaderProgram;
class TrackStore;

// 航迹图层：所有航迹平铺在固定高度，宽线在顶点着色器里按屏幕像素展开
// （ANGLE 不支持 glLineWidth > 1，所以不用 GL_LINES）。
// 静态线段按「导入文件」分组上传，每组一种颜色，组内按 65535 顶点切批（ES2 只有 16 位索引）。
// 回放时每个顶点带线段结束时刻，着色器按时间窗口裁剪；航迹头部插值段和端点由 CPU 每帧生成。
// 悬停的航迹再用红色宽线叠画一遍；单独显示时只画该航迹自己的一份批次。
class TrackLayer
{
public:
    static const double kAltitudeM;     // 所有航迹的显示高度

    TrackLayer();
    ~TrackLayer();

    // 需在 GL 上下文当前时调用
    bool initialize(QOpenGLFunctions *gl);
    void cleanup();
    void draw(const QMatrix4x4 &mvp, const QSize &viewportPx, float dpr);

    // 数据与显示状态（任何时候都可调用，下次 draw 时生效）
    void setStore(const TrackStore *store);
    void setGroupVisible(const QString &groupKey, bool visible);
    bool isGroupVisible(const QString &groupKey) const;
    void setReplay(bool active, qint64 timeMs);
    void setTrailSeconds(double seconds);   // <= 0 表示显示全部历史

    // 交互：参数为 TrackStore::tracks() 的下标，-1 表示无
    void setHoveredTrack(int index);
    int hoveredTrack() const { return m_hoverTrack; }
    void setIsolatedTrack(int index);       // 单独显示该航迹（不受分组显隐影响）
    int isolatedTrack() const { return m_isoTrack; }

    // 屏幕拾取：返回离 pos 不超过 tolPx 的最近可见航迹，没有返回 -1。
    // viewport 与 pos 同为逻辑像素；只在 CPU 上计算，不需要 GL 上下文
    int pick(const QMatrix4x4 &mvp, const QVector3D &eye, const QSizeF &viewport,
             const QPointF &pos, float tolPx) const;

    QString shaderLog() const { return m_log; }
    bool isReady() const { return m_lineProg && m_dotProg; }
    int segmentCount() const { return m_segments; }   // 已上传的静态线段数
    int visibleTrackCount() const { return m_visibleTracks; }   // 上一帧显示端点的航迹数
    bool hoverDrawn() const { return m_hoverDrawn; }  // 上一帧是否画了悬停高亮

private:
    struct Group {
        QString key;
        QColor color;
        float alpha = 0.88f;
        float radiusPx = 6.0f;
        bool visible = true;
    };
    struct Batch { GLuint vbo = 0; int indexCount = 0; int group = 0; };
    // C++11 里带默认成员初始值的结构体不是聚合体，不能花括号初始化，所以显式写构造函数
    struct GroupRange {   // 动态缓冲中某段四边形的范围
        int first, count;
        GroupRange(int f = 0, int c = 0) : first(f), count(c) {}
    };
    struct Piece {        // 某条航迹在静态批次中的一段
        int batch, first, count;
        Piece(int b = 0, int f = 0, int c = 0) : batch(b), first(f), count(c) {}
    };
    // 拾取缓存：与静态线段相同的折线顶点（含细分点）、每个顶点所属线段的结束时刻、包围盒
    struct PickTrack {
        QVector<QVector3D> pos;
        QVector<qint64> t;
        QVector3D bmin, bmax;
    };

    void rebuildStatic();
    void rebuildDynamic();
    void releaseBatches();
    int groupIndex(const QString &key) const;
    bool trackShown(int index) const;
    void ensurePickCache() const;

    QOpenGLFunctions *m_gl = nullptr;
    QOpenGLShaderProgram *m_lineProg = nullptr;
    QOpenGLShaderProgram *m_dotProg = nullptr;
    QString m_log;
    int m_uLineMvp = -1, m_uLineViewport = -1, m_uLineHalfWidth = -1, m_uLineWindow = -1, m_uLineColor = -1;
    int m_uDotMvp = -1, m_uDotViewport = -1, m_uDotRadius = -1, m_uDotColor = -1;

    const TrackStore *m_store = nullptr;
    quint64 m_builtVersion = ~quint64(0);
    qint64 m_baseTime = 0;               // 顶点时间 = (t - m_baseTime) / 1000 秒
    QVector<Group> m_groups;
    QHash<QString, bool> m_hidden;       // 被用户隐藏的组（重建后保持）
    QVector<int> m_trackGroup;           // 每条航迹所属组
    QVector<Batch> m_batches;
    int m_segments = 0;

    GLuint m_quadIbo = 0;                // 所有四边形共用的索引缓冲
    // 动态部分：头部插值段（线）+ 端点（点）
    GLuint m_headVbo = 0, m_dotVbo = 0;
    QVector<GroupRange> m_headRanges, m_dotRanges;
    bool m_dynamicDirty = true;
    int m_visibleTracks = 0;

    bool m_replay = false;
    qint64 m_replayTime = 0;
    double m_trailSeconds = 0.0;

    // 悬停 / 单独显示：直接复用静态批次里该航迹的那几段，不另外上传
    int m_hoverTrack = -1, m_isoTrack = -1;
    QVector<QVector<Piece>> m_trackPieces;   // 每条航迹在静态批次中的位置
    GroupRange m_hoverHead;                  // 悬停航迹在动态缓冲中的头部段
    int m_hoverDot = -1;                     // 悬停航迹在动态缓冲中的端点序号
    bool m_hoverDrawn = false;

    mutable QVector<PickTrack> m_pick;
    mutable quint64 m_pickVersion = ~quint64(0);
};

#endif // TRACKLAYER_H
