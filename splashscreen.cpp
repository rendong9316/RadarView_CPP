#include "splashscreen.h"

#include <QApplication>
#include <QEventLoop>
#include <QFont>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QPainter>
#include <QPropertyAnimation>
#include <QRadialGradient>
#include <QRegion>
#include <QScreen>
#include <QTimer>
#include <QtGlobal>

#include <cmath>

namespace {

const int kWidth = 760;
const int kHeight = 430;
const int kMinShowMs = 900;      // 最短展示时间：加载再快也让品牌页完整出现一次
const int kFadeMs = 220;
const int kTickMs = 33;
const int kBarCycles = 110;      // 扫光走完一轮需要的 tick 数
const double kPi = 3.14159265358979323846;

// 界面字体：与 ui::uiFamilies() 同一套回退顺序。
// 不能用 QApplication::font()——中文 Windows 上它默认是宋体，标题会变成衬线体。
QFont splashFont(int px, int weight)
{
    QFont f;
    f.setFamilies(QStringList() << QStringLiteral("Segoe UI")
                                << QStringLiteral("Microsoft YaHei")
                                << QStringLiteral("PingFang SC")
                                << QStringLiteral("WenQuanYi Micro Hei")
                                << QStringLiteral("Noto Sans CJK SC")
                                << QStringLiteral("DejaVu Sans"));
    f.setPixelSize(px);
    f.setWeight(weight);
    return f;
}

// 正对视角的地球：球体渐变 + 经纬网 + 外圈高光
void paintGlobe(QPainter &p, const QPointF &c, qreal r)
{
    QRadialGradient sphere(c + QPointF(-r * 0.35, -r * 0.38), r * 1.55);
    sphere.setColorAt(0.00, QColor(0x3a, 0x86, 0xc0));
    sphere.setColorAt(0.45, QColor(0x1a, 0x4a, 0x74));
    sphere.setColorAt(1.00, QColor(0x07, 0x1b, 0x2e));
    p.setPen(Qt::NoPen);
    p.setBrush(sphere);
    p.drawEllipse(c, r, r);

    p.save();
    QRegion clip(QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r).toRect(), QRegion::Ellipse);
    p.setClipRegion(clip);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(120, 190, 230, 70), 1.0));

    // 纬线：圆投影成椭圆，越靠两极越扁
    const double lats[] = { -60.0, -30.0, 0.0, 30.0, 60.0 };
    for (int i = 0; i < 5; ++i) {
        const double a = lats[i] * kPi / 180.0;
        const qreal rx = r * std::cos(a);
        p.drawEllipse(QPointF(c.x(), c.y() - r * std::sin(a)), rx, rx * 0.26);
    }
    // 经线：每 30 度一条，宽度随经度收窄
    for (int i = 1; i <= 5; ++i) {
        const qreal k = std::cos(i * kPi / 6.0);
        p.drawEllipse(c, qAbs(k) * r, r);
    }
    p.restore();

    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(150, 215, 245, 150), 1.2));
    p.drawEllipse(c, r, r);
}

} // namespace

SplashScreen::SplashScreen(QWidget *parent)
    : QWidget(parent, Qt::SplashScreen | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
{
    setAttribute(Qt::WA_ShowWithoutActivating, true);   // 别抢主窗口的焦点
    setAttribute(Qt::WA_DeleteOnClose, true);
    setFixedSize(kWidth, kHeight);

    if (const QScreen *scr = QGuiApplication::primaryScreen()) {
        const QRect g = scr->availableGeometry();
        move(g.center() - QPoint(kWidth / 2, kHeight / 2));
    }

    m_timer = new QTimer(this);
    m_timer->setInterval(kTickMs);
    connect(m_timer, &QTimer::timeout, this, &SplashScreen::tick);
}

void SplashScreen::showNow()
{
    m_clock.start();
    m_timer->start();
    show();
    raise();
    // 主窗口构造在下一步就阻塞事件循环，这里先把启动页真正画到屏幕上
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

void SplashScreen::finish()
{
    m_ready = true;
}

void SplashScreen::tick()
{
    ++m_phase;
    update();
    if (!m_fading && m_ready && m_clock.elapsed() >= kMinShowMs)
        startFade();
}

void SplashScreen::startFade()
{
    m_fading = true;
    m_timer->stop();

    QPropertyAnimation *anim = new QPropertyAnimation(this, "windowOpacity", this);
    anim->setDuration(kFadeMs);
    anim->setStartValue(1.0);
    anim->setEndValue(0.0);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    connect(anim, &QPropertyAnimation::finished, this, &QWidget::close);
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

void SplashScreen::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    const int w = width();
    const int h = height();

    // 背景：与主界面暗色主题同色系，自上而下由深到浅
    QLinearGradient bg(0, 0, 0, h);
    bg.setColorAt(0.00, QColor(0x08, 0x0d, 0x16));
    bg.setColorAt(0.35, QColor(0x0b, 0x13, 0x22));
    bg.setColorAt(0.70, QColor(0x0e, 0x18, 0x2a));
    bg.setColorAt(1.00, QColor(0x11, 0x1d, 0x33));
    p.fillRect(rect(), bg);

    // 地球背后的柔光
    const QPointF c(w / 2.0, h * 0.355);
    const qreal r = 84.0;
    QRadialGradient glow(c, r * 2.2);
    glow.setColorAt(0.0, QColor(64, 150, 210, 62));
    glow.setColorAt(1.0, QColor(64, 150, 210, 0));
    p.setPen(Qt::NoPen);
    p.setBrush(glow);
    p.drawEllipse(c, r * 2.2, r * 2.2);

    paintGlobe(p, c, r);

    // 标题与副标题
    QFont title = splashFont(36, QFont::DemiBold);
    title.setLetterSpacing(QFont::AbsoluteSpacing, 3.0);
    p.setFont(title);
    p.setPen(QColor(0xe8, 0xf2, 0xfa));
    p.drawText(QRectF(0, h * 0.355 + r + 22, w, 48),
               Qt::AlignHCenter | Qt::AlignVCenter, QStringLiteral("RadarView"));

    QFont sub = splashFont(12, QFont::Normal);
    sub.setLetterSpacing(QFont::AbsoluteSpacing, 2.0);
    p.setFont(sub);
    p.setPen(QColor(160, 190, 215, 150));
    p.drawText(QRectF(0, h * 0.355 + r + 72, w, 22),
               Qt::AlignHCenter | Qt::AlignVCenter, QStringLiteral("三维地球 · 航迹可视化平台"));

    // 底部状态行
    QFont small = splashFont(11, QFont::Normal);
    p.setFont(small);
    p.setPen(QColor(0x5a, 0x65, 0x77));
    p.drawText(QRectF(64, h - 40, w - 128, 20),
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("正在加载地图与航迹数据…"));
    p.setPen(QColor(140, 155, 175, 80));
    p.drawText(QRectF(64, h - 40, w - 128, 20),
               Qt::AlignRight | Qt::AlignVCenter,
               QStringLiteral("Qt %1").arg(QString::fromLatin1(QT_VERSION_STR)));

    // 底部扫光进度条
    const qreal barY = h - 3;
    p.setPen(Qt::NoPen);
    p.fillRect(QRectF(0, barY, w, 3), QColor(109, 179, 216, 28));

    const qreal segW = w * 0.32;
    const qreal travel = w + segW;
    const qreal x = -segW + travel * ((m_phase % kBarCycles) / qreal(kBarCycles));
    QLinearGradient g(x, 0, x + segW, 0);
    g.setColorAt(0.0, QColor(109, 179, 216, 0));
    g.setColorAt(0.5, QColor(109, 179, 216, 225));
    g.setColorAt(1.0, QColor(109, 179, 216, 0));
    p.fillRect(QRectF(x, barY, segW, 3), g);

    // 无边框窗口描一道极淡的边，避免和桌面糊在一起
    p.setBrush(Qt::NoBrush);
    p.setPen(QColor(120, 160, 200, 30));
    p.drawRect(rect().adjusted(0, 0, -1, -1));
}
