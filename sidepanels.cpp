#include "sidepanels.h"
#include "apppaths.h"
#include "geocalc.h"
#include "lucide.h"
#include "maptools.h"
#include "theme.h"
#include "uiwidgets.h"

#include <QCheckBox>
#include <QCalendarWidget>
#include <QDateTime>
#include <QEvent>
#include <QLocale>
#include <QRegularExpression>
#include <QTextCharFormat>
#include <QToolButton>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <QStyle>

namespace {

const qint64 kBjMs = 8LL * 3600 * 1000;

// 北京时间文本；整分钟只显示到分，否则带秒
QString bjMinute(qint64 ms)
{
    return QDateTime::fromMSecsSinceEpoch(ms + kBjMs, Qt::UTC)
        .toString(ms % 60000 == 0 ? QStringLiteral("yyyy-MM-dd HH:mm") : QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

QDate bjDate(qint64 ms)
{
    return QDateTime::fromMSecsSinceEpoch(ms + kBjMs, Qt::UTC).date();
}

// 宽松解析北京时间（结果为数值上加了 8 小时的 UTC QDateTime）。支持：
//   2026-04-27 09:30[:15]、2026/4/27 9:30、2026.4.27、2026年4月27日 9:30、20260427 0930、202604270930、
//   只有日期（起始取 00:00:00，结束取 23:59:59）、只有时间 9:30（日期用 fallback）
bool parseLoose(const QString &input, bool isEnd, const QDate &fallback, QDateTime *out)
{
    QString s = input.trimmed();
    if (s.isEmpty())
        return false;
    s.replace(QLatin1Char('/'), QLatin1Char('-')).replace(QLatin1Char('.'), QLatin1Char('-'))
        .replace(QStringLiteral("年"), QStringLiteral("-")).replace(QStringLiteral("月"), QStringLiteral("-"))
        .replace(QStringLiteral("日"), QStringLiteral(" ")).replace(QStringLiteral("："), QStringLiteral(":"))
        .replace(QLatin1Char('T'), QLatin1Char(' '));
    s = s.simplified();

    QDate date;
    int h = -1, mi = 0, sec = 0;
    static const QRegularExpression full(QStringLiteral(
        "^(\\d{4})-(\\d{1,2})-(\\d{1,2})(?:\\s+(\\d{1,2})(?::(\\d{1,2}))?(?::(\\d{1,2}))?)?$"));
    static const QRegularExpression compact(QStringLiteral("^(\\d{4})(\\d{2})(\\d{2})(?:\\s*(\\d{2})(\\d{2})?(\\d{2})?)?$"));
    static const QRegularExpression timeOnly(QStringLiteral("^(\\d{1,2}):(\\d{1,2})(?::(\\d{1,2}))?$"));
    QRegularExpressionMatch m = full.match(s);
    if (!m.hasMatch())
        m = compact.match(s);
    if (m.hasMatch()) {
        date = QDate(m.captured(1).toInt(), m.captured(2).toInt(), m.captured(3).toInt());
        if (!m.captured(4).isEmpty()) {
            h = m.captured(4).toInt();
            mi = m.captured(5).toInt();
            sec = m.captured(6).toInt();
        }
    } else {
        m = timeOnly.match(s);
        if (!m.hasMatch() || !fallback.isValid())
            return false;
        date = fallback;
        h = m.captured(1).toInt();
        mi = m.captured(2).toInt();
        sec = m.captured(3).toInt();
    }
    if (!date.isValid())
        return false;
    const QTime time = h < 0 ? (isEnd ? QTime(23, 59, 59) : QTime(0, 0)) : QTime(h, mi, sec);
    if (!time.isValid())
        return false;
    *out = QDateTime(date, time, Qt::UTC);
    return true;
}

// 「2 天 3 小时 15 分」
QString durationText(qint64 ms)
{
    const qint64 totalMin = ms / 60000;
    const qint64 d = totalMin / 1440, h = totalMin % 1440 / 60, mi = totalMin % 60;
    QStringList parts;
    if (d)
        parts << QStringLiteral("%1 天").arg(d);
    if (h)
        parts << QStringLiteral("%1 小时").arg(h);
    if (mi || parts.isEmpty())
        parts << QStringLiteral("%1 分").arg(mi);
    return parts.join(QLatin1Char(' '));
}

// 「数据源 key」与 TrackSource 下标
const TrackSource kSources[3] = { TrackSource::Adsb, TrackSource::Radar, TrackSource::RadarRaw };

void clearLayout(QLayout *l)
{
    while (QLayoutItem *it = l->takeAt(0)) {
        if (QWidget *w = it->widget()) {
            w->hide();
            w->deleteLater();
        } else if (QLayout *sub = it->layout()) {
            clearLayout(sub);
        }
        delete it;
    }
}

// 带勾的小方框（FlagPanel / TimeFilterPanel 的 .flag-check-box）
class CheckBox : public QCheckBox
{
public:
    explicit CheckBox(QWidget *parent) : QCheckBox(parent)
    {
        ui::bindSize(this, ui::SizeKind::Fixed, 16, 16);
        setCursor(Qt::PointingHandCursor);
        connect(Theme::instance(), &Theme::changed, this, [this]() { update(); });
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(width() / 16.0, height() / 16.0);   // 按 16px 基准绘制，随字号缩放
        const QRectF box(1.5, 1.5, 13, 13);
        const bool on = isChecked();
        p.setPen(QPen(on ? themeColor("accent-primary") : themeColor("border-primary"), 1));
        p.setBrush(on ? themeColor("accent-primary") : themeColor("input-bg"));
        if (!isEnabled())
            p.setOpacity(0.5);
        p.drawRoundedRect(box, 2, 2);
        if (on) {
            QPen pen(Qt::white, 2, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin);
            p.setPen(pen);
            QPainterPath tick;
            tick.moveTo(4.6, 8.2);
            tick.lineTo(7.0, 10.6);
            tick.lineTo(11.4, 5.4);
            p.drawPath(tick);
        }
    }
    bool hitButton(const QPoint &) const override { return true; }
};

class Dot : public QWidget
{
public:
    Dot(const char *colorVar, QWidget *parent) : QWidget(parent), m_var(colorVar)
    {
        ui::bindSize(this, ui::SizeKind::Fixed, 8, 8);
        connect(Theme::instance(), &Theme::changed, this, [this]() { update(); });
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(themeColor(m_var));
        p.drawEllipse(rect());
    }

private:
    const char *m_var;
};

// 「● 时间筛选已激活」
QLabel *indicator(const QString &text, QWidget *parent)
{
    QLabel *l = new QLabel(parent);
    l->setObjectName(QStringLiteral("indicator"));
    auto apply = [l, text]() {
        const QColor c = themeColor("accent-primary");
        const QString img = QStringLiteral("<span style='color:%1'>●</span>").arg(c.name());
        l->setText(img + QStringLiteral("&nbsp;") + text.toHtmlEscaped());
    };
    apply();
    QObject::connect(Theme::instance(), &Theme::changed, l, apply);
    return l;
}

class EscFilter : public QObject
{
public:
    EscFilter(QObject *parent, std::function<void()> fn) : QObject(parent), m_fn(fn) {}
    bool eventFilter(QObject *, QEvent *e) override
    {
        if (e->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(e)->key() == Qt::Key_Escape) {
            m_fn();
            return true;
        }
        return false;
    }

private:
    std::function<void()> m_fn;
};

QString panelFont()
{
    return QStringLiteral("font-family: %1;").arg(ui::uiFamilies());
}

} // namespace

// ---------------------------------------------------------------
//  TrackFilterState
// ---------------------------------------------------------------
bool TrackFilterState::hasPointCountFilter() const
{
    for (const PointCount &p : pc)
        if (p.enabled)
            return true;
    return false;
}

bool TrackFilterState::accepts(const Track &t) const
{
    if (t.points.isEmpty())
        return false;
    if (timeActive) {
        if (t.maxTime() < timeMin || t.minTime() > timeMax)
            return false;
    }
    const PointCount &p = pc[int(t.source)];
    if (p.enabled) {
        if (p.min >= 0 && t.points.size() < p.min)
            return false;
        if (p.max >= 0 && t.points.size() > p.max)
            return false;
    }
    if (timeActive) {
        // 裁剪到窗口后没有点的航迹不显示
        auto it = std::lower_bound(t.points.begin(), t.points.end(), timeMin,
                                   [](const TrackPoint &a, qint64 v) { return a.t < v; });
        if (it == t.points.end() || it->t > timeMax)
            return false;
    }
    return true;
}

void TrackFilterState::save() const
{
    QSettings &s = app::settings();
    s.setValue(QStringLiteral("filter.active_min"), timeActive ? QString::number(timeMin) : QStringLiteral("null"));
    s.setValue(QStringLiteral("filter.active_max"), timeActive ? QString::number(timeMax) : QStringLiteral("null"));
    for (int i = 0; i < 3; ++i) {
        QJsonObject o;
        o.insert(QStringLiteral("enabled"), pc[i].enabled);
        o.insert(QStringLiteral("min"), pc[i].min >= 0 ? QJsonValue(pc[i].min) : QJsonValue());
        o.insert(QStringLiteral("max"), pc[i].max >= 0 ? QJsonValue(pc[i].max) : QJsonValue());
        s.setValue(QStringLiteral("filter.point_count.") + trackSourceKey(kSources[i]),
                   QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    }
}

void TrackFilterState::load()
{
    QSettings &s = app::settings();
    bool ok1 = false, ok2 = false;
    const qint64 mn = s.value(QStringLiteral("filter.active_min")).toString().toLongLong(&ok1);
    const qint64 mx = s.value(QStringLiteral("filter.active_max")).toString().toLongLong(&ok2);
    timeActive = ok1 && ok2 && mn < mx;
    timeMin = timeActive ? mn : 0;
    timeMax = timeActive ? mx : 0;
    for (int i = 0; i < 3; ++i) {
        const QJsonObject o = QJsonDocument::fromJson(
            s.value(QStringLiteral("filter.point_count.") + trackSourceKey(kSources[i])).toString().toUtf8()).object();
        pc[i].enabled = o.value(QStringLiteral("enabled")).toBool();
        pc[i].min = o.value(QStringLiteral("min")).isDouble() ? o.value(QStringLiteral("min")).toInt() : -1;
        pc[i].max = o.value(QStringLiteral("max")).isDouble() ? o.value(QStringLiteral("max")).toInt() : -1;
    }
}

// ---------------------------------------------------------------
//  FilterPanel
// ---------------------------------------------------------------
FilterPanel::FilterPanel(TrackFilterState *state, QWidget *parent) : QWidget(parent), m_state(state)
{
    setObjectName(QStringLiteral("filterPanel"));
    QVBoxLayout *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(6);

    m_timeIndicator = indicator(QStringLiteral("时间筛选已激活"), this);
    v->addWidget(m_timeIndicator);
    m_range = new QLabel(this);
    m_range->setObjectName(QStringLiteral("rangeInfo"));
    m_range->setAlignment(Qt::AlignCenter);
    m_range->setWordWrap(true);
    v->addWidget(m_range);

    // 快捷范围：一键填入并立即应用
    QHBoxLayout *pr = new QHBoxLayout;
    pr->setSpacing(4);
    static const char *const presetNames[3] = { "全部数据", "最早 1 小时", "最后 1 小时" };
    for (int i = 0; i < 3; ++i) {
        QPushButton *b = new QPushButton(QString::fromUtf8(presetNames[i]), this);
        b->setObjectName(QStringLiteral("presetBtn"));
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(QStringLiteral("填入该时间段并立即应用过滤"));
        b->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        pr->addWidget(b, 1);
        m_presets << b;
        connect(b, &QPushButton::clicked, this, [this, i]() {
            const qint64 hour = 3600000;
            if (i == 0)
                fillRange(m_dataMin, m_dataMax);
            else if (i == 1)
                fillRange(m_dataMin, qMin(m_dataMax, m_dataMin + hour));
            else
                fillRange(qMax(m_dataMin, m_dataMax - hour), m_dataMax);
            clickApply();
        });
    }
    v->addLayout(pr);

    // 起止输入：可直接键入（宽松格式），或点右侧日历按钮在下方内嵌日历里选日期
    for (int which = 0; which < 2; ++which) {
        QHBoxLayout *row = new QHBoxLayout;
        row->setSpacing(4);
        QLabel *l = new QLabel(which == 0 ? QStringLiteral("起始") : QStringLiteral("结束"), this);
        l->setObjectName(QStringLiteral("timeSep"));
        QLineEdit *e = new QLineEdit(this);
        e->setObjectName(QStringLiteral("timeInput"));
        e->setMinimumWidth(0);
        e->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        e->setClearButtonEnabled(true);
        e->setToolTip(QStringLiteral("北京时间。支持 2026-04-27 09:30、2026/4/27 9:30、20260427 0930；"
                                     "只填日期时起始取 00:00、结束取 23:59；只填时间时日期取另一框或数据起始日"));
        ui::IconButton *cal = new ui::IconButton(LucideIcon::Calendar, 14, "text-tertiary", "accent-primary", this);
        ui::bindSize(cal, ui::SizeKind::Fixed, 24, 24);
        cal->setCursor(Qt::PointingHandCursor);
        cal->setToolTip(QStringLiteral("从日历选择日期"));
        row->addWidget(l);
        row->addWidget(e, 1);
        row->addWidget(cal);
        v->addLayout(row);
        (which == 0 ? m_start : m_end) = e;
        connect(cal, &QToolButton::clicked, this, [this, which]() { openCalendar(which); });
        connect(e, &QLineEdit::textChanged, this, [this]() { syncUi(); });
        connect(e, &QLineEdit::returnPressed, this, [this]() { clickApply(); });
    }

    // 内嵌日历（不用弹出窗口）：数据覆盖的日期加粗，已选区间底色高亮
    m_calBox = new QWidget(this);
    m_calBox->setObjectName(QStringLiteral("calBox"));
    m_calBox->setAttribute(Qt::WA_StyledBackground);
    QVBoxLayout *cv = new QVBoxLayout(m_calBox);
    ui::bindMargins(cv, 6, 4, 6, 6);
    ui::bindSpacing(cv, 4);
    QHBoxLayout *ch = new QHBoxLayout;
    m_calTitle = new QLabel(m_calBox);
    m_calTitle->setObjectName(QStringLiteral("calTitle"));
    ui::IconButton *calClose = new ui::IconButton(LucideIcon::X, 13, "text-tertiary", "text-primary", m_calBox);
    ui::bindSize(calClose, ui::SizeKind::Fixed, 20, 20);
    calClose->setToolTip(QStringLiteral("收起日历"));
    ch->addWidget(m_calTitle, 1);
    ch->addWidget(calClose);
    cv->addLayout(ch);
    m_cal = new QCalendarWidget(m_calBox);
    m_cal->setLocale(QLocale(QLocale::Chinese, QLocale::China));
    m_cal->setFirstDayOfWeek(Qt::Monday);
    m_cal->setGridVisible(false);
    m_cal->setVerticalHeaderFormat(QCalendarWidget::NoVerticalHeader);
    m_cal->setHorizontalHeaderFormat(QCalendarWidget::SingleLetterDayNames);
    m_cal->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    if (QWidget *nav = m_cal->findChild<QWidget *>(QStringLiteral("qt_calendar_navigationbar")))
        nav->setAttribute(Qt::WA_StyledBackground);   // 默认用调色板高亮色铺底，换成 QSS 背景
    cv->addWidget(m_cal);
    v->addWidget(m_calBox);
    m_calBox->hide();
    connect(calClose, &QToolButton::clicked, this, &FilterPanel::closeCalendar);
    connect(m_cal, &QCalendarWidget::clicked, this, &FilterPanel::pickDate);
    connect(m_cal, &QCalendarWidget::activated, this, &FilterPanel::pickDate);
    m_cal->installEventFilter(new EscFilter(m_cal, [this]() { closeCalendar(); }));
    // 翻月按钮换成 lucide 箭头，随主题和字号刷新
    auto navIcons = [this]() {
        const char *names[2] = { "qt_calendar_prevmonth", "qt_calendar_nextmonth" };
        const LucideIcon icons[2] = { LucideIcon::ChevronLeft, LucideIcon::ChevronRight };
        for (int i = 0; i < 2; ++i)
            if (QToolButton *b = m_cal->findChild<QToolButton *>(QLatin1String(names[i]))) {
                const int px = ui::sz(14);
                b->setIcon(QIcon(lucidePixmap(icons[i], px, themeColor("text-primary"), devicePixelRatioF())));
                b->setIconSize(QSize(px, px));
            }
        updateCalendarMarks();
    };
    navIcons();
    connect(Theme::instance(), &Theme::changed, m_cal, navIcons);
    connect(UiScale::instance(), &UiScale::changed, m_cal, navIcons);

    m_duration = new QLabel(this);
    m_duration->setObjectName(QStringLiteral("timeHint"));
    m_duration->setAlignment(Qt::AlignCenter);
    m_duration->setWordWrap(true);
    v->addWidget(m_duration);

    QHBoxLayout *br = new QHBoxLayout;
    br->setSpacing(4);
    m_apply = new QPushButton(QStringLiteral("应用过滤"), this);
    m_apply->setObjectName(QStringLiteral("applyBtn"));
    m_apply->setToolTip(QStringLiteral("应用时间范围过滤条件"));
    m_clear = new QPushButton(QStringLiteral("清除"), this);
    m_clear->setObjectName(QStringLiteral("clearBtn"));
    m_clear->setToolTip(QStringLiteral("清除时间过滤条件"));
    for (QPushButton *b : { m_apply, m_clear })
        b->setCursor(Qt::PointingHandCursor);
    br->addWidget(m_apply, 1);
    br->addWidget(m_clear);
    v->addLayout(br);
    m_error = new QLabel(this);
    m_error->setObjectName(QStringLiteral("errorMsg"));
    m_error->setAlignment(Qt::AlignCenter);
    m_error->hide();
    v->addWidget(m_error);
    connect(m_apply, &QPushButton::clicked, this, &FilterPanel::clickApply);
    connect(m_clear, &QPushButton::clicked, this, &FilterPanel::clickClear);

    QFrame *div = new QFrame(this);
    div->setObjectName(QStringLiteral("divider"));
    div->setFixedHeight(1);
    v->addWidget(div);

    m_pcIndicator = indicator(QStringLiteral("点数筛选已激活"), this);
    v->addWidget(m_pcIndicator);
    QHBoxLayout *lh = new QHBoxLayout;
    lh->setSpacing(4);
    QLabel *sec = new QLabel(QStringLiteral("航迹点长度筛选"), this);
    sec->setObjectName(QStringLiteral("sectionLabel"));
    lh->addWidget(sec);
    lh->addStretch(1);
    v->addLayout(lh);

    static const char *const labels[3] = { "ADS-B", "Radar", "Raw" };
    static const char *const colors[3] = { "source-adsb", "source-radar", "source-radar_raw" };
    for (int i = 0; i < 3; ++i) {
        QHBoxLayout *row = new QHBoxLayout;
        row->setSpacing(4);
        row->addWidget(new Dot(colors[i], this));
        m_pcCheck[i] = new CheckBox(this);
        m_pcCheck[i]->setToolTip(QStringLiteral("启用此数据源的点数筛选"));
        row->addWidget(m_pcCheck[i]);
        QLabel *l = new QLabel(QLatin1String(labels[i]), this);
        l->setObjectName(QStringLiteral("pfLabel"));
        ui::bindSize(l, ui::SizeKind::MinWidth, 52);
        row->addWidget(l);
        m_pcMin[i] = new QLineEdit(this);
        m_pcMax[i] = new QLineEdit(this);
        m_pcMin[i]->setPlaceholderText(QStringLiteral("最小"));
        m_pcMax[i]->setPlaceholderText(QStringLiteral("最大"));
        m_pcMin[i]->setToolTip(QStringLiteral("最小航迹点数阈值"));
        m_pcMax[i]->setToolTip(QStringLiteral("最大航迹点数阈值"));
        QLabel *dash = new QLabel(QStringLiteral("-"), this);
        dash->setObjectName(QStringLiteral("pfSep"));
        for (QLineEdit *e : { m_pcMin[i], m_pcMax[i] }) {
            e->setObjectName(QStringLiteral("pfInput"));
            ui::bindSize(e, ui::SizeKind::FixedWidth, 50);
            e->setValidator(new QIntValidator(0, 100000000, e));
        }
        row->addWidget(m_pcMin[i]);
        row->addWidget(dash);
        row->addWidget(m_pcMax[i]);
        row->addStretch(1);
        v->addLayout(row);
        connect(m_pcCheck[i], &QCheckBox::toggled, this, [this, i](bool on) {
            m_state->pc[i].enabled = on;
            m_state->save();
            syncUi();
            emit changed();
        });
        auto onEdit = [this, i](QLineEdit *e, bool isMin) {
            connect(e, &QLineEdit::editingFinished, this, [this, i, e, isMin]() {
                const int val = e->text().isEmpty() ? -1 : e->text().toInt();
                int &field = isMin ? m_state->pc[i].min : m_state->pc[i].max;
                if (field == val)
                    return;
                field = val;
                m_state->save();
                emit changed();
            });
        };
        onEdit(m_pcMin[i], true);
        onEdit(m_pcMax[i], false);
    }
    v->addStretch(1);

    // TimeFilterPanel.vue 样式
    setThemedStyle(this, QStringLiteral(
        "QLabel { background: transparent; %1 }"
        "#indicator { font-size: 9px; color: var(--accent-primary); }"
        "#rangeInfo { font-size: 10px; color: var(--text-tertiary); }"
        "#timeInput { padding: 4px 6px; background: var(--input-bg); border: 1px solid var(--input-border); border-radius: 2px;"
        " color: var(--input-fg); font-family: %2; font-size: 11px; }"
        "#timeInput:focus { border-color: var(--accent-primary); }"
        "#timeInput[invalid=\"true\"] { border-color: var(--error); }"
        "#timeSep { color: var(--text-tertiary); font-size: 11px; }"
        "#timeHint { color: var(--text-tertiary); font-size: 10px; }"
        "#timeHint[invalid=\"true\"] { color: var(--error); }"
        "#presetBtn { padding: 3px 4px; background: var(--button-bg); color: var(--button-fg); border: 1px solid var(--border-primary);"
        " border-radius: 2px; %1 font-size: 10px; }"
        "#presetBtn:hover { background: var(--button-hover); border-color: var(--accent-primary); }"
        "#presetBtn:disabled { color: var(--text-tertiary); }"
        "#calBox { background: var(--bg-primary); border: 1px solid var(--accent-primary); border-radius: 4px; }"
        "#calTitle { color: var(--accent-primary); font-size: 11px; font-weight: 600; }"
        "QCalendarWidget QWidget#qt_calendar_navigationbar { background: var(--bg-secondary); border-radius: 2px; }"
        "QCalendarWidget QToolButton { background: transparent; color: var(--text-primary); border: none; border-radius: 2px;"
        " padding: 2px 6px; %1 font-size: 12px; font-weight: 600; }"
        "QCalendarWidget QToolButton:hover { background: var(--button-hover); }"
        "QCalendarWidget QToolButton::menu-indicator { image: none; width: 0px; }"
        "QCalendarWidget QSpinBox { background: var(--input-bg); color: var(--input-fg); border: 1px solid var(--input-border);"
        " %1 font-size: 12px; padding: 1px 2px; }"
        "QCalendarWidget QMenu { background: var(--bg-primary); color: var(--text-primary); border: 1px solid var(--border-primary);"
        " %1 font-size: 11px; }"
        "QCalendarWidget QMenu::item:selected { background: var(--accent-primary); color: #ffffff; }"
        "QCalendarWidget QAbstractItemView { background: var(--bg-primary); color: var(--text-primary); outline: none;"
        " selection-background-color: var(--accent-primary); selection-color: #ffffff; %1 font-size: 11px; }"
        "QCalendarWidget QAbstractItemView:disabled { color: var(--text-tertiary); }"
        "#applyBtn { padding: 4px 8px; background: var(--accent-primary); color: #ffffff; border: none; border-radius: 2px;"
        " %1 font-size: 11px; font-weight: 600; }"
        "#applyBtn:disabled { background: #66007acc; color: #66ffffff; }"
        "#clearBtn { padding: 4px 8px; background: var(--button-bg); color: var(--button-fg); border: 1px solid var(--border-primary);"
        " border-radius: 2px; %1 font-size: 11px; }"
        "#clearBtn:hover { background: var(--button-hover); }"
        "#errorMsg { color: var(--error); font-size: 11px; }"
        "#divider { background: var(--border-primary); border: none; }"
        "#sectionLabel { font-size: 11px; color: var(--text-tertiary); font-weight: 600; }"
        "#pfLabel { color: var(--text-secondary); font-size: 11px; }"
        "#pfInput { padding: 2px 4px; background: var(--input-bg); border: 1px solid var(--input-border); border-radius: 2px;"
        " color: var(--input-fg); %1 font-size: 11px; }"
        "#pfInput:focus { border-color: var(--accent-primary); }"
        "#pfInput:disabled { color: #4dffffff; border-color: #4d555555; background: #4d3c3c3c; }"
        "#pfSep { color: var(--text-tertiary); font-size: 11px; }")
        .arg(panelFont(), ui::monoFamilies()));

    // 从筛选状态恢复输入框（切换面板后不显示空白）
    for (int i = 0; i < 3; ++i) {
        m_pcCheck[i]->blockSignals(true);
        m_pcCheck[i]->setChecked(m_state->pc[i].enabled);
        m_pcCheck[i]->blockSignals(false);
        m_pcMin[i]->setText(m_state->pc[i].min >= 0 ? QString::number(m_state->pc[i].min) : QString());
        m_pcMax[i]->setText(m_state->pc[i].max >= 0 ? QString::number(m_state->pc[i].max) : QString());
    }
    setDataRange(0, 0);
    if (m_state->timeActive)
        setInputs(m_state->timeMin, m_state->timeMax);
    syncUi();
}

void FilterPanel::setDataRange(qint64 minMs, qint64 maxMs)
{
    m_dataMin = minMs;
    m_dataMax = maxMs;
    const bool has = maxMs > minMs;
    m_range->setVisible(has);
    if (has)
        m_range->setText(QStringLiteral("数据范围: %1 — %2").arg(bjMinute(minMs), bjMinute(maxMs)));
    for (QPushButton *b : m_presets)
        b->setEnabled(has);
    // 日历只能选有数据的日期段
    if (has)
        m_cal->setDateRange(bjDate(minMs), bjDate(maxMs));
    else
        m_cal->setDateRange(QDate(2000, 1, 1), QDate(2100, 12, 31));
    updateCalendarMarks();
    syncUi();
}

bool FilterPanel::parseInput(int which, qint64 *ms) const
{
    const QLineEdit *self = which == 0 ? m_start : m_end;
    const QLineEdit *other = which == 0 ? m_end : m_start;
    // 只填时间时，日期取另一个输入框的日期，否则取数据起始日
    QDate fallback;
    QDateTime od;
    if (parseLoose(other->text(), which != 0, QDate(), &od))
        fallback = od.date();
    else if (m_dataMax > m_dataMin)
        fallback = bjDate(which == 0 ? m_dataMin : m_dataMax);
    QDateTime dt;
    if (!parseLoose(self->text(), which == 1, fallback, &dt))
        return false;
    if (ms)
        *ms = dt.toMSecsSinceEpoch() - kBjMs;
    return true;
}

void FilterPanel::fillRange(qint64 startMs, qint64 endMs)
{
    if (endMs <= startMs)
        return;
    m_start->setText(bjMinute(startMs));
    m_end->setText(bjMinute((endMs + 59999) / 60000 * 60000));   // 结束向上取整到分钟，不丢最后几秒
    m_error->hide();
    updateCalendarMarks();
}

void FilterPanel::openCalendar(int which)
{
    if (m_calBox->isVisible() && m_calTarget == which) {
        closeCalendar();
        return;
    }
    m_calTarget = which;
    m_calTitle->setText(which == 0 ? QStringLiteral("选择起始日期") : QStringLiteral("选择结束日期"));
    qint64 ms = 0;
    QDate d;
    if (parseInput(which, &ms) || parseInput(1 - which, &ms))
        d = bjDate(ms);
    else if (m_dataMax > m_dataMin)
        d = bjDate(which == 0 ? m_dataMin : m_dataMax);
    else
        d = QDate::currentDate();
    m_cal->setSelectedDate(d);
    m_cal->setCurrentPage(d.year(), d.month());
    updateCalendarMarks();
    m_calBox->show();
    m_cal->setFocus();
}

void FilterPanel::closeCalendar()
{
    m_calBox->hide();
    m_calTarget = -1;
}

void FilterPanel::pickDate(const QDate &d)
{
    if (m_calTarget < 0 || !d.isValid())
        return;
    const int which = m_calTarget;
    QLineEdit *self = which == 0 ? m_start : m_end;
    QLineEdit *other = which == 0 ? m_end : m_start;
    // 保留已填的时分；没填时起始取 00:00、结束取 23:59
    qint64 ms = 0;
    QTime t = which == 0 ? QTime(0, 0) : QTime(23, 59);
    if (parseInput(which, &ms))
        t = QDateTime::fromMSecsSinceEpoch(ms + kBjMs, Qt::UTC).time();
    const QString fmt = QStringLiteral("yyyy-MM-dd HH:mm");
    self->setText(QDateTime(d, t, Qt::UTC).toString(fmt));
    // 另一个框还空着时一并填上同一天，选完一天就能直接应用
    if (other->text().trimmed().isEmpty())
        other->setText(QDateTime(d, which == 0 ? QTime(23, 59) : QTime(0, 0), Qt::UTC).toString(fmt));
    m_error->hide();
    if (which == 0) {
        // 选完起始接着选结束
        openCalendar(1);
    } else {
        closeCalendar();
    }
    updateCalendarMarks();
}

void FilterPanel::updateCalendarMarks()
{
    if (!m_cal)
        return;
    m_cal->setDateTextFormat(QDate(), QTextCharFormat());   // 清空全部
    QTextCharFormat wk;
    wk.setForeground(themeColor("text-primary"));
    for (int d = Qt::Monday; d <= Qt::Sunday; ++d)
        m_cal->setWeekdayTextFormat(Qt::DayOfWeek(d), wk);
    QTextCharFormat hdr;
    hdr.setForeground(themeColor("text-tertiary"));
    hdr.setBackground(themeColor("bg-primary"));
    m_cal->setHeaderTextFormat(hdr);
    // 有数据的日期加粗、强调色
    if (m_dataMax > m_dataMin) {
        QTextCharFormat f;
        f.setFontWeight(QFont::Bold);
        f.setForeground(themeColor("accent-primary"));
        const QDate last = bjDate(m_dataMax);
        int guard = 0;
        for (QDate d = bjDate(m_dataMin); d <= last && guard < 400; d = d.addDays(1), ++guard)
            m_cal->setDateTextFormat(d, f);
    }
    // 已填区间铺底色
    qint64 a = 0, b = 0;
    if (parseInput(0, &a) && parseInput(1, &b) && a < b) {
        QColor bg = themeColor("accent-primary");
        bg.setAlpha(60);
        const QDate last = bjDate(b);
        int guard = 0;
        for (QDate d = bjDate(a); d <= last && guard < 400; d = d.addDays(1), ++guard) {
            QTextCharFormat f = m_cal->dateTextFormat(d);
            f.setBackground(bg);
            m_cal->setDateTextFormat(d, f);
        }
    }
}

void FilterPanel::setInputs(qint64 startMs, qint64 endMs)
{
    m_start->setText(bjMinute(startMs));
    m_end->setText(bjMinute(endMs));
    syncUi();
}

void FilterPanel::setInputTexts(const QString &start, const QString &end)
{
    m_start->setText(start);
    m_end->setText(end);
}

void FilterPanel::clickApply()
{
    m_error->hide();
    auto fail = [this](const QString &msg) {
        m_error->setText(msg);
        m_error->show();
    };
    if (m_start->text().trimmed().isEmpty() || m_end->text().trimmed().isEmpty())
        return fail(QStringLiteral("请设置起始和结束时间"));
    qint64 start = 0, end = 0;
    if (!parseInput(0, &start))
        return fail(QStringLiteral("起始时间格式无法识别，示例：2026-04-27 09:30"));
    if (!parseInput(1, &end))
        return fail(QStringLiteral("结束时间格式无法识别，示例：2026-04-27 18:00"));
    if (start >= end)
        return fail(QStringLiteral("起始时间必须早于结束时间"));
    if (m_dataMax > m_dataMin && (end < m_dataMin || start > m_dataMax))
        return fail(QStringLiteral("该时间段内没有数据（数据范围 %1 — %2）").arg(bjMinute(m_dataMin), bjMinute(m_dataMax)));
    // 把宽松输入规范成统一格式，让用户看到实际生效的时间
    m_start->setText(bjMinute(start));
    m_end->setText(bjMinute(end));
    closeCalendar();
    m_state->timeActive = true;
    m_state->timeMin = start;
    m_state->timeMax = end;
    m_state->save();
    syncUi();
    emit changed();
}

void FilterPanel::clickClear()
{
    m_state->timeActive = false;
    m_state->timeMin = m_state->timeMax = 0;
    m_state->save();
    m_start->clear();
    m_end->clear();
    closeCalendar();
    m_error->hide();
    syncUi();
    emit changed();
}

QString FilterPanel::errorText() const
{
    return m_error->isVisible() || !m_error->isHidden() ? m_error->text() : QString();
}

QString FilterPanel::rangeText() const
{
    return m_range->isHidden() ? QString() : m_range->text();
}

void FilterPanel::setPointCount(TrackSource s, bool enabled, int min, int max)
{
    const int i = int(s);
    m_pcMin[i]->setText(min >= 0 ? QString::number(min) : QString());
    m_pcMax[i]->setText(max >= 0 ? QString::number(max) : QString());
    m_state->pc[i].min = min;
    m_state->pc[i].max = max;
    if (m_pcCheck[i]->isChecked() == enabled) {
        m_state->save();
        emit changed();
    } else {
        m_pcCheck[i]->setChecked(enabled);   // toggled 里保存并发 changed
    }
}

void FilterPanel::syncUi()
{
    m_timeIndicator->setVisible(m_state->timeActive);
    m_clear->setVisible(m_state->timeActive || !m_start->text().isEmpty() || !m_end->text().isEmpty());
    // 输入框逐个校验：格式错误时红框，下方提示改为示例格式
    qint64 a = 0, b = 0;
    const bool okA = parseInput(0, &a), okB = parseInput(1, &b);
    const bool badA = !m_start->text().trimmed().isEmpty() && !okA;
    const bool badB = !m_end->text().trimmed().isEmpty() && !okB;
    auto setInvalid = [](QWidget *w, bool bad) {
        if (w->property("invalid").toBool() == bad)
            return;
        w->setProperty("invalid", bad);
        w->style()->unpolish(w);
        w->style()->polish(w);
    };
    setInvalid(m_start, badA);
    setInvalid(m_end, badB);
    bool hintBad = false;
    if (badA || badB) {
        m_duration->setText(QStringLiteral("格式无法识别，示例：2026-04-27 09:30 或 2026/4/27 9:30"));
        hintBad = true;
    } else if (okA && okB && a < b) {
        m_duration->setText(QStringLiteral("时长 %1").arg(durationText(b - a)));
    } else if (okA && okB) {
        m_duration->setText(QStringLiteral("起始时间必须早于结束时间"));
        hintBad = true;
    } else {
        m_duration->setText(QStringLiteral("可直接输入、点日历选日期，或用上方快捷范围"));
    }
    setInvalid(m_duration, hintBad);
    m_apply->setEnabled(okA && okB && a < b);
    if (m_calBox->isVisible())
        updateCalendarMarks();
    m_pcIndicator->setVisible(m_state->hasPointCountFilter());
    for (int i = 0; i < 3; ++i) {
        m_pcMin[i]->setEnabled(m_pcCheck[i]->isChecked());
        m_pcMax[i]->setEnabled(m_pcCheck[i]->isChecked());
    }
}

// ---------------------------------------------------------------
//  FlagPanel
// ---------------------------------------------------------------
FlagPanel::FlagPanel(FlagStore *flags, RulerState *ruler, QWidget *parent)
    : QWidget(parent), m_flags(flags), m_ruler(ruler)
{
    setObjectName(QStringLiteral("flagPanel"));
    QVBoxLayout *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(6);

    QHBoxLayout *ir = new QHBoxLayout;
    ir->setSpacing(4);
    m_lat = new QLineEdit(this);
    m_lon = new QLineEdit(this);
    m_lat->setPlaceholderText(QStringLiteral("纬度 (-90~90)"));
    m_lon->setPlaceholderText(QStringLiteral("经度 (-180~180)"));
    for (QLineEdit *e : { m_lat, m_lon }) {
        e->setObjectName(QStringLiteral("coordInput"));
        e->setMinimumWidth(0);
        e->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        ir->addWidget(e, 1);
    }
    QPushButton *place = new QPushButton(QStringLiteral("放置旗标"), this);
    place->setObjectName(QStringLiteral("placeBtn"));
    place->setToolTip(QStringLiteral("在地图上放置旗标"));
    place->setCursor(Qt::PointingHandCursor);
    ir->addWidget(place);
    v->addLayout(ir);
    connect(place, &QPushButton::clicked, this, [this]() { placeFlag(m_lat->text(), m_lon->text()); });
    for (QLineEdit *e : { m_lat, m_lon })
        connect(e, &QLineEdit::returnPressed, place, &QPushButton::click);

    m_error = new QLabel(this);
    m_error->setObjectName(QStringLiteral("coordError"));
    m_error->setAlignment(Qt::AlignCenter);
    m_error->hide();
    v->addWidget(m_error);
    m_geo = new QLabel(this);
    m_geo->setObjectName(QStringLiteral("geoResult"));
    m_geo->hide();
    v->addWidget(m_geo);
    m_empty = new QLabel(QStringLiteral("暂无旗标，双击地图放置"), this);
    m_empty->setObjectName(QStringLiteral("emptyText"));
    m_empty->setAlignment(Qt::AlignCenter);
    v->addWidget(m_empty);

    QScrollArea *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("panelScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QWidget *inner = new QWidget(scroll);
    inner->setObjectName(QStringLiteral("scrollInner"));
    QVBoxLayout *sv = new QVBoxLayout(inner);
    sv->setContentsMargins(0, 0, 0, 0);
    sv->setSpacing(6);
    m_flagList = new QWidget(inner);
    m_flagList->setLayout(new QVBoxLayout);
    m_flagList->layout()->setContentsMargins(0, 0, 0, 0);
    m_flagList->layout()->setSpacing(2);
    sv->addWidget(m_flagList);

    // 航线标尺
    QWidget *rs = new QWidget(inner);
    rs->setObjectName(QStringLiteral("rulerSection"));
    QVBoxLayout *rv = new QVBoxLayout(rs);
    rv->setContentsMargins(0, 8, 0, 0);
    rv->setSpacing(4);
    QHBoxLayout *rh = new QHBoxLayout;
    rh->setSpacing(4);
    QLabel *rt = new QLabel(QStringLiteral("航线标尺"), rs);
    rt->setObjectName(QStringLiteral("rulerTitle"));
    rh->addWidget(rt);
    rh->addStretch(1);
    m_rulerToggle = new QPushButton(rs);
    m_rulerToggle->setObjectName(QStringLiteral("rulerToggle"));
    m_rulerToggle->setToolTip(QStringLiteral("启用地图航线测距标尺"));
    m_rulerToggle->setCursor(Qt::PointingHandCursor);
    rh->addWidget(m_rulerToggle);
    rv->addLayout(rh);
    m_rulerBody = new QWidget(rs);
    m_rulerBody->setLayout(new QVBoxLayout);
    m_rulerBody->layout()->setContentsMargins(0, 0, 0, 0);
    m_rulerBody->layout()->setSpacing(2);
    rv->addWidget(m_rulerBody);
    sv->addWidget(rs);
    sv->addStretch(1);
    scroll->setWidget(inner);
    v->addWidget(scroll, 1);
    connect(m_rulerToggle, &QPushButton::clicked, m_ruler, &RulerState::toggle);

    setThemedStyle(this, QStringLiteral(
        "QLabel { background: transparent; %1 }"
        "#panelScroll, #scrollInner { background: transparent; }"
        "#coordInput { padding: 4px 6px; background: var(--input-bg); border: 1px solid var(--input-border); border-radius: 2px;"
        " color: var(--input-fg); %1 font-size: 11px; }"
        "#coordInput:focus { border-color: var(--accent-primary); }"
        "#placeBtn { padding: 4px 10px; background: var(--accent-primary); color: #ffffff; border: none; border-radius: 2px;"
        " %1 font-size: 11px; font-weight: 600; }"
        "#coordError { color: var(--error); font-size: 11px; }"
        "#geoResult { padding: 6px 8px; background: var(--bg-tertiary); border: 1px solid var(--border-primary); border-radius: 2px;"
        " font-size: 11px; color: var(--accent-primary); }"
        "#emptyText { color: var(--text-tertiary); font-size: 11px; padding: 8px 0px; }"
        "#clearAllBtn { padding: 5px 10px; background: var(--error-bg); color: var(--error); border: 1px solid var(--error);"
        " border-radius: 2px; %1 font-size: 12px; }"
        "#clearAllBtn:hover { background: #40f44747; }"
        "#flagRow, #rulerRow { border-bottom: 1px solid var(--border-secondary); }"
        "#flagLabel { font-size: 11px; color: var(--text-primary); font-weight: 500; }"
        "#flagLabel:hover { color: var(--accent-primary); }"
        "#renameInput { padding: 2px 4px; background: var(--input-bg); border: 1px solid var(--accent-primary); border-radius: 2px;"
        " color: var(--input-fg); %1 font-size: 11px; }"
        "#flagCoords { font-size: 10px; color: var(--text-tertiary); font-family: %2; }"
        "#rulerSection { border-top: 1px solid var(--border-primary); }"
        "#rulerTitle { font-size: 12px; font-weight: 600; color: var(--text-primary); }"
        "#rulerToggle { padding: 3px 10px; border: 1px solid var(--border-primary); border-radius: 3px; background: var(--button-bg);"
        " color: var(--text-secondary); %1 font-size: 10px; }"
        "#rulerToggle:hover { background: var(--button-hover); color: var(--text-primary); }"
        "#rulerToggle[active=\"true\"] { background: #f59e0b; border-color: #f59e0b; color: #000000; font-weight: 600; }"
        "#rulerHint { font-size: 10px; color: var(--text-tertiary); }"
        "#rulerIndex { background: #f59e0b; color: #000000; font-size: 9px; font-weight: 700; border-radius: 9px; }"
        "#rulerCoords { font-size: 10px; color: var(--text-secondary); font-family: %2; }"
        "#rulerSeg, #rulerDirect { background: var(--bg-tertiary); border-radius: 2px; }"
        "#segLabel { font-size: 10px; color: var(--text-tertiary); }"
        "#segDist { font-size: 10px; color: var(--accent-primary); font-weight: 600; }"
        "#segBearing { font-size: 10px; color: var(--text-secondary); }"
        "#rulerTotal { background: #1ff59e0b; border: 1px solid #4df59e0b; border-radius: 3px; }"
        "#totalLabel { font-size: 10px; color: var(--text-secondary); }"
        "#totalVal { font-size: 12px; font-weight: 700; color: #f59e0b; }"
        "#rulerAction, #rulerDanger { padding: 4px 0px; border: 1px solid var(--border-primary); border-radius: 2px;"
        " background: var(--button-bg); color: var(--text-secondary); %1 font-size: 10px; }"
        "#rulerAction:hover:!disabled { background: var(--button-hover); color: var(--text-primary); }"
        "#rulerDanger { color: var(--error); border-color: var(--error); }"
        "#rulerDanger:hover:!disabled { background: var(--error-bg); }"
        "#rulerAction:disabled, #rulerDanger:disabled { color: #66808080; border-color: #66808080; }")
        .arg(panelFont(), ui::monoFamilies()));

    connect(m_flags, &FlagStore::changed, this, &FlagPanel::rebuildFlags);
    connect(m_ruler, &RulerState::changed, this, &FlagPanel::rebuildRuler);
    rebuildFlags();
    rebuildRuler();
}

void FlagPanel::placeFlag(const QString &latText, const QString &lonText)
{
    m_error->hide();
    auto fail = [this](const QString &msg) {
        m_error->setText(msg);
        m_error->show();
    };
    if (latText.trimmed().isEmpty() || lonText.trimmed().isEmpty())
        return fail(QStringLiteral("请输入经纬度"));
    bool ok1 = false, ok2 = false;
    const double lat = latText.trimmed().toDouble(&ok1), lon = lonText.trimmed().toDouble(&ok2);
    if (!ok1 || !ok2 || !std::isfinite(lat) || !std::isfinite(lon))
        return fail(QStringLiteral("请输入有效数字"));
    if (lat < -90 || lat > 90)
        return fail(QStringLiteral("纬度范围 -90 ~ 90"));
    if (lon < -180 || lon > 180)
        return fail(QStringLiteral("经度范围 -180 ~ 180"));
    m_flags->addFlag(lat, lon);
    m_lat->clear();
    m_lon->clear();
}

QString FlagPanel::coordError() const { return m_error->isHidden() ? QString() : m_error->text(); }
QString FlagPanel::geoText() const { return m_geo->isHidden() ? QString() : m_geo->text(); }
QString FlagPanel::rulerTotalText() const { return m_total ? m_total->text() : QString(); }
QStringList FlagPanel::segmentTexts() const { return m_segTexts; }

void FlagPanel::rebuildFlags()
{
    QVBoxLayout *l = static_cast<QVBoxLayout *>(m_flagList->layout());
    clearLayout(l);
    const QVector<MapFlag> &flags = m_flags->flags();
    m_empty->setVisible(flags.isEmpty() && !m_ruler->isActive());
    m_flagList->setVisible(!flags.isEmpty());
    MapFlag a, b;
    if (m_flags->selectedPair(&a, &b)) {
        const double dist = geocalc::vincentyKm(a.lat, a.lon, b.lat, b.lon);
        const double brg = geocalc::initialBearing(a.lat, a.lon, b.lat, b.lon);
        m_geo->setText(QStringLiteral("距离: %1 km\n方位角: %2° (%3)").arg(dist, 0, 'f', 1).arg(brg, 0, 'f', 1)
                           .arg(geocalc::bearingToCardinal(brg)));
        m_geo->show();
    } else {
        m_geo->hide();
    }
    if (flags.isEmpty())
        return;
    QPushButton *clearAll = new QPushButton(QStringLiteral("清除全部旗标"), m_flagList);
    clearAll->setObjectName(QStringLiteral("clearAllBtn"));
    clearAll->setToolTip(QStringLiteral("清除所有旗标，此操作不可撤销"));
    clearAll->setCursor(Qt::PointingHandCursor);
    clearAll->setIcon(lucideQIcon(LucideIcon::Trash2, 13, themeColor("error")));
    connect(clearAll, &QPushButton::clicked, this, [this]() {
        if (ui::confirm(this, QStringLiteral("确定要清除地图上所有旗标吗？此操作不可撤销。"), QStringLiteral("清除旗标")))
            m_flags->clearAll();
    });
    l->addWidget(clearAll);
    l->addSpacing(4);
    for (const MapFlag &f : flags) {
        QWidget *row = new QWidget(m_flagList);
        row->setObjectName(QStringLiteral("flagRow"));
        row->setAttribute(Qt::WA_StyledBackground);
        QHBoxLayout *h = new QHBoxLayout(row);
        h->setContentsMargins(4, 3, 4, 3);
        h->setSpacing(6);
        CheckBox *cb = new CheckBox(row);
        cb->setToolTip(QStringLiteral("选中后计算两点间距离和方位角"));
        cb->setChecked(m_flags->selectedIds().contains(f.id));
        const QString id = f.id;
        connect(cb, &QCheckBox::clicked, this, [this, id]() { m_flags->toggleSelect(id); });
        h->addWidget(cb);
        QVBoxLayout *info = new QVBoxLayout;
        info->setSpacing(1);
        if (m_editingId == f.id) {
            QLineEdit *edit = new QLineEdit(f.label, row);
            edit->setObjectName(QStringLiteral("renameInput"));
            auto commit = [this, edit, id]() {
                if (m_editingId != id)
                    return;
                m_editingId.clear();
                if (!edit->text().trimmed().isEmpty())
                    m_flags->renameFlag(id, edit->text());
                else
                    rebuildFlags();
            };
            connect(edit, &QLineEdit::editingFinished, this, commit);
            edit->installEventFilter(new EscFilter(edit, [this]() {
                m_editingId.clear();
                rebuildFlags();
            }));
            info->addWidget(edit);
            edit->setFocus();
            edit->selectAll();
        } else {
            QPushButton *lbl = new QPushButton(f.label, row);
            lbl->setObjectName(QStringLiteral("flagLabel"));
            lbl->setFlat(true);
            lbl->setToolTip(QStringLiteral("点击修改旗标名称"));
            lbl->setCursor(Qt::PointingHandCursor);
            lbl->setIcon(lucideQIcon(LucideIcon::Pencil, 11, themeColor("text-primary"), themeColor("accent-primary")));
            lbl->setIconSize(QSize(11, 11));
            lbl->setStyleSheet(QStringLiteral("text-align: left; border: none; background: transparent; padding: 0px;"));
            connect(lbl, &QPushButton::clicked, this, [this, id]() {
                m_editingId = id;
                rebuildFlags();
            });
            info->addWidget(lbl);
        }
        QLabel *coords = new QLabel(QStringLiteral("%1, %2").arg(f.lat, 0, 'f', 4).arg(f.lon, 0, 'f', 4), row);
        coords->setObjectName(QStringLiteral("flagCoords"));
        info->addWidget(coords);
        h->addLayout(info, 1);
        ui::IconButton *del = new ui::IconButton(LucideIcon::X, 13, "error", "error", row);
        del->setToolTip(QStringLiteral("删除此旗标"));
        ui::bindSize(del, ui::SizeKind::Fixed, 20, 20);
        connect(del, &QToolButton::clicked, this, [this, id]() { m_flags->removeFlag(id); });
        h->addWidget(del);
        l->addWidget(row);
    }
}

void FlagPanel::rebuildRuler()
{
    m_rulerToggle->setText(m_ruler->isActive() ? QStringLiteral("关闭标尺") : QStringLiteral("启用标尺"));
    m_rulerToggle->setProperty("active", m_ruler->isActive());
    m_rulerToggle->style()->unpolish(m_rulerToggle);
    m_rulerToggle->style()->polish(m_rulerToggle);
    m_empty->setVisible(m_flags->flags().isEmpty() && !m_ruler->isActive());
    QVBoxLayout *l = static_cast<QVBoxLayout *>(m_rulerBody->layout());
    clearLayout(l);
    m_total = nullptr;
    m_segTexts.clear();
    m_rulerBody->setVisible(m_ruler->isActive());
    if (!m_ruler->isActive())
        return;
    QLabel *hint = new QLabel(QStringLiteral("单击地图添加航点，Esc 退出"), m_rulerBody);
    hint->setObjectName(QStringLiteral("rulerHint"));
    hint->setAlignment(Qt::AlignCenter);
    l->addWidget(hint);
    const QVector<RulerState::Waypoint> &w = m_ruler->waypoints();
    if (w.isEmpty()) {
        QLabel *e = new QLabel(QStringLiteral("点击地图放置第一个航点"), m_rulerBody);
        e->setObjectName(QStringLiteral("emptyText"));
        e->setAlignment(Qt::AlignCenter);
        l->addWidget(e);
    }
    for (int i = 0; i < w.size(); ++i) {
        QWidget *row = new QWidget(m_rulerBody);
        row->setObjectName(QStringLiteral("rulerRow"));
        row->setAttribute(Qt::WA_StyledBackground);
        QHBoxLayout *h = new QHBoxLayout(row);
        h->setContentsMargins(4, 2, 4, 2);
        h->setSpacing(4);
        QLabel *idx = new QLabel(QString::number(i + 1), row);
        idx->setObjectName(QStringLiteral("rulerIndex"));
        ui::bindSize(idx, ui::SizeKind::Fixed, 18, 18);
        idx->setAlignment(Qt::AlignCenter);
        h->addWidget(idx);
        QLabel *c = new QLabel(QStringLiteral("%1, %2").arg(w[i].lat, 0, 'f', 4).arg(w[i].lon, 0, 'f', 4), row);
        c->setObjectName(QStringLiteral("rulerCoords"));
        h->addWidget(c, 1);
        ui::IconButton *del = new ui::IconButton(LucideIcon::X, 12, "error", "error", row);
        del->setToolTip(QStringLiteral("删除此航点"));
        ui::bindSize(del, ui::SizeKind::Fixed, 18, 18);
        const int id = w[i].id;
        connect(del, &QToolButton::clicked, this, [this, id]() { m_ruler->removeWaypoint(id); });
        h->addWidget(del);
        l->addWidget(row);
    }
    const QVector<RulerSegment> segs = m_ruler->segments();
    if (!segs.isEmpty())
        l->addSpacing(4);
    for (const RulerSegment &s : segs) {
        QWidget *row = new QWidget(m_rulerBody);
        row->setObjectName(QStringLiteral("rulerSeg"));
        row->setAttribute(Qt::WA_StyledBackground);
        QHBoxLayout *h = new QHBoxLayout(row);
        h->setContentsMargins(4, 2, 4, 2);
        h->setSpacing(6);
        const QString label = QStringLiteral("%1 → %2").arg(s.index + 1).arg(s.index + 2);
        const QString dist = formatRulerDistance(s.distanceKm);
        const QString brg = QStringLiteral("%1° %2").arg(s.bearingDeg, 0, 'f', 0).arg(s.cardinal);
        QLabel *a = new QLabel(label, row);
        a->setObjectName(QStringLiteral("segLabel"));
        QLabel *b = new QLabel(dist, row);
        b->setObjectName(QStringLiteral("segDist"));
        QLabel *c = new QLabel(brg, row);
        c->setObjectName(QStringLiteral("segBearing"));
        h->addWidget(a);
        h->addWidget(b, 1);
        h->addWidget(c);
        l->addWidget(row);
        m_segTexts << label + QLatin1Char(' ') + dist + QLatin1Char(' ') + brg;
    }
    auto summary = [this, l](const QString &name, const QString &label, const QString &val) {
        QWidget *row = new QWidget(m_rulerBody);
        row->setObjectName(name);
        row->setAttribute(Qt::WA_StyledBackground);
        QHBoxLayout *h = new QHBoxLayout(row);
        h->setContentsMargins(6, 3, 6, 3);
        QLabel *a = new QLabel(label, row);
        a->setObjectName(QStringLiteral("totalLabel"));
        QLabel *b = new QLabel(val, row);
        b->setObjectName(QStringLiteral("totalVal"));
        h->addWidget(a);
        h->addStretch(1);
        h->addWidget(b);
        l->addWidget(row);
        return b;
    };
    if (!segs.isEmpty()) {
        l->addSpacing(4);
        m_total = summary(QStringLiteral("rulerTotal"), QStringLiteral("总距离"), formatRulerDistance(m_ruler->totalKm()));
    }
    double deg = 0.0;
    QString card;
    if (m_ruler->directBearing(&deg, &card))
        summary(QStringLiteral("rulerDirect"), QStringLiteral("首尾方位"), QStringLiteral("%1° %2").arg(deg, 0, 'f', 0).arg(card));
    l->addSpacing(4);
    QHBoxLayout *act = new QHBoxLayout;
    act->setSpacing(6);
    QPushButton *undo = new QPushButton(QStringLiteral("↩ 撤销"), m_rulerBody);
    undo->setObjectName(QStringLiteral("rulerAction"));
    undo->setToolTip(QStringLiteral("撤销上一个航点"));
    QPushButton *clr = new QPushButton(QStringLiteral("清空"), m_rulerBody);
    clr->setObjectName(QStringLiteral("rulerDanger"));
    clr->setToolTip(QStringLiteral("清空所有航点"));
    for (QPushButton *b : { undo, clr }) {
        b->setEnabled(!w.isEmpty());
        b->setCursor(Qt::PointingHandCursor);
        act->addWidget(b, 1);
    }
    connect(undo, &QPushButton::clicked, m_ruler, &RulerState::undo);
    connect(clr, &QPushButton::clicked, m_ruler, &RulerState::clearAll);
    l->addLayout(act);
}
