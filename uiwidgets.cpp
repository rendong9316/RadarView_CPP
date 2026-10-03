#include "uiwidgets.h"
#include "theme.h"

#include <QApplication>
#include <QDialog>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QStyleOption>
#include <QVBoxLayout>
#include <QWidgetAction>

namespace ui {

namespace {
bool g_autoConfirm = false;

// 遮罩 + 居中卡片的模态对话框：覆盖整个主窗口，点遮罩取消
class OverlayDialog : public QDialog
{
public:
    OverlayDialog(QWidget *parent, int overlayAlpha) : QDialog(parent ? parent->window() : nullptr, Qt::FramelessWindowHint | Qt::Dialog),
        m_alpha(overlayAlpha)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setModal(true);
        if (QWidget *w = parentWidget())
            setGeometry(QRect(w->mapToGlobal(QPoint(0, 0)), w->size()));
        card = new QWidget(this);
        card->setObjectName(QStringLiteral("card"));
        QVBoxLayout *outer = new QVBoxLayout(this);
        outer->addStretch(1);
        QHBoxLayout *row = new QHBoxLayout;
        row->addStretch(1);
        row->addWidget(card);
        row->addStretch(1);
        outer->addLayout(row);
        outer->addStretch(1);
    }
    QWidget *card;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(0, 0, 0, m_alpha));
    }
    void mousePressEvent(QMouseEvent *) override
    {
        if (!card->geometry().contains(mapFromGlobal(QCursor::pos())))
            reject();
    }

private:
    int m_alpha;
};

QString fontCss(int px)
{
    return QStringLiteral("font-family: %1; font-size: %2px;").arg(uiFamilies()).arg(px);
}
} // namespace

void setAutoConfirm(bool on) { g_autoConfirm = on; }
bool autoConfirm() { return g_autoConfirm; }

bool confirm(QWidget *parent, const QString &message, const QString &title, bool danger,
             const QString &confirmText, const QString &cancelText)
{
    if (g_autoConfirm)
        return true;
    OverlayDialog dlg(parent, 128);   // rgba(0,0,0,0.5)
    QWidget *card = dlg.card;
    card->setMinimumWidth(sz(320));
    card->setMaximumWidth(sz(440));
    QVBoxLayout *v = new QVBoxLayout(card);
    v->setContentsMargins(sz(32), sz(24), sz(32), sz(24));
    v->setSpacing(0);
    QLabel *icon = new QLabel(card);
    icon->setAlignment(Qt::AlignCenter);
    icon->setPixmap(lucidePixmap(danger ? LucideIcon::TriangleAlert : LucideIcon::Info, sz(32),
                                 themeColor(danger ? "error" : "accent-primary"), card->devicePixelRatioF()));
    v->addWidget(icon);
    v->addSpacing(8);
    QLabel *t = new QLabel(title, card);
    t->setObjectName(QStringLiteral("title"));
    t->setAlignment(Qt::AlignCenter);
    v->addWidget(t);
    v->addSpacing(8);
    QLabel *m = new QLabel(message, card);
    m->setObjectName(QStringLiteral("msg"));
    m->setAlignment(Qt::AlignCenter);
    m->setWordWrap(true);
    v->addWidget(m);
    v->addSpacing(sz(20));
    QHBoxLayout *btns = new QHBoxLayout;
    btns->setSpacing(sz(12));
    btns->addStretch(1);
    QPushButton *cancel = new QPushButton(cancelText, card);
    cancel->setObjectName(QStringLiteral("cancel"));
    cancel->setToolTip(QStringLiteral("取消并关闭对话框"));
    QPushButton *ok = new QPushButton(confirmText, card);
    ok->setObjectName(danger ? QStringLiteral("danger") : QStringLiteral("ok"));
    for (QPushButton *b : { cancel, ok }) {
        b->setCursor(Qt::PointingHandCursor);
        btns->addWidget(b);
    }
    btns->addStretch(1);
    v->addLayout(btns);
    setThemedStyle(card, QStringLiteral(
        "#card { background: var(--bg-secondary); border: 1px solid %1; border-radius: 4px; }"
        "QLabel { background: transparent; %2 }"
        "#title { color: var(--text-primary); font-size: 14px; font-weight: 600; }"
        "#msg { color: var(--text-secondary); font-size: 12px; }"
        "QPushButton { padding: 6px 20px; border-radius: 3px; %3 border: 1px solid var(--border-primary); }"
        "#cancel { background: var(--button-bg); color: var(--text-secondary); }"
        "#cancel:hover { background: var(--button-hover); color: var(--text-primary); }"
        "#ok { background: var(--accent-primary); color: #ffffff; border-color: var(--accent-primary); }"
        "#danger { background: #d32f2f; border-color: #d32f2f; color: #ffffff; }"
        "#danger:hover { background: #b71c1c; border-color: #b71c1c; }")
        .arg(danger ? QStringLiteral("#4ddc3232") : QStringLiteral("var(--border-primary)"), fontCss(12), fontCss(12)));
    QObject::connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    QObject::connect(ok, &QPushButton::clicked, &dlg, &QDialog::accept);
    ok->setDefault(true);
    ok->setFocus();
    return dlg.exec() == QDialog::Accepted;
}

bool prompt(QWidget *parent, const QString &message, const QString &defaultValue, QString *out)
{
    if (g_autoConfirm) {
        *out = defaultValue;
        return true;
    }
    OverlayDialog dlg(parent, 115);   // rgba(0,0,0,0.45)
    QWidget *card = dlg.card;
    card->setMinimumWidth(sz(320));
    card->setMaximumWidth(sz(440));
    QVBoxLayout *v = new QVBoxLayout(card);
    v->setContentsMargins(sz(24), sz(20), sz(24), sz(20));
    v->setSpacing(0);
    QLabel *m = new QLabel(message, card);
    m->setWordWrap(true);
    v->addWidget(m);
    v->addSpacing(12);
    QLineEdit *edit = new QLineEdit(defaultValue, card);
    v->addWidget(edit);
    v->addSpacing(14);
    QHBoxLayout *btns = new QHBoxLayout;
    btns->setSpacing(8);
    btns->addStretch(1);
    QPushButton *cancel = new QPushButton(QStringLiteral("取消"), card);
    cancel->setObjectName(QStringLiteral("cancel"));
    QPushButton *ok = new QPushButton(QStringLiteral("确定"), card);
    ok->setObjectName(QStringLiteral("ok"));
    btns->addWidget(cancel);
    btns->addWidget(ok);
    v->addLayout(btns);
    setThemedStyle(card, QStringLiteral(
        "#card { background: var(--bg-primary); border: 1px solid var(--border-primary); border-radius: 6px; }"
        "QLabel { background: transparent; color: var(--text-primary); %1 }"
        "QLineEdit { padding: 6px 10px; %1 color: var(--text-primary); background: var(--input-bg);"
        " border: 1px solid var(--input-border); border-radius: 4px; }"
        "QLineEdit:focus { border-color: var(--accent-primary); }"
        "QPushButton { padding: 5px 16px; border-radius: 4px; %2 border: 1px solid transparent; }"
        "#cancel { background: transparent; color: var(--text-secondary); border-color: var(--border-secondary); }"
        "#cancel:hover { background: var(--bg-tertiary); }"
        "#ok { background: var(--accent-primary); color: #ffffff; }").arg(fontCss(12), fontCss(11)));
    QObject::connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    QObject::connect(ok, &QPushButton::clicked, &dlg, &QDialog::accept);
    QObject::connect(edit, &QLineEdit::returnPressed, &dlg, &QDialog::accept);
    edit->selectAll();
    edit->setFocus();
    if (dlg.exec() != QDialog::Accepted)
        return false;
    *out = edit->text();
    return true;
}

// ---------------------------------------------------------------
IconButton::IconButton(LucideIcon icon, int iconPx, const char *colorVar, const char *hoverVar, QWidget *parent)
    : QToolButton(parent), m_icon(icon), m_px(iconPx), m_color(colorVar), m_hover(hoverVar)
{
    setCursor(Qt::PointingHandCursor);
    setAutoRaise(true);
    setAttribute(Qt::WA_Hover);
    connect(Theme::instance(), &Theme::changed, this, [this]() { update(); });
}

void IconButton::setIconColors(const char *colorVar, const char *hoverVar)
{
    m_color = colorVar;
    m_hover = hoverVar;
    update();
}

void IconButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    QStyleOption opt;
    opt.initFrom(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);   // QSS 背景
    const bool hov = underMouse() && isEnabled();
    QColor c = themeColor(hov ? m_hover.constData() : m_color.constData());
    if (!isEnabled())
        c.setAlphaF(c.alphaF() * 0.4);
    const double ipx = sz(m_px);
    const QRectF box((width() - ipx) / 2.0, (height() - ipx) / 2.0, ipx, ipx);
    drawLucide(p, m_icon, box, c);
}

// ---------------------------------------------------------------
HelpTip::HelpTip(const QString &text, QWidget *parent) : QToolButton(parent), m_text(text)
{
    bindSize(this, SizeKind::Fixed, 16, 16);
    setCursor(Qt::PointingHandCursor);
    setToolTip(QStringLiteral("点击查看帮助"));
    connect(this, &QToolButton::clicked, this, [this]() {
        // 弹出说明框：max-width 280，bg-primary，10px 12px 内边距
        QLabel *pop = new QLabel(m_text, this, Qt::Popup);
        pop->setAttribute(Qt::WA_DeleteOnClose);
        pop->setWordWrap(true);
        pop->setMaximumWidth(280);
        setThemedStyle(pop, QStringLiteral("QLabel { background: var(--bg-primary); color: var(--text-primary);"
                                           " border: 1px solid var(--border-primary); border-radius: 4px;"
                                           " padding: 10px 12px; font-family: %1; font-size: 11px; }")
                                .arg(uiFamilies()));
        pop->adjustSize();
        const QPoint below = mapToGlobal(QPoint(width() / 2, height() + 6));
        pop->move(below.x() - pop->width() / 2, below.y());
        pop->show();
    });
    connect(Theme::instance(), &Theme::changed, this, [this]() { update(); });
}

void HelpTip::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor c = themeColor(m_hover ? "accent-primary" : "text-tertiary");
    if (m_hover)
        p.setBrush(QColor(0, 122, 204, 26));
    p.setPen(QPen(c, 1.0));
    p.drawEllipse(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5));
    drawLucide(p, LucideIcon::HelpCircle, QRectF(rect()).adjusted(1, 1, -1, -1), c);
}

void HelpTip::enterEvent(QEvent *e)
{
    m_hover = true;
    update();
    QToolButton::enterEvent(e);
}

void HelpTip::leaveEvent(QEvent *e)
{
    m_hover = false;
    update();
    QToolButton::leaveEvent(e);
}

// ---------------------------------------------------------------
namespace {
// 菜单项：图标 + 文字，用 QWidgetAction 才能分别控制普通项和危险项的悬停色
class MenuItem : public QWidget
{
public:
    MenuItem(LucideIcon icon, const QString &text, bool danger, int fontPx, QMargins pad, int iconPx,
             const char *hoverBg, QWidget *parent)
        : QWidget(parent), m_icon(icon), m_text(text), m_danger(danger), m_font(fontPx), m_pad(pad),
          m_iconPx(iconPx), m_hoverBg(hoverBg)
    {
        setAttribute(Qt::WA_Hover);
        setMouseTracking(true);
        QFont f = font();
        f.setPixelSize(fontPx);
        setFont(f);
        const QFontMetrics fm(f);
        setMinimumSize(pad.left() + iconPx + sz(6) + fm.horizontalAdvance(text) + pad.right(),
                       pad.top() + qMax(fm.height(), iconPx) + pad.bottom());
    }
    bool small = false;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const bool hov = underMouse();
        QColor fg = themeColor("text-primary");
        if (!small) {
            if (hov)
                p.fillRect(rect(), m_danger ? QColor(0xef, 0x44, 0x44) : themeColor(m_hoverBg));
            if (hov)
                fg = Qt::white;
        } else {
            if (hov)
                p.fillRect(rect(), themeColor(m_hoverBg));
            if (m_danger)
                fg = themeColor("error");
        }
        const QRect inner = rect().marginsRemoved(m_pad);
        drawLucide(p, m_icon, QRectF(inner.left(), inner.center().y() - m_iconPx / 2.0 + 0.5, m_iconPx, m_iconPx), fg);
        p.setPen(fg);
        p.drawText(inner.adjusted(m_iconPx + sz(6), 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, m_text);
    }
    void enterEvent(QEvent *) override { update(); }
    void leaveEvent(QEvent *) override { update(); }

private:
    LucideIcon m_icon;
    QString m_text;
    bool m_danger;
    int m_font;
    QMargins m_pad;
    int m_iconPx;
    const char *m_hoverBg;
};

QAction *addMenuItem(QMenu *menu, MenuItem *w, std::function<void()> fn)
{
    QWidgetAction *a = new QWidgetAction(menu);
    a->setDefaultWidget(w);
    QObject::connect(a, &QAction::triggered, menu, [fn]() { fn(); });
    menu->addAction(a);
    // QWidgetAction 的默认控件不会自己触发 triggered：点击时手动触发并关闭菜单
    w->installEventFilter(menu);
    w->setProperty("_action", QVariant::fromValue<QObject *>(a));
    return a;
}

class ClickForwarder : public QObject
{
public:
    using QObject::QObject;
    bool eventFilter(QObject *obj, QEvent *e) override
    {
        if (e->type() == QEvent::MouseButtonRelease) {
            if (QAction *a = qobject_cast<QAction *>(obj->property("_action").value<QObject *>())) {
                if (QMenu *m = qobject_cast<QMenu *>(parent()))
                    m->close();
                a->trigger();
                return true;
            }
        }
        return false;
    }
};
} // namespace

ContextMenu::ContextMenu(QWidget *parent) : QMenu(parent)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    setStyleSheet(Theme::scalePx(QStringLiteral("QMenu { background: #1e1e2e; border: 1px solid #3a3a5c; border-radius: 6px;"
                                 " padding: 4px 0px; min-width: 120px; font-family: %1; font-size: 13px; color: #cdd6f4; }")
                      .arg(uiFamilies())));
}

QAction *ContextMenu::addItem(LucideIcon icon, const QString &text, std::function<void()> fn, bool danger)
{
    MenuItem *w = new MenuItem(icon, text, danger, sz(13), QMargins(sz(16), sz(8), sz(16), sz(8)), sz(13), "accent-primary", this);
    w->setStyleSheet(QStringLiteral("background: transparent;"));
    QPalette pal = w->palette();
    pal.setColor(QPalette::WindowText, themeColor("text-primary"));
    w->setPalette(pal);
    QAction *a = addMenuItem(this, w, fn);
    w->removeEventFilter(this);
    w->installEventFilter(new ClickForwarder(this));
    return a;
}

SmallMenu::SmallMenu(QWidget *parent) : QMenu(parent)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    setStyleSheet(Theme::instance()->qss(QStringLiteral(
        "QMenu { background: var(--bg-primary); border: 1px solid var(--border-secondary); border-radius: 4px;"
        " padding: 2px 0px; min-width: 130px; font-family: %1; font-size: 10px; }")).arg(uiFamilies()));
}

QAction *SmallMenu::addItem(LucideIcon icon, const QString &text, std::function<void()> fn, bool danger)
{
    MenuItem *w = new MenuItem(icon, text, danger, px(0.714), QMargins(sz(10), sz(4), sz(10), sz(4)), sz(13), "button-hover", this);
    w->small = true;
    QAction *a = addMenuItem(this, w, fn);
    w->removeEventFilter(this);
    w->installEventFilter(new ClickForwarder(this));
    return a;
}

// ---------------------------------------------------------------
UndoToast::UndoToast(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("undoToast"));
    setAttribute(Qt::WA_StyledBackground);
    QHBoxLayout *h = new QHBoxLayout(this);
    bindMargins(h, 16, 8, 16, 8);
    bindSpacing(h, 10);
    m_icon = new QLabel(this);
    m_text = new QLabel(this);
    m_text->setTextFormat(Qt::RichText);
    m_btn = new QPushButton(QStringLiteral("撤销"), this);
    m_btn->setCursor(Qt::PointingHandCursor);
    h->addWidget(m_icon);
    h->addWidget(m_text);
    h->addWidget(m_btn);
    setThemedStyle(this, QStringLiteral(
        "#undoToast { background: var(--bg-secondary); border: 1px solid var(--border-primary); border-radius: 4px; }"
        "QLabel { background: transparent; color: var(--text-primary); font-family: %1; font-size: 12px; }"
        "QPushButton { padding: 4px 12px; font-family: %1; font-size: 11px; font-weight: 600; color: #ffffff;"
        " background: var(--accent-primary); border: none; border-radius: 3px; }").arg(uiFamilies()));
    connect(m_btn, &QPushButton::clicked, this, &UndoToast::undoClicked);
    auto setIc = [this]() {
        m_icon->setPixmap(lucidePixmap(LucideIcon::Trash2, sz(14), themeColor("text-primary"), devicePixelRatioF()));
        if (isVisible()) {
            adjustSize();
            place();
        }
    };
    connect(Theme::instance(), &Theme::changed, this, setIc);
    connect(UiScale::instance(), &UiScale::changed, this, setIc);
    setIc();
    parent->installEventFilter(this);
    hide();
}

void UndoToast::setState(const QString &label, int count)
{
    if (count <= 0) {
        hide();
        return;
    }
    QString t = QStringLiteral("已删除 <b style='color:%1'>%2</b>").arg(themeColor("error").name(), label.toHtmlEscaped());
    if (count > 1)
        t += QStringLiteral(" 等 %1 组").arg(count);
    m_text->setText(t);
    adjustSize();
    place();
    show();
    raise();
}

QString UndoToast::text() const
{
    return isVisible() ? m_text->text() : QString();
}

void UndoToast::place()
{
    if (QWidget *p = parentWidget())
        move((p->width() - width()) / 2, p->height() - height() - 16);
}

bool UndoToast::eventFilter(QObject *obj, QEvent *e)
{
    if (obj == parentWidget() && e->type() == QEvent::Resize)
        place();
    return QWidget::eventFilter(obj, e);
}

} // namespace ui
