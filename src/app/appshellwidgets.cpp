#include "appshellwidgets.h"

#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QScrollArea>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include "lucide.h"
#include "theme.h"
#include "uiwidgets.h"

namespace {

struct PanelDef {
    LucideIcon icon;
    const char *tooltip;
    const char *title;
};

const PanelDef kPanels[] = {
    { LucideIcon::ChartColumn, "航迹管理系统 (Ctrl+Shift+M)", "航迹管理系统" },
    { LucideIcon::Flag, "旗标面板 (Ctrl+Shift+F)", "旗标面板" },
    { LucideIcon::Funnel, "时间筛选 (Ctrl+Shift+E)", "筛选" },
    { LucideIcon::Settings, "设置", "设置" },
};

// 活动栏按钮：48×48，24px 图标；悬停 / 选中为 --activitybar-active，选中时左侧 2px 竖条
class ActivityButton : public QAbstractButton
{
public:
    ActivityButton(LucideIcon icon, QWidget *parent) : QAbstractButton(parent), m_icon(icon)
    {
        ui::bindSize(this, ui::SizeKind::Fixed, 48, 48);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setFocusPolicy(Qt::NoFocus);
    }
    bool active = false;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        if (active)
            p.fillRect(QRect(0, 0, ui::sz(2), height()), themeColor("activitybar-active-border"));
        const QColor c = themeColor(active || underMouse() ? "activitybar-active" : "activitybar-fg");
        const double ic = ui::sz(24);
        drawLucide(p, m_icon, QRectF((width() - ic) / 2.0, (height() - ic) / 2.0, ic, ic), c);
    }
    void enterEvent(QEvent *) override { update(); }
    void leaveEvent(QEvent *) override { update(); }

private:
    LucideIcon m_icon;
};

// 每个面板放进滚动区：字号调大后内容比侧栏高时改为滚动，控件不会被压缩重叠
QScrollArea *wrapScroll(QWidget *w)
{
    QScrollArea *sa = new QScrollArea;
    sa->setObjectName(QStringLiteral("panelScroll"));
    sa->setFrameShape(QFrame::NoFrame);
    sa->setWidgetResizable(true);
    sa->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sa->setWidget(w);
    sa->viewport()->setAutoFillBackground(false);
    w->setAutoFillBackground(false);
    return sa;
}

} // namespace

namespace appshell {

QString panelTitle(PanelId id)
{
    const int idx = int(id);
    if (idx < 0 || idx >= int(PanelId::Count))
        return QString();
    return QString::fromUtf8(kPanels[idx].title);
}

} // namespace appshell

// ---------------------------------------------------------------
//  活动栏
// ---------------------------------------------------------------
ActivityBar::ActivityBar(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("activityBar"));
    ui::bindSize(this, ui::SizeKind::FixedWidth, 48);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    QVBoxLayout *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    for (int i = 0; i < int(PanelId::Count); ++i) {
        ActivityButton *b = new ActivityButton(kPanels[i].icon, this);
        b->setToolTip(QString::fromUtf8(kPanels[i].tooltip));
        b->setAccessibleName(QString::fromUtf8(kPanels[i].tooltip));
        if (i == int(PanelId::Settings)) {
            b->setVisible(false);         // 设置入口改到标题栏菜单，侧栏不再显示；保留索引以维持 activatePanel(Settings)
            lay->addStretch(1);           // 设置固定在底部
        }
        lay->addWidget(b);
        m_buttons << b;
        connect(b, &QAbstractButton::clicked, this, [this, i]() { emit iconClicked(i); });
    }
    connect(Theme::instance(), &Theme::changed, this, [this]() {
        update();
        for (QAbstractButton *b : qAsConst(m_buttons))
            b->update();
    });
}

void ActivityBar::highlight(int index)
{
    m_active = index;
    for (int i = 0; i < m_buttons.size(); ++i) {
        static_cast<ActivityButton *>(m_buttons[i])->active = i == index;
        m_buttons[i]->update();
    }
}

void ActivityBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), themeColor("activitybar-bg"));
    p.fillRect(QRect(width() - 1, 0, 1, height()), themeColor("border-secondary"));
}

// ---------------------------------------------------------------
//  侧栏
// ---------------------------------------------------------------
SidePanel::SidePanel(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("sidePanel"));
    setAttribute(Qt::WA_StyledBackground);
    ui::bindSize(this, ui::SizeKind::MinWidth, appshell::kSidebarMin);
    ui::bindSize(this, ui::SizeKind::MaxWidth, appshell::kSidebarMax);
    QVBoxLayout *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    QWidget *header = new QWidget(this);
    header->setObjectName(QStringLiteral("sidebarHeader"));
    header->setAttribute(Qt::WA_StyledBackground);
    ui::bindSize(header, ui::SizeKind::FixedHeight, 32);
    QHBoxLayout *hh = new QHBoxLayout(header);
    ui::bindMargins(hh, 12, 0, 12, 0);
    m_title = new QLabel(header);
    m_title->setObjectName(QStringLiteral("sidebarTitle"));
    hh->addWidget(m_title);
    hh->addStretch(1);
    ui::IconButton *close = new ui::IconButton(LucideIcon::X, 14, "text-tertiary", "text-primary", header);
    close->setObjectName(QStringLiteral("sidebarClose"));
    ui::bindSize(close, ui::SizeKind::Fixed, 20, 20);
    close->setToolTip(QStringLiteral("关闭侧边栏"));
    hh->addWidget(close);
    lay->addWidget(header);
    m_stack = new QStackedWidget(this);
    m_stack->setObjectName(QStringLiteral("sidebarBody"));
    m_stack->setContentsMargins(8, 8, 8, 8);
    lay->addWidget(m_stack, 1);
    connect(close, &QToolButton::clicked, this, &SidePanel::closeRequested);
    setThemedStyle(this, QStringLiteral(
        "#sidePanel { background: var(--sidebar-bg); border-right: 1px solid var(--sidebar-border); }"
        "#sidebarHeader { background: var(--sidebar-bg); border-bottom: 1px solid var(--sidebar-border); }"
        "#sidebarTitle { color: var(--sidebar-header); font-family: %1; font-size: 11px; font-weight: 600; background: transparent; }"
        "#sidebarClose { border: none; border-radius: 3px; background: transparent; }"
        "#sidebarClose:hover { background: var(--button-hover); }"
        "#sidebarBody { background: var(--sidebar-bg); }"
        "#panelScroll, #panelScroll > QWidget > QWidget { background: transparent; border: none; }"
        "#panelScroll QScrollBar:vertical { background: var(--scrollbar-bg); width: 10px; margin: 0px; }"
        "#panelScroll QScrollBar::handle:vertical { background: var(--scrollbar-thumb); min-height: 20px; border-radius: 3px; }"
        "#sidebarBody QToolTip { font-size: 11px; }").arg(ui::uiFamilies()));
}

void SidePanel::addPanel(QWidget *w)
{
    m_stack->addWidget(wrapScroll(w));
}

void SidePanel::replacePanel(int index, QWidget *w)
{
    QScrollArea *sa = qobject_cast<QScrollArea *>(m_stack->widget(index));
    if (!sa)
        return;
    QWidget *old = sa->takeWidget();
    sa->setWidget(w);
    if (old)
        old->deleteLater();
}

void SidePanel::showPanel(int index, const QString &title)
{
    m_stack->setCurrentIndex(index);
    m_title->setText(title);
}

int SidePanel::currentPanel() const
{
    return m_stack->currentIndex();
}

QString SidePanel::title() const
{
    return m_title->text();
}
