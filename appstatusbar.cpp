#include "appstatusbar.h"
#include "replaycontroller.h"
#include "track.h"
#include "apppaths.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QDoubleValidator>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QTimer>
#include <cmath>
#include <functional>

namespace {

const int kBarHeight = 22;          // .statusbar { height: 22px; border-top: 1px }（border-box）
const int kPadX = 8;                // padding: 0 8px
const int kGap = 6;                 // .statusbar-left/right { gap: 6px }
const int kFontPx = 11;             // 0.786rem（根字号 14px）
const int kSmallFontPx = 10;        // 0.714rem
const int kSpeedOptions[] = { 1, 10, 50, 100, 300, 500, 2000 };   // useReplay.ts SPEED_OPTIONS

// 界面字体：RadarView 为 'Segoe UI', 'PingFang SC', 'Microsoft YaHei', sans-serif；补上 Linux 中文字体
QFont uiFont(int px)
{
    QFont f = QApplication::font();
    f.setFamilies(QStringList() << QStringLiteral("Segoe UI") << QStringLiteral("Microsoft YaHei")
                                << QStringLiteral("WenQuanYi Micro Hei") << QStringLiteral("Noto Sans CJK SC")
                                << QApplication::font().family());
    f.setPixelSize(px);
    return f;
}

// 等宽字体：RadarView 为 'Cascadia Code', 'JetBrains Mono', 'Consolas', 'Courier New', monospace
QFont monoFont(int px)
{
    QFont f = QApplication::font();
    f.setFamilies(QStringList() << QStringLiteral("Cascadia Code") << QStringLiteral("JetBrains Mono")
                                << QStringLiteral("Consolas") << QStringLiteral("DejaVu Sans Mono")
                                << QStringLiteral("Liberation Mono") << QStringLiteral("Courier New")
                                << QApplication::font().family());
    f.setStyleHint(QFont::TypeWriter);
    f.setPixelSize(px);
    return f;
}

StatusTheme themeById(const QString &id)
{
    StatusTheme t;
    if (id == QLatin1String("light")) {
        t = StatusTheme{ id, QColor("#005fb8"), QColor("#ffffff"), QColor("#004c9a"),
                         QColor("#ececec"), QColor("#005fb8"), QColor("#e51400") };
    } else if (id == QLatin1String("hc")) {
        t = StatusTheme{ id, QColor("#000000"), QColor("#ffffff"), QColor("#6fc3df"),
                         QColor("#111111"), QColor("#1aebff"), QColor("#f44747") };
    } else {
        t = StatusTheme{ QStringLiteral("dark"), QColor("#1f2330"), QColor("#e0e0e0"), QColor("#2d3240"),
                         QColor("#2d2d2d"), QColor("#007acc"), QColor("#f44747") };
    }
    return t;
}

QColor withAlpha(QColor c, double a)
{
    c.setAlphaF(a);
    return c;
}

// 前景色按 opacity 与背景混合（CSS 的 opacity 作用在文字上）
QColor blend(const QColor &fg, const QColor &bg, double opacity)
{
    return QColor::fromRgbF(fg.redF() * opacity + bg.redF() * (1.0 - opacity),
                            fg.greenF() * opacity + bg.greenF() * (1.0 - opacity),
                            fg.blueF() * opacity + bg.blueF() * (1.0 - opacity));
}

QString cssColor(const QColor &c)
{
    return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alphaF(), 0, 'f', 3);
}

// StatusBar.vue formatHeightKm / formatCoordinate
QString formatHeightKm(double v)
{
    if (!std::isfinite(v))
        return QStringLiteral("0");
    if (v >= 1000.0)
        return QString::number(v, 'f', 0);
    if (v >= 100.0)
        return QString::number(v, 'f', 1);
    return QString::number(v, 'f', 2);
}

QString formatCoordinate(double v)
{
    if (!std::isfinite(v))
        return QStringLiteral("0.0000");
    QString s = QString::number(v, 'f', 4);
    if (s == QLatin1String("-0.0000"))
        s = QStringLiteral("0.0000");
    return s;
}

// lucide 图标（viewBox 24×24、线宽 2、圆角端点），用 QPainter 画，不依赖 QtSvg
enum class Icon { Play, Pause, Moon, Sun, Contrast, Alert };

void drawIcon(QPainter &p, Icon icon, const QRectF &box, const QColor &color)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(box.topLeft());
    p.scale(box.width() / 24.0, box.height() / 24.0);
    QPen pen(color, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    switch (icon) {
    case Icon::Play: {
        const QPointF tri[3] = { QPointF(6, 3), QPointF(20, 12), QPointF(6, 21) };
        p.drawPolygon(tri, 3);
        break;
    }
    case Icon::Pause:
        p.drawRoundedRect(QRectF(14, 4, 4, 16), 1, 1);
        p.drawRoundedRect(QRectF(6, 4, 4, 16), 1, 1);
        break;
    case Icon::Moon: {
        // M12 3a6 6 0 0 0 9 9 9 9 0 1 1-9-9Z：半径 9 的圆减去以 (16.5,7.5) 为心、过 (12,3)(21,12) 的圆
        QPainterPath outer, cut;
        outer.addEllipse(QPointF(12, 12), 9, 9);
        const double r = std::sqrt(4.5 * 4.5 * 2.0);
        cut.addEllipse(QPointF(16.5, 7.5), r, r);
        p.drawPath(outer.subtracted(cut));
        break;
    }
    case Icon::Sun:
        p.drawEllipse(QPointF(12, 12), 4, 4);
        p.drawLine(QPointF(12, 2), QPointF(12, 4));
        p.drawLine(QPointF(12, 20), QPointF(12, 22));
        p.drawLine(QPointF(4.93, 4.93), QPointF(6.34, 6.34));
        p.drawLine(QPointF(17.66, 17.66), QPointF(19.07, 19.07));
        p.drawLine(QPointF(2, 12), QPointF(4, 12));
        p.drawLine(QPointF(20, 12), QPointF(22, 12));
        p.drawLine(QPointF(6.34, 17.66), QPointF(4.93, 19.07));
        p.drawLine(QPointF(19.07, 4.93), QPointF(17.66, 6.34));
        break;
    case Icon::Contrast: {
        p.drawEllipse(QPointF(12, 12), 10, 10);
        QPainterPath half;                          // M12 18a6 6 0 0 0 0-12v12z：右半圆
        half.moveTo(12, 18);
        half.arcTo(QRectF(6, 6, 12, 12), -90, 180);
        half.closeSubpath();
        p.drawPath(half);
        break;
    }
    case Icon::Alert: {
        const QPointF tri[3] = { QPointF(12, 3.2), QPointF(21.9, 20.4), QPointF(2.1, 20.4) };
        p.drawPolygon(tri, 3);
        p.drawLine(QPointF(12, 9), QPointF(12, 13));
        p.drawLine(QPointF(12, 17), QPointF(12.01, 17));
        break;
    }
    }
    p.restore();
}

} // namespace

namespace statusbar_detail {

// .status-btn：22×22 图标按钮，悬停半透明白底，禁用时 opacity 0.4
class IconButton : public QAbstractButton
{
public:
    IconButton(Icon icon, int width, QWidget *parent) : QAbstractButton(parent), m_icon(icon)
    {
        setFixedSize(width, kBarHeight - 1);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
    }
    void setIcon(Icon icon) { m_icon = icon; update(); }
    Icon icon() const { return m_icon; }
    void setColor(const QColor &c) { m_color = c; update(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (isEnabled() && underMouse()) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 38));
            p.drawRoundedRect(rect(), 3, 3);
        }
        p.setOpacity(isEnabled() ? 1.0 : 0.4);
        drawIcon(p, m_icon, QRectF((width() - 14) / 2.0, (height() - 14) / 2.0, 14, 14), m_color);
    }
    void changeEvent(QEvent *e) override
    {
        if (e->type() == QEvent::EnabledChange)
            setCursor(isEnabled() ? Qt::PointingHandCursor : Qt::ArrowCursor);
        QAbstractButton::changeEvent(e);
    }

private:
    Icon m_icon;
    QColor m_color = Qt::white;
};

// .status-progress：4px 轨道 + 填充 + 悬停/拖动时显示的 12px 圆形滑块；点击跳转、拖动定位
class SeekBar : public QWidget
{
public:
    std::function<void(double)> onSeek;

    explicit SeekBar(QWidget *parent) : QWidget(parent)
    {
        setMinimumWidth(60);
        setMaximumWidth(300);
        setFixedHeight(kBarHeight - 1);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setAttribute(Qt::WA_Hover);
        setToolTip(QStringLiteral("拖动或点击跳转回放位置"));
        setCursor(Qt::PointingHandCursor);
    }
    void setProgress(double v)
    {
        v = qBound(0.0, std::isfinite(v) ? v : 0.0, 1.0);
        if (v != m_progress) {
            m_progress = v;
            update();
        }
    }
    double progress() const { return m_progress; }
    void setColors(const QColor &fg, const QColor &track) { m_fg = fg; m_track = track; update(); }
    QSize sizeHint() const override { return QSize(300, kBarHeight - 1); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setOpacity(isEnabled() ? 1.0 : 0.5);
        const QRectF track(0, height() / 2.0 - 2.0, width(), 4.0);
        p.setPen(Qt::NoPen);
        p.setBrush(m_track);
        p.drawRoundedRect(track, 2, 2);
        const double x = m_progress * width();
        if (x > 0.0) {
            p.setBrush(m_fg);
            p.drawRoundedRect(QRectF(0, track.top(), x, 4.0), 2, 2);
        }
        if (isEnabled() && (underMouse() || m_dragging)) {
            const QPointF c(x, height() / 2.0);
            p.setBrush(QColor(0, 0, 0, 60));               // box-shadow: 0 0 4px rgba(0,0,0,0.4)
            p.drawEllipse(c, 8.0, 8.0);
            p.setBrush(m_fg);
            p.setPen(QPen(QColor(255, 255, 255, 230), 2.0));
            p.drawEllipse(c, 5.0, 5.0);
        }
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (!isEnabled() || e->button() != Qt::LeftButton)
            return;
        m_dragging = true;
        setCursor(Qt::ClosedHandCursor);
        seekTo(e->pos().x());
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (m_dragging)
            seekTo(e->pos().x());
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton || !m_dragging)
            return;
        m_dragging = false;
        setCursor(Qt::PointingHandCursor);
        update();
    }
    void changeEvent(QEvent *e) override
    {
        if (e->type() == QEvent::EnabledChange) {
            m_dragging = false;
            setCursor(isEnabled() ? Qt::PointingHandCursor : Qt::ArrowCursor);
        }
        QWidget::changeEvent(e);
    }

private:
    void seekTo(int x)
    {
        if (onSeek)
            onSeek(qBound(0.0, double(x) / qMax(1, width()), 1.0));
    }

    double m_progress = 0.0;
    bool m_dragging = false;
    QColor m_fg = Qt::white, m_track = Qt::darkGray;
};

// .status-speed-select：高 16px、字号 10px 的下拉框，自己画外观（避免各平台原生箭头颜色不一）
class SpeedSelect : public QComboBox
{
public:
    explicit SpeedSelect(QWidget *parent) : QComboBox(parent)
    {
        setFocusPolicy(Qt::NoFocus);
        setFont(monoFont(kSmallFontPx));
        setFixedHeight(16);
        setToolTip(QStringLiteral("选择回放倍速"));
        setCursor(Qt::PointingHandCursor);
    }
    void setColors(const QColor &fg, const QColor &bg) { m_fg = fg; m_bg = bg; update(); }
    QSize sizeHint() const override
    {
        int w = 0;
        for (int i = 0; i < count(); ++i)
            w = qMax(w, fontMetrics().horizontalAdvance(itemText(i)));
        return QSize(qMin(72, w + 2 * 2 + 2 + 10), 16);   // max-width: 72px
    }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(QPen(QColor(255, 255, 255, 38), 1.0));
        p.setBrush(QColor(255, 255, 255, 20));
        p.drawRoundedRect(r, 2, 2);
        p.setPen(m_fg);
        p.setFont(font());
        const QRect textRect = rect().adjusted(3, 0, -11, 0);
        p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                   fontMetrics().elidedText(currentText(), Qt::ElideRight, textRect.width()));
        // 下拉箭头
        QPen pen(m_fg, 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        const double cx = width() - 6.0, cy = height() / 2.0;
        const QPointF chevron[3] = { QPointF(cx - 2.5, cy - 1.2), QPointF(cx, cy + 1.3), QPointF(cx + 2.5, cy - 1.2) };
        p.drawPolyline(chevron, 3);
    }

private:
    QColor m_fg = Qt::white, m_bg = Qt::black;
};

// .load-spinner：10px 圆环，顶部一段为强调色，0.6 s 转一圈
class Spinner : public QWidget
{
public:
    explicit Spinner(QWidget *parent) : QWidget(parent)
    {
        setFixedSize(10, 10);
        m_timer.setInterval(16);
        QObject::connect(&m_timer, &QTimer::timeout, this, [this]() {
            m_angle = std::fmod(m_angle + 360.0 * 16.0 / 600.0, 360.0);
            update();
        });
    }
    void setColor(const QColor &c) { m_color = c; update(); }

protected:
    void showEvent(QShowEvent *) override { m_timer.start(); }
    void hideEvent(QHideEvent *) override { m_timer.stop(); }
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r(1, 1, 8, 8);
        p.setPen(QPen(QColor(255, 255, 255, 51), 2.0));
        p.drawEllipse(r);
        p.setPen(QPen(m_color, 2.0, Qt::SolidLine, Qt::FlatCap));
        // border-top 着色的圆：45°~135° 那一段，整体按 m_angle 顺时针旋转
        p.drawArc(r, int((45.0 - m_angle) * 16), 90 * 16);
    }

private:
    QTimer m_timer;
    double m_angle = 0.0;
    QColor m_color = Qt::white;
};

// .status-error：警告图标 + 文字，最宽 200px，超出省略，悬停显示全文
class ErrorTag : public QWidget
{
public:
    explicit ErrorTag(QWidget *parent) : QWidget(parent)
    {
        setFont(uiFont(kFontPx));
        setFixedHeight(kBarHeight - 1);
    }
    void setText(const QString &t)
    {
        m_text = t;
        setToolTip(t);
        updateGeometry();
        update();
    }
    QString text() const { return m_text; }
    void setColor(const QColor &c) { m_color = c; update(); }
    QSize sizeHint() const override
    {
        return QSize(qMin(200, 12 + 3 + fontMetrics().horizontalAdvance(m_text)), kBarHeight - 1);
    }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        drawIcon(p, Icon::Alert, QRectF(0, (height() - 12) / 2.0, 12, 12), m_color);
        p.setPen(m_color);
        const QRect tr = rect().adjusted(15, 0, 0, 0);
        p.drawText(tr, Qt::AlignVCenter | Qt::AlignLeft, fontMetrics().elidedText(m_text, Qt::ElideRight, tr.width()));
    }

private:
    QString m_text;
    QColor m_color = Qt::red;
};

// .status-source：7px 色点（隐藏时 opacity 0.3）+ "名称:航迹数"，点击切换显隐
class SourceButton : public QAbstractButton
{
public:
    explicit SourceButton(QWidget *parent) : QAbstractButton(parent)
    {
        setFont(uiFont(kFontPx));
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
    }
    void setItem(const AppStatusBar::SourceItem &item)
    {
        m_item = item;
        setText(QStringLiteral("%1:%2").arg(item.label).arg(item.count));
        setToolTip(QStringLiteral("点击切换 %1 可见性").arg(item.label));
        updateGeometry();
        update();
    }
    const AppStatusBar::SourceItem &item() const { return m_item; }
    void setTextColor(const QColor &c) { m_fg = c; update(); }
    QSize sizeHint() const override
    {
        return QSize(4 + 7 + 3 + fontMetrics().horizontalAdvance(text()) + 4, fontMetrics().height() + 2);
    }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (underMouse()) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 26));
            p.drawRoundedRect(rect(), 3, 3);
        }
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(m_item.color, m_item.visible ? 1.0 : 0.3));
        p.drawEllipse(QRectF(4, (height() - 7) / 2.0, 7, 7));
        p.setPen(m_fg);
        p.drawText(rect().adjusted(4 + 7 + 3, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, text());
    }

private:
    AppStatusBar::SourceItem m_item;
    QColor m_fg = Qt::white;
};

} // namespace statusbar_detail

using namespace statusbar_detail;

// ---------------------------------------------------------------
//  AppStatusBar
// ---------------------------------------------------------------
AppStatusBar::AppStatusBar(ReplayController *replay, QWidget *parent)
    : QWidget(parent), m_ctrl(replay)
{
    setObjectName(QStringLiteral("appStatusBar"));
    setFixedHeight(kBarHeight);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);   // 窗口变窄时右侧裁掉，不撑大主窗口
    setFont(uiFont(kFontPx));

    // ---- 左：回放控件 ----
    m_left = new QWidget(this);
    QHBoxLayout *left = new QHBoxLayout(m_left);
    left->setContentsMargins(0, 0, 0, 0);
    left->setSpacing(kGap);
    m_play = new IconButton(Icon::Play, kBarHeight, m_left);
    left->addWidget(m_play);
    m_seek = new SeekBar(m_left);
    left->addWidget(m_seek, 1);
    m_time = new QLabel(m_left);
    m_time->setFont(monoFont(kFontPx));
    left->addWidget(m_time);

    QWidget *speedBox = m_speedBox = new QWidget(m_left);
    speedBox->setObjectName(QStringLiteral("speedBox"));
    QHBoxLayout *speedLay = new QHBoxLayout(speedBox);
    speedLay->setContentsMargins(0, 0, 0, 0);
    speedLay->setSpacing(2);
    m_speed = new SpeedSelect(speedBox);
    for (int s : kSpeedOptions)
        m_speed->addItem(QStringLiteral("%1x").arg(s), s);
    m_speed->addItem(QStringLiteral("自定义..."), 0);
    speedLay->addWidget(m_speed);
    m_customSpeed = new QLineEdit(speedBox);
    m_customSpeed->setFont(monoFont(kSmallFontPx));
    m_customSpeed->setFixedSize(64, 16);
    m_customSpeed->setToolTip(QStringLiteral("输入自定义倍速，按回车键确认生效"));
    QDoubleValidator *val = new QDoubleValidator(0.0, 1e9, 3, m_customSpeed);
    val->setNotation(QDoubleValidator::StandardNotation);
    val->setLocale(QLocale::c());
    m_customSpeed->setValidator(val);
    speedLay->addWidget(m_customSpeed);
    left->addWidget(speedBox);
    left->addStretch(0);

    // ---- 右：信息 ----
    QWidget *rightClip = m_rightClip = new QWidget(this);   // 宽度不够时裁掉左边超出的部分（overflow: hidden）
    rightClip->setObjectName(QStringLiteral("rightClip"));
    m_right = new QWidget(rightClip);
    QHBoxLayout *right = new QHBoxLayout(m_right);
    right->setContentsMargins(0, 0, 0, 0);
    right->setSpacing(kGap);
    m_spinner = new Spinner(m_right);
    m_loading = new QLabel(m_right);
    QWidget *loadingBox = m_loadingBox = new QWidget(m_right);
    loadingBox->setObjectName(QStringLiteral("loadingBox"));
    QHBoxLayout *loadingLay = new QHBoxLayout(loadingBox);
    loadingLay->setContentsMargins(0, 0, 0, 0);
    loadingLay->setSpacing(4);
    m_spinner->setParent(loadingBox);
    m_loading->setParent(loadingBox);
    loadingLay->addWidget(m_spinner);
    loadingLay->addWidget(m_loading);
    right->addWidget(loadingBox);
    loadingBox->hide();
    m_error = new ErrorTag(m_right);
    m_error->hide();
    right->addWidget(m_error);
    m_height = new QLabel(m_right);
    m_lonLat = new QLabel(m_right);
    m_fps = new QLabel(m_right);
    for (QLabel *l : { m_height, m_lonLat, m_fps }) {
        l->setFont(monoFont(kFontPx));
        right->addWidget(l);
    }
    m_sourceLayout = new QHBoxLayout;
    m_sourceLayout->setSpacing(kGap);
    m_sourceLayout->setContentsMargins(0, 0, 0, 0);
    right->addLayout(m_sourceLayout);
    m_count = new QLabel(m_right);
    m_count->setFont(monoFont(kFontPx));
    right->addWidget(m_count);
    m_themeBtn = new IconButton(Icon::Moon, 22, m_right);   // .status-theme { width: auto; padding: 0 4px }
    m_themeBtn->setToolTip(QStringLiteral("切换主题"));
    right->addWidget(m_themeBtn);

    m_left->installEventFilter(this);
    m_right->installEventFilter(this);

    // ---- 交互 ----
    connect(m_play, &QAbstractButton::clicked, m_ctrl, &ReplayController::togglePlay);
    m_seek->onSeek = [this](double f) { m_ctrl->seek(f); };
    connect(m_speed, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
        const int v = m_speed->itemData(i).toInt();
        if (v <= 0) {                     // 「自定义...」：只显示输入框，倍速不变
            m_showCustom = true;
        } else {
            m_showCustom = false;
            m_ctrl->setSpeed(v);
        }
        syncState();
    });
    connect(m_customSpeed, &QLineEdit::textEdited, this, &AppStatusBar::applyCustomSpeed);
    connect(m_customSpeed, &QLineEdit::editingFinished, this, [this]() {
        applyCustomSpeed(m_customSpeed->text());
        syncState();
    });
    connect(m_themeBtn, &QAbstractButton::clicked, this, &AppStatusBar::cycleTheme);
    connect(m_ctrl, &ReplayController::stateChanged, this, &AppStatusBar::syncState);
    connect(m_ctrl, &ReplayController::timeChanged, this, &AppStatusBar::syncTime);

    QSettings &settings = app::settings();
    m_theme = themeById(settings.value(QStringLiteral("theme"), QStringLiteral("dark")).toString());
    m_showCustom = false;
    setViewStatus(0.0, 0.0, 0.0, 0);
    setTrackCount(0);
    setSources(QVector<SourceItem>());
    applyTheme();
    syncState();
}

// ---------------------------------------------------------------
//  回放：按钮 / 进度 / 时间 / 倍速，全部直接取自 ReplayController，任何时刻都一致
// ---------------------------------------------------------------
void AppStatusBar::syncState()
{
    const bool hasData = m_ctrl->end() > m_ctrl->start();
    m_play->setEnabled(hasData);
    m_play->setIcon(m_ctrl->isPlaying() ? Icon::Pause : Icon::Play);
    m_play->setToolTip(m_ctrl->isPlaying() ? QStringLiteral("暂停") : QStringLiteral("播放"));
    m_seek->setEnabled(hasData);
    m_time->setVisible(hasData);
    m_speedBox->setVisible(hasData);

    // 倍速：在预设里且没点「自定义...」就选中预设，否则选「自定义...」并显示输入框
    const double speed = m_ctrl->speed();
    int preset = -1;
    for (int i = 0; i < m_speed->count() - 1; ++i)
        if (m_speed->itemData(i).toInt() == speed)
            preset = i;
    if (preset >= 0 && !m_customSpeed->hasFocus())
        m_showCustom = false;              // StatusBar.vue: watch(speed) 命中预设时收起输入框
    const bool custom = m_showCustom || preset < 0;
    m_speed->setCurrentIndex(custom ? m_speed->count() - 1 : preset);
    m_customSpeed->setVisible(custom || m_customSpeed->hasFocus());
    if (!m_customSpeed->hasFocus())
        m_customSpeed->setText(QString::number(speed));
    syncTime();
}

void AppStatusBar::syncTime()
{
    const bool hasData = m_ctrl->end() > m_ctrl->start();
    m_seek->setProgress(hasData ? m_ctrl->progress() : 0.0);
    const QString text = formatBeijingTime(m_ctrl->current()) + QStringLiteral(" / ")
                       + (hasData ? formatBeijingTime(m_ctrl->end()) : QStringLiteral("--"));
    if (m_time->text() != text)
        m_time->setText(text);
}

void AppStatusBar::applyCustomSpeed(const QString &text)
{
    bool ok = false;
    const double v = text.trimmed().toDouble(&ok);
    if (ok && std::isfinite(v) && v > 0.0)
        m_ctrl->setSpeed(v);
}

// ---------------------------------------------------------------
//  右侧信息
// ---------------------------------------------------------------
void AppStatusBar::setViewStatus(double heightKm, double lonDeg, double latDeg, int fps)
{
    const QString h = formatHeightKm(qMax(0.0, heightKm));
    const QString lon = formatCoordinate(lonDeg), lat = formatCoordinate(latDeg);
    const QString f = fps > 0 ? QString::number(fps) : QStringLiteral("--");
    const QString ht = QStringLiteral("高: %1 km").arg(h);
    const QString lt = QStringLiteral("经纬: %1, %2").arg(lon, lat);
    const QString ft = QStringLiteral("FPS: %1").arg(f);
    if (m_height->text() != ht) {
        m_height->setText(ht);
        m_height->setToolTip(QStringLiteral("相机高度 %1 km").arg(h));
    }
    if (m_lonLat->text() != lt) {
        m_lonLat->setText(lt);
        m_lonLat->setToolTip(QStringLiteral("鼠标经纬度 %1, %2").arg(lon, lat));
    }
    if (m_fps->text() != ft) {
        m_fps->setText(ft);
        m_fps->setToolTip(QStringLiteral("渲染帧率 %1 FPS").arg(f));
    }
}

void AppStatusBar::setSources(const QVector<SourceItem> &items)
{
    while (m_sourceButtons.size() > items.size())
        delete m_sourceButtons.takeLast();
    while (m_sourceButtons.size() < items.size()) {
        SourceButton *b = new SourceButton(m_right);
        b->setTextColor(m_theme.fg);
        connect(b, &QAbstractButton::clicked, this, [this, b]() { emit sourceToggled(b->item().key); });
        m_sourceLayout->addWidget(b);
        m_sourceButtons.append(b);
    }
    for (int i = 0; i < items.size(); ++i)
        m_sourceButtons[i]->setItem(items[i]);
}

void AppStatusBar::setTrackCount(int count)
{
    m_count->setText(QStringLiteral("航迹: %1").arg(count));
}

void AppStatusBar::setLoading(bool loading, int percent)
{
    m_isLoading = loading;
    m_loadPercent = qBound(0, percent, 100);
    syncLoading();
}

// StatusBar.vue：导入中显示百分比，后台入库时显示「保存中」
void AppStatusBar::setPersisting(bool persisting)
{
    m_persisting = persisting;
    syncLoading();
}

void AppStatusBar::syncLoading()
{
    m_loading->setText(m_isLoading ? QStringLiteral("%1%").arg(m_loadPercent) : QStringLiteral("保存中"));
    m_loadingBox->setVisible(m_isLoading || m_persisting);
}

QString AppStatusBar::loadingText() const
{
    return m_loadingBox->isVisible() ? m_loading->text() : QString();
}

void AppStatusBar::setError(const QString &message)
{
    m_error->setText(message);
    m_error->setVisible(!message.isEmpty());
}

// ---------------------------------------------------------------
//  主题
// ---------------------------------------------------------------
void AppStatusBar::setTheme(const QString &id)
{
    m_theme = themeById(id);
    QSettings &settings = app::settings();
    settings.setValue(QStringLiteral("theme"), m_theme.id);
    applyTheme();
    emit themeChanged(m_theme.id);
}

void AppStatusBar::cycleTheme()
{
    static const char *const order[] = { "dark", "light", "hc" };
    int idx = 0;
    for (int i = 0; i < 3; ++i)
        if (m_theme.id == QLatin1String(order[i]))
            idx = i;
    setTheme(QLatin1String(order[(idx + 1) % 3]));
}

void AppStatusBar::setLabelColor(QLabel *label, double opacity)
{
    QPalette pal = label->palette();
    pal.setColor(QPalette::WindowText, blend(m_theme.fg, m_theme.bg, opacity));
    label->setPalette(pal);
}

void AppStatusBar::applyTheme()
{
    m_play->setColor(m_theme.fg);
    m_themeBtn->setColor(m_theme.fg);
    m_themeBtn->setIcon(m_theme.id == QLatin1String("dark") ? Icon::Moon
                        : m_theme.id == QLatin1String("light") ? Icon::Sun : Icon::Contrast);
    m_seek->setColors(m_theme.fg, m_theme.track);
    m_speed->setColors(m_theme.fg, m_theme.bg);
    m_speed->view()->setStyleSheet(QStringLiteral("QAbstractItemView { background: %1; color: %2;"
                                                  " selection-background-color: %3; selection-color: %2; }")
                                       .arg(m_theme.bg.name(), m_theme.fg.name(), cssColor(withAlpha(m_theme.fg, 0.2))));
    m_customSpeed->setStyleSheet(QStringLiteral("QLineEdit { padding: 0 4px; background: rgba(255,255,255,0.08);"
                                                " border: 1px solid rgba(255,255,255,0.15); border-radius: 2px; color: %1; }")
                                     .arg(m_theme.fg.name()));
    setLabelColor(m_time, 1.0);
    for (QLabel *l : { m_height, m_lonLat, m_fps })
        setLabelColor(l, 0.85);
    setLabelColor(m_count, 0.8);
    QPalette pal = m_loading->palette();
    pal.setColor(QPalette::WindowText, m_theme.accent);
    m_loading->setPalette(pal);
    m_spinner->setColor(m_theme.accent);
    m_error->setColor(m_theme.error);
    for (SourceButton *b : qAsConst(m_sourceButtons))
        b->setTextColor(m_theme.fg);
    update();
}

// ---------------------------------------------------------------
//  绘制与布局
// ---------------------------------------------------------------
void AppStatusBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), m_theme.bg);
    p.fillRect(QRect(0, 0, width(), 1), m_theme.border);
}

void AppStatusBar::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    updateSeekWidth();
}

bool AppStatusBar::eventFilter(QObject *obj, QEvent *e)
{
    if ((obj == m_left || obj == m_right) && e->type() == QEvent::LayoutRequest)
        QTimer::singleShot(0, this, &AppStatusBar::updateSeekWidth);
    return QWidget::eventFilter(obj, e);
}

// 左侧占满剩余宽度（flex: 1），右侧按内容宽度靠右（flex: 0 1 auto），太窄时右侧从左边裁掉
void AppStatusBar::updateSeekWidth()
{
    const int avail = qMax(0, width() - 2 * kPadX);
    const int h = kBarHeight - 1;
    const int rightHint = m_right->sizeHint().width();
    const int leftMin = m_left->minimumSizeHint().width();
    const int rightW = qBound(0, qMin(rightHint, avail - leftMin), avail);
    const int leftW = avail - rightW;
    m_left->setGeometry(kPadX, 1, leftW, h);
    m_rightClip->setGeometry(kPadX + leftW, 1, rightW, h);
    m_right->setGeometry(rightW - rightHint, 0, rightHint, h);
}

// ---------------------------------------------------------------
//  自检用
// ---------------------------------------------------------------
QString AppStatusBar::timeText() const { return m_time->isVisible() ? m_time->text() : QString(); }
QString AppStatusBar::speedText() const
{
    return m_customSpeed->isVisible() ? m_speed->currentText() + QStringLiteral(" ") + m_customSpeed->text()
                                      : m_speed->currentText();
}
QString AppStatusBar::heightText() const { return m_height->text(); }
QString AppStatusBar::lonLatText() const { return m_lonLat->text(); }
QString AppStatusBar::fpsText() const { return m_fps->text(); }
QString AppStatusBar::trackCountText() const { return m_count->text(); }
QString AppStatusBar::errorText() const { return m_error->isVisible() ? m_error->text() : QString(); }

QStringList AppStatusBar::sourceTexts() const
{
    QStringList out;
    for (const SourceButton *b : m_sourceButtons)
        out << (b->item().visible ? QString() : QStringLiteral("off ")) + b->text();
    return out;
}

bool AppStatusBar::isLoadingShown() const
{
    return m_loadingBox->isVisible();
}

bool AppStatusBar::areReplayControlsShown() const
{
    return isVisible() && m_play->isVisible() && m_seek->isVisible() && m_seek->width() >= 60;
}

bool AppStatusBar::isTimeShown() const
{
    return m_time->isVisible() && m_speed->isVisible();
}

bool AppStatusBar::isCustomSpeedShown() const { return m_customSpeed->isVisible(); }
double AppStatusBar::seekProgress() const { return m_seek->progress(); }

void AppStatusBar::clickSource(int index)
{
    if (index >= 0 && index < m_sourceButtons.size())
        m_sourceButtons[index]->click();
}

void AppStatusBar::clickTheme() { m_themeBtn->click(); }

bool AppStatusBar::checkLayout(QString *report) const
{
    // 依次检查左、右两侧可见项：都在状态栏垂直范围内、从左到右排列、互不重叠
    QList<QWidget *> items;
    items << m_play << m_seek << m_time << m_speed << m_customSpeed;
    items << m_loadingBox << m_error << m_height << m_lonLat << m_fps;
    for (SourceButton *b : m_sourceButtons)
        items << b;
    items << m_count << m_themeBtn;

    QStringList parts;
    bool ok = true;
    int lastRight = -1;
    const QRect bar = rect();
    AppStatusBar *self = const_cast<AppStatusBar *>(this);
    // 右侧太宽时从左边被裁掉（与 RadarView overflow: hidden 一致），只按实际露出的部分检查
    const QRect clip(m_rightClip->mapTo(self, QPoint(0, 0)), m_rightClip->size());
    for (QWidget *w : items) {
        if (!w->isVisible())
            continue;
        QRect g(w->mapTo(self, QPoint(0, 0)), w->size());
        if (m_right->isAncestorOf(w)) {
            g = g.intersected(clip);
            if (g.isEmpty())
                continue;
        }
        const QString name = w->objectName().isEmpty() ? QString::fromLatin1(w->metaObject()->className())
                                                        : w->objectName();
        parts << QStringLiteral("%1[%2,%3 %4x%5]").arg(name).arg(g.x()).arg(g.y()).arg(g.width()).arg(g.height());
        if (g.top() < 0 || g.bottom() > bar.bottom() || g.left() < 0 || g.right() > bar.right()
                || g.width() <= 0 || g.left() < lastRight) {
            ok = false;
            parts.last() += QStringLiteral("!");
        }
        lastRight = g.right() + 1;
    }
    if (report)
        *report = parts.join(QLatin1Char(' '));
    return ok;
}
