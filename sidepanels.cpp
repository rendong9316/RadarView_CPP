#include "sidepanels.h"
#include "apppaths.h"
#include "geocalc.h"
#include "lucide.h"
#include "maptools.h"
#include "theme.h"
#include "uiwidgets.h"

#include <QCheckBox>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QEvent>
#include <QHBoxLayout>
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

QString bjMinute(qint64 ms)
{
    return QDateTime::fromMSecsSinceEpoch(ms + kBjMs, Qt::UTC).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
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
        setFixedSize(16, 16);
        setCursor(Qt::PointingHandCursor);
        connect(Theme::instance(), &Theme::changed, this, [this]() { update(); });
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
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
        setFixedSize(8, 8);
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

    QHBoxLayout *ir = new QHBoxLayout;
    ir->setSpacing(4);
    m_start = new QDateTimeEdit(this);
    m_end = new QDateTimeEdit(this);
    for (QDateTimeEdit *e : { m_start, m_end }) {
        e->setObjectName(QStringLiteral("timeInput"));
        e->setTimeSpec(Qt::UTC);                 // 显示值 = 北京时间（数值上加了 8 小时的 UTC）
        e->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm"));
        e->setCalendarPopup(true);
        e->setSpecialValueText(QStringLiteral("年 /月/日 --:--"));
        e->setMinimumWidth(0);
        e->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }
    QLabel *sep = new QLabel(QStringLiteral("至"), this);
    sep->setObjectName(QStringLiteral("timeSep"));
    ir->addWidget(m_start, 1);
    ir->addWidget(sep);
    ir->addWidget(m_end, 1);
    v->addLayout(ir);
    connect(m_start, &QDateTimeEdit::dateTimeChanged, this, [this]() {
        m_startSet = m_start->dateTime() != m_start->minimumDateTime();
        syncUi();
    });
    connect(m_end, &QDateTimeEdit::dateTimeChanged, this, [this]() {
        m_endSet = m_end->dateTime() != m_end->minimumDateTime();
        syncUi();
    });

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
    lh->addWidget(new ui::HelpTip(QStringLiteral("按航迹点数过滤各数据源。勾选数据源后设置最小和最大点数阈值，仅显示点数在范围内的航迹。"
                                                 "可用于过滤掉采样点过少的低质量航迹。"), this));
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
        l->setMinimumWidth(52);
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
            e->setFixedWidth(50);
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
        "#timeInput::drop-down { border: none; width: 14px; }"
        "#timeSep { color: var(--text-tertiary); font-size: 11px; }"
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
    m_range->setVisible(maxMs > minMs);
    if (maxMs > minMs)
        m_range->setText(QStringLiteral("数据范围: %1 — %2").arg(bjMinute(minMs), bjMinute(maxMs)));
    // 可选范围为数据范围前后各 1 小时；最小值本身作为「未设置」
    const QDateTime lo = maxMs > minMs ? QDateTime::fromMSecsSinceEpoch(minMs - 3600000 + kBjMs, Qt::UTC)
                                       : QDateTime(QDate(2000, 1, 1), QTime(0, 0), Qt::UTC);
    const QDateTime hi = maxMs > minMs ? QDateTime::fromMSecsSinceEpoch(maxMs + 3600000 + kBjMs, Qt::UTC)
                                       : QDateTime(QDate(2100, 1, 1), QTime(0, 0), Qt::UTC);
    for (QDateTimeEdit *e : { m_start, m_end }) {
        const bool set = e == m_start ? m_startSet : m_endSet;
        const QDateTime keep = e->dateTime();
        e->blockSignals(true);
        e->setDateTimeRange(lo.addSecs(-60), hi);
        e->setDateTime(set ? keep : e->minimumDateTime());
        e->blockSignals(false);
    }
}

void FilterPanel::setInputs(qint64 startMs, qint64 endMs)
{
    auto toEdit = [](qint64 ms) {
        const qint64 m = (ms + kBjMs) / 60000 * 60000;   // 分钟精度
        return QDateTime::fromMSecsSinceEpoch(m, Qt::UTC);
    };
    for (QDateTimeEdit *e : { m_start, m_end }) {
        const QDateTime v = toEdit(e == m_start ? startMs : endMs);
        if (v <= e->minimumDateTime())
            e->setMinimumDateTime(v.addSecs(-60));
        if (v > e->maximumDateTime())
            e->setMaximumDateTime(v);
        e->setDateTime(v);
    }
    m_startSet = m_endSet = true;
    syncUi();
}

void FilterPanel::clickApply()
{
    m_error->hide();
    if (!m_startSet || !m_endSet) {
        m_error->setText(QStringLiteral("请设置起始和结束时间"));
        m_error->show();
        return;
    }
    const qint64 start = m_start->dateTime().toMSecsSinceEpoch() - kBjMs;
    const qint64 end = m_end->dateTime().toMSecsSinceEpoch() - kBjMs;
    if (start >= end) {
        m_error->setText(QStringLiteral("起始时间必须早于结束时间"));
        m_error->show();
        return;
    }
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
    for (QDateTimeEdit *e : { m_start, m_end }) {
        e->blockSignals(true);
        e->setDateTime(e->minimumDateTime());
        e->blockSignals(false);
    }
    m_startSet = m_endSet = false;
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
    m_clear->setVisible(m_state->timeActive);
    m_apply->setEnabled(m_startSet && m_endSet);
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
    ir->addWidget(new ui::HelpTip(QStringLiteral("旗标是地图上的标记点。双击地图可放置旗标，或在此输入经纬度手动放置。"
                                                 "选中两个旗标可计算两点间的距离（Vincenty 公式）和方位角。"), this),
                  0, Qt::AlignVCenter);
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
    rh->addWidget(new ui::HelpTip(QStringLiteral("航线规划测距工具。启用后单击地图依次放置航点，自动计算每段距离和方位角，"
                                                 "显示总距离和首尾方位。Esc 键退出标尺模式。"), rs));
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
        del->setFixedSize(20, 20);
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
        idx->setFixedSize(18, 18);
        idx->setAlignment(Qt::AlignCenter);
        h->addWidget(idx);
        QLabel *c = new QLabel(QStringLiteral("%1, %2").arg(w[i].lat, 0, 'f', 4).arg(w[i].lon, 0, 'f', 4), row);
        c->setObjectName(QStringLiteral("rulerCoords"));
        h->addWidget(c, 1);
        ui::IconButton *del = new ui::IconButton(LucideIcon::X, 12, "error", "error", row);
        del->setToolTip(QStringLiteral("删除此航点"));
        del->setFixedSize(18, 18);
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
