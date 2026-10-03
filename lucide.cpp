#include "lucide.h"

#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QStringList>
#include <QtMath>
#include <cmath>

namespace {

struct IconData { LucideIcon icon; const char *data; };

const IconData kIcons[] = {
#include "lucidedata.inc"
};

// ---- SVG path "d" 解析：支持 M L H V C S Q T A Z（含相对坐标）----
class PathParser
{
public:
    explicit PathParser(const QString &d) : s(d) {}

    QPainterPath parse()
    {
        QPainterPath path;
        QPointF cur, start, lastCtrl;
        QChar cmd, prevCmd;
        while (true) {
            skipSep();
            if (i >= s.size())
                break;
            if (s[i].isLetter())
                cmd = s[i++];
            else if (cmd.isNull())
                break;
            const bool rel = cmd.isLower();
            const QChar c = cmd.toUpper();
            const QPointF base = rel ? cur : QPointF();
            if (c == QLatin1Char('Z')) {
                path.closeSubpath();
                cur = start;
                prevCmd = c;
                cmd = QChar();
                continue;
            }
            if (c == QLatin1Char('M')) {
                cur = base + point();
                start = cur;
                path.moveTo(cur);
                cmd = rel ? QLatin1Char('l') : QLatin1Char('L');   // 后续坐标对按 L 处理
            } else if (c == QLatin1Char('L')) {
                cur = base + point();
                path.lineTo(cur);
            } else if (c == QLatin1Char('H')) {
                cur.setX((rel ? cur.x() : 0.0) + num());
                path.lineTo(cur);
            } else if (c == QLatin1Char('V')) {
                cur.setY((rel ? cur.y() : 0.0) + num());
                path.lineTo(cur);
            } else if (c == QLatin1Char('C')) {
                const QPointF c1 = base + point(), c2 = base + point(), e = base + point();
                path.cubicTo(c1, c2, e);
                lastCtrl = c2;
                cur = e;
            } else if (c == QLatin1Char('S')) {
                const QPointF c1 = (prevCmd == QLatin1Char('C') || prevCmd == QLatin1Char('S'))
                                       ? 2 * cur - lastCtrl : cur;
                const QPointF c2 = base + point(), e = base + point();
                path.cubicTo(c1, c2, e);
                lastCtrl = c2;
                cur = e;
            } else if (c == QLatin1Char('Q')) {
                const QPointF q = base + point(), e = base + point();
                path.quadTo(q, e);
                lastCtrl = q;
                cur = e;
            } else if (c == QLatin1Char('T')) {
                const QPointF q = (prevCmd == QLatin1Char('Q') || prevCmd == QLatin1Char('T'))
                                      ? 2 * cur - lastCtrl : cur;
                const QPointF e = base + point();
                path.quadTo(q, e);
                lastCtrl = q;
                cur = e;
            } else if (c == QLatin1Char('A')) {
                const double rx = num(), ry = num(), rot = num();
                const bool large = flag(), sweep = flag();
                const QPointF e = base + point();
                arcTo(path, cur, rx, ry, rot, large, sweep, e);
                cur = e;
            } else {
                break;   // 不认识的命令
            }
            prevCmd = c;
        }
        return path;
    }

private:
    void skipSep()
    {
        while (i < s.size() && (s[i].isSpace() || s[i] == QLatin1Char(',')))
            ++i;
    }
    double num()
    {
        skipSep();
        const int b = i;
        if (i < s.size() && (s[i] == QLatin1Char('-') || s[i] == QLatin1Char('+')))
            ++i;
        bool dot = false;
        while (i < s.size()) {
            const QChar ch = s[i];
            if (ch.isDigit()) {
                ++i;
            } else if (ch == QLatin1Char('.') && !dot) {
                dot = true;
                ++i;
            } else if ((ch == QLatin1Char('e') || ch == QLatin1Char('E')) && i > b) {
                ++i;
                if (i < s.size() && (s[i] == QLatin1Char('-') || s[i] == QLatin1Char('+')))
                    ++i;
            } else {
                break;
            }
        }
        return s.midRef(b, i - b).toDouble();
    }
    bool flag()
    {
        skipSep();
        if (i < s.size() && (s[i] == QLatin1Char('0') || s[i] == QLatin1Char('1')))
            return s[i++] == QLatin1Char('1');
        return num() != 0.0;
    }
    QPointF point()
    {
        const double x = num();
        const double y = num();
        return QPointF(x, y);
    }

    // SVG 椭圆弧（端点参数）-> 三次贝塞尔
    static void arcTo(QPainterPath &path, const QPointF &p0, double rx, double ry, double rotDeg,
                      bool large, bool sweep, const QPointF &p1)
    {
        if (p0 == p1)
            return;
        rx = std::fabs(rx);
        ry = std::fabs(ry);
        if (rx < 1e-9 || ry < 1e-9) {
            path.lineTo(p1);
            return;
        }
        const double phi = qDegreesToRadians(rotDeg);
        const double cp = std::cos(phi), sp = std::sin(phi);
        const double dx = (p0.x() - p1.x()) / 2.0, dy = (p0.y() - p1.y()) / 2.0;
        const double x1 = cp * dx + sp * dy, y1 = -sp * dx + cp * dy;
        const double lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
        if (lambda > 1.0) {
            rx *= std::sqrt(lambda);
            ry *= std::sqrt(lambda);
        }
        const double num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
        const double den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
        double coef = den > 0.0 ? std::sqrt(qMax(0.0, num / den)) : 0.0;
        if (large == sweep)
            coef = -coef;
        const double cxp = coef * rx * y1 / ry, cyp = -coef * ry * x1 / rx;
        const double cx = cp * cxp - sp * cyp + (p0.x() + p1.x()) / 2.0;
        const double cy = sp * cxp + cp * cyp + (p0.y() + p1.y()) / 2.0;
        auto angle = [](double ux, double uy, double vx, double vy) {
            return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
        };
        const double t1 = angle(1, 0, (x1 - cxp) / rx, (y1 - cyp) / ry);
        double dt = angle((x1 - cxp) / rx, (y1 - cyp) / ry, (-x1 - cxp) / rx, (-y1 - cyp) / ry);
        if (!sweep && dt > 0)
            dt -= 2 * M_PI;
        else if (sweep && dt < 0)
            dt += 2 * M_PI;
        const int segs = qMax(1, int(std::ceil(std::fabs(dt) / (M_PI / 2.0))));
        const double step = dt / segs;
        const double k = 4.0 / 3.0 * std::tan(step / 4.0);
        auto pt = [&](double t, double ox, double oy) {
            const double x = rx * (std::cos(t) + ox), y = ry * (std::sin(t) + oy);
            return QPointF(cp * x - sp * y + cx, sp * x + cp * y + cy);
        };
        double t = t1;
        for (int n = 0; n < segs; ++n) {
            const double ta = t, tb = t + step;
            const QPointF c1 = pt(ta, -k * std::sin(ta), k * std::cos(ta));
            const QPointF c2 = pt(tb, k * std::sin(tb), -k * std::cos(tb));
            const QPointF e = n == segs - 1 ? p1 : pt(tb, 0, 0);
            path.cubicTo(c1, c2, e);
            t = tb;
        }
    }

    QString s;
    int i = 0;
};

QPainterPath buildIcon(LucideIcon icon)
{
    static QHash<int, QPainterPath> cache;
    auto it = cache.find(int(icon));
    if (it != cache.end())
        return *it;
    QPainterPath path;
    for (const IconData &d : kIcons) {
        if (d.icon != icon)
            continue;
        const QStringList parts = QString::fromLatin1(d.data).split(QLatin1Char('|'));
        for (const QString &part : parts) {
            const QChar kind = part.at(0);
            const QString body = part.mid(2);
            const QStringList v = body.split(QLatin1Char(' '), QString::SkipEmptyParts);
            if (kind == QLatin1Char('P')) {
                path.addPath(PathParser(body).parse());
            } else if (kind == QLatin1Char('C') && v.size() >= 3) {
                const double r = v[2].toDouble();
                path.addEllipse(QPointF(v[0].toDouble(), v[1].toDouble()), r, r);
            } else if (kind == QLatin1Char('R') && v.size() >= 5) {
                const double rx = v[4].toDouble();
                path.addRoundedRect(QRectF(v[0].toDouble(), v[1].toDouble(), v[2].toDouble(), v[3].toDouble()), rx, rx);
            } else if (kind == QLatin1Char('L') && v.size() >= 4) {
                path.moveTo(v[0].toDouble(), v[1].toDouble());
                path.lineTo(v[2].toDouble(), v[3].toDouble());
            } else if (kind == QLatin1Char('Y')) {
                const QStringList pts = body.split(QRegExp(QStringLiteral("[ ,]+")), QString::SkipEmptyParts);
                for (int k = 0; k + 1 < pts.size(); k += 2) {
                    const QPointF p(pts[k].toDouble(), pts[k + 1].toDouble());
                    if (k == 0)
                        path.moveTo(p);
                    else
                        path.lineTo(p);
                }
            }
        }
        break;
    }
    cache.insert(int(icon), path);
    return path;
}

} // namespace

void drawLucide(QPainter &p, LucideIcon icon, const QRectF &box, const QColor &color, const QColor &fill)
{
    const QPainterPath path = buildIcon(icon);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(box.topLeft());
    p.scale(box.width() / 24.0, box.height() / 24.0);
    p.setBrush(fill.alpha() > 0 ? QBrush(fill) : QBrush(Qt::NoBrush));
    p.setPen(QPen(color, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(path);
    p.restore();
}

QPixmap lucidePixmap(LucideIcon icon, int size, const QColor &color, qreal dpr, const QColor &fill)
{
    QPixmap pm(int(std::ceil(size * dpr)), int(std::ceil(size * dpr)));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    drawLucide(p, icon, QRectF(0, 0, size, size), color, fill);
    return pm;
}

QIcon lucideQIcon(LucideIcon icon, int size, const QColor &color, const QColor &hover)
{
    QIcon ic;
    for (qreal dpr : { 1.0, 2.0 }) {
        ic.addPixmap(lucidePixmap(icon, size, color, dpr), QIcon::Normal);
        QColor dis = color;
        dis.setAlphaF(color.alphaF() * 0.5);
        ic.addPixmap(lucidePixmap(icon, size, dis, dpr), QIcon::Disabled);
        if (hover.isValid())
            ic.addPixmap(lucidePixmap(icon, size, hover, dpr), QIcon::Active);
    }
    return ic;
}
