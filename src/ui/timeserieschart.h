#ifndef TIMESERIESCHART_H
#define TIMESERIESCHART_H

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>
#include <QWidget>

// 纯 QPainter 2D 时序折线/散点控件：零第三方依赖，支持多曲线叠加、框选缩放、滚轮缩放、
// 联动十字准星与回放时间游标。
class TimeSeriesChart : public QWidget
{
    Q_OBJECT
public:
    enum class SeriesStyle { SolidLine = 0, DashLine = 1, Dots = 2 };

    struct Series {
        QString name;
        QColor color;
        SeriesStyle style;
        QVector<QPointF> pts;   // x = epoch ms (double), y = 物理量
        Series(const QString &n = QString(), const QColor &c = QColor(),
               SeriesStyle st = SeriesStyle::SolidLine, const QVector<QPointF> &p = QVector<QPointF>())
            : name(n), color(c), style(st), pts(p) {}
    };

    explicit TimeSeriesChart(QWidget *parent = nullptr);

    void setTitle(const QString &title, const QString &unit = QString());
    void setSeries(const QVector<Series> &series);
    void clear();

    void setXRange(double minMs, double maxMs, bool emitSignal = false);
    void resetZoom(bool emitSignal = true);
    double viewMinX() const { return m_viewMinX; }
    double viewMaxX() const { return m_viewMaxX; }

    void setCursorTime(qint64 ms, bool active);
    void setLinkedHoverTime(qint64 ms, bool valid);

signals:
    void xRangeChanged(double minMs, double maxMs);
    void hoverTimeChanged(qint64 ms, bool valid);
    void timePicked(qint64 ms);

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    QRectF plotRect() const;
    void computeDataBounds();
    void computeVisibleYRange(double *yMin, double *yMax) const;
    double xToTime(double px, const QRectF &pr) const;
    double timeToX(double ms, const QRectF &pr) const;
    double valToY(double v, double yMin, double yMax, const QRectF &pr) const;

    QString m_title;
    QString m_unit;
    QVector<Series> m_series;

    bool m_hasData = false;
    double m_dataMinX = 0.0, m_dataMaxX = 1.0;
    double m_viewMinX = 0.0, m_viewMaxX = 1.0;

    bool m_cursorActive = false;
    qint64 m_cursorMs = 0;
    bool m_hoverValid = false;
    qint64 m_hoverMs = 0;

    bool m_boxZooming = false;
    bool m_panning = false;
    QPoint m_pressPos;
    QPoint m_curPos;
    double m_panStartMinX = 0.0, m_panStartMaxX = 0.0;
};

#endif // TIMESERIESCHART_H
