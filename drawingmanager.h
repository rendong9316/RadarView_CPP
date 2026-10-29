#ifndef DRAWINGMANAGER_H
#define DRAWINGMANAGER_H

#include <QColor>
#include <QJsonObject>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QVector>

// 球面贴地绘图的数据层：距离圆、直线、自由曲线、多边形、矩形、填充、文本框统一成一个
// DrawingFeature，交互状态机在 GlobeWidget 里，主窗口只负责工具栏和弹窗。
//
// 几何一律保存成 WGS84 经纬度顶点（x = 经度、y = 纬度，单位度），并且**已经按大圆加密**：
//   - 直线/多边形/矩形/圆的边用球面 slerp 插值，投影到球面才是贴地曲线，不是两点间的弦；
//   - 圆用 Vincenty 正算逐方位角求点，是椭球上的等距圆，不是经纬度平面上的椭圆；
//   - 自由曲线本来就点很密，按原样保存；
//   - 闭合图形首尾重复，渲染时一律当一条折线画。
// 数据永远是 WGS84；高德底图的 GCJ-02 只在 GlobeWidget 投影那一刻转换。
// 不依赖 GeographicLib：本工程禁止第三方库（见 CODEBUDDY.md）。

class QUndoStack;

// Dot：站点标记用的固定屏幕大小圆点（半径与航迹端点圆球一致，不随缩放变化）
enum class DrawKind { Circle, Line, Polyline, Polygon, Rect, Fill, Text, Dot };

// 绘图工具（GlobeWidget 的状态机；None = 漫游，左键拖拽旋转）
enum class DrawTool { None, Line, Polyline, Polygon, Rect, Text, Fill, Eraser, BoxSelect, Select, Break };

struct DrawingFeature {
    int id = 0;
    DrawKind kind = DrawKind::Polyline;
    QColor color = QColor(0xf5, 0x9e, 0x0b);   // 线色 / 文字色；Fill 时同时是填充色
    QColor fill;                               // 有效时填充（alpha 即透明度），仅闭合图形
    double widthPx = 3.0;                      // 需求：线宽固定 3px
    QVector<QPointF> pts;                      // 已加密的 WGS84 顶点（度）；Text 只有 1 个锚点
    // 圆专用（其余图形为 0），序列化后据此还原参数
    double centerLon = 0.0, centerLat = 0.0, radiusKm = 0.0;
    QString text;                              // Text 专用
    double textSizePx = 16.0;                  // Text 专用（逻辑像素，渲染时随界面字号缩放）
    QString label;
    // 打断/裁剪点（WGS84）：渲染成一个小圆点。挂在要素自己身上，
    // 所以要素被删掉时标记跟着消失，不会留下孤零零的点。
    QVector<QPointF> marks;

    bool isText() const { return kind == DrawKind::Text; }
    bool isClosed() const
    {
        return kind == DrawKind::Circle || kind == DrawKind::Polygon
            || kind == DrawKind::Rect || kind == DrawKind::Fill;
    }
    bool isCircle() const { return kind == DrawKind::Circle; }
};

// 圆/直线/曲线/多边形/矩形/填充/文本的名字，用于 GeoJSON properties.kind
QString drawKindName(DrawKind k);
bool drawKindFromName(const QString &name, DrawKind *out);

class DrawingManager : public QObject
{
    Q_OBJECT
public:
    explicit DrawingManager(QObject *parent = nullptr);

    // 撤销/重做：主窗口把 QUndoStack 交给这里以后，所有改动（画图、删除、框选删除、填充、
    // 文字修改、打断、裁剪、清空）都会自动压栈，调用方不需要自己造命令对象。
    void setUndoStack(QUndoStack *stack) { m_undo = stack; }
    QUndoStack *undoStack() const { return m_undo; }

    const QVector<DrawingFeature> &features() const { return m_features; }
    int count() const { return m_features.size(); }
    const DrawingFeature *feature(int id) const;
    int indexOf(int id) const;

    int addFeature(const DrawingFeature &f);      // 返回分配到的 id（0 表示无效）
    int addCircle(double lat, double lon, double radiusKm, const QColor &color,
                  const QString &label = QString());
    // 直线：两端点按大圆加密
    int addLine(double lat1, double lon1, double lat2, double lon2, const QColor &color,
                double widthPx = 3.0);
    // 自由曲线：点列已经是逐像素采样的密点，原样保存
    int addPolyline(const QVector<QPointF> &wgsPts, const QColor &color, double widthPx = 3.0);
    // 多边形：顶点为稀疏点，每条边按大圆加密并闭合
    int addPolygon(const QVector<QPointF> &verts, const QColor &color, double widthPx = 3.0);
    // 矩形：两个对角点（经纬度包围盒），四条边按大圆加密
    int addRect(double lat1, double lon1, double lat2, double lon2, const QColor &color,
                double widthPx = 3.0);
    int addText(double lat, double lon, const QString &text, const QColor &color, int sizePx = 16);
    bool setFill(int id, const QColor &fill);     // alpha 即透明度；无效颜色表示清除填充

    // 属性编辑：按 id 覆盖（颜色 / 线宽 / 填充 / 文本 / 半径），压一条 ModifyFeatureCommand。
    // 圆会按 centerLat/centerLon/radiusKm 重新生成点环，所以改半径后仍是真正的等距圆。
    bool updateFeature(const DrawingFeature &f);
    bool setCircleRadius(int id, double radiusKm);

    bool removeFeature(int id);
    void removeFeatures(const QVector<int> &ids);
    void clear();

    // ---- 打断 / 裁剪 ----
    // 开放图形（直线/曲线）断成两段；闭合图形（圆/多边形/矩形/填充）在断点处剪开，
    // 变成一条首尾重合的开放曲线（CAD 里剪开一个环就是这个结果）。文本不能打断。
    bool canBreak(int id) const;
    // 离 (lat,lon) 最近的顶点下标（打断用），失败返回 -1
    int nearestVertex(int id, double lat, double lon) const;
    // 在 vertexIndex 处一分为二（首尾顶点不能断），返回是否成功
    bool breakFeature(int id, int vertexIndex);
    // 剪切线（两点定义的大圆弧）与哪些开放图形相交
    QVector<int> crossingFeatures(double lat1, double lon1, double lat2, double lon2) const;
    // 裁剪：与剪切线相交的每个图形，在交点处断开并删掉中间被划过的那一段
    // （只交一次时退化成打断）。返回被改动的图形数。
    int trimCrossing(double lat1, double lon1, double lat2, double lon2);

    quint64 version() const { return m_version; }

    // GeoJSON FeatureCollection：闭合图形 -> Polygon，直线/曲线 -> LineString，文本 -> Point；
    // 圆心半径、颜色、线宽、填充、名称放在 properties 里，其他 GIS 软件能直接打开。
    QJsonObject toJson() const;
    bool fromJson(const QJsonObject &root, QString *error = nullptr);
    bool saveToFile(const QString &path, QString *error = nullptr) const;
    bool loadFromFile(const QString &path, QString *error = nullptr);

    // ---- 撤销栈专用的底层操作（不压栈，直接改数据并发 changed()）----
    void insertFeature(int index, const DrawingFeature &f);   // 保留 f.id
    bool takeFeature(int id);
    bool replaceFeature(const DrawingFeature &f);

signals:
    void changed();

private:
    bool pushOrApply(class QUndoCommand *cmd);    // 有撤销栈就压栈，否则执行并删除
    QVector<DrawingFeature> m_features;
    QUndoStack *m_undo = nullptr;
    int m_nextId = 1;
    quint64 m_version = 0;
};

#endif // DRAWINGMANAGER_H
