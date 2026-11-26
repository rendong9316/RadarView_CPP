#ifndef LUCIDE_H
#define LUCIDE_H

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QRectF>

class QPainter;

// RadarView 用的 lucide 图标（viewBox 24×24，线宽 2，圆角端点），用 QPainter 按原始路径数据绘制，不依赖 QtSvg
enum class LucideIcon {
    List, ChartColumn, Layers, Flag, Funnel, Settings, X, Eye, Circle, Trash2, ClipboardList,
    FileText, Dot, Download, RefreshCw, Sparkles, Loader, Hash, Clock, Package, RotateCcw,
    HelpCircle, Pencil, TriangleAlert, Info, Eraser, ChevronDown, Check, ArrowUp,
    Palette, GripHorizontal, CircleDot, Type, Wrench, Database, Maximize2, Calendar,
    ChevronLeft, ChevronRight
};

// 在 box 里画图标；fill 不透明时先填充（如筛选面板的实心圆点）
void drawLucide(QPainter &p, LucideIcon icon, const QRectF &box, const QColor &color,
                const QColor &fill = QColor(Qt::transparent));
QPixmap lucidePixmap(LucideIcon icon, int size, const QColor &color, qreal dpr = 1.0,
                     const QColor &fill = QColor(Qt::transparent));
// 普通态 / 悬停态两种颜色（悬停用 QIcon::Active）
QIcon lucideQIcon(LucideIcon icon, int size, const QColor &color, const QColor &hover = QColor());

#endif // LUCIDE_H
