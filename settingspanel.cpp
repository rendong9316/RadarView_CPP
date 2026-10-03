#include "settingspanel.h"
#include "apppaths.h"
#include "globewidget.h"
#include "lucide.h"
#include "managepanel.h"
#include "theme.h"
#include "trackdb.h"
#include "uiwidgets.h"

#include <QColorDialog>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QVBoxLayout>
#include <cmath>

namespace {

const TrackSource kSources[] = { TrackSource::Adsb, TrackSource::Radar, TrackSource::RadarRaw };

QString srcLabel(TrackSource s)   // SettingsPanel.vue sourceLabel
{
    switch (s) {
    case TrackSource::Adsb: return QStringLiteral("ADS-B");
    case TrackSource::Radar: return QStringLiteral("Radar");
    case TrackSource::RadarRaw: return QStringLiteral("Raw");
    }
    return QString();
}

// 组头点击折叠 / 展开（.group-header @click toggleSection）
class HeaderClick : public QObject
{
public:
    HeaderClick(QWidget *body, QLabel *chevron, QObject *parent) : QObject(parent), m_body(body), m_chev(chevron) {}
    bool eventFilter(QObject *, QEvent *e) override
    {
        if (e->type() == QEvent::MouseButtonRelease && static_cast<QMouseEvent *>(e)->button() == Qt::LeftButton) {
            m_body->setVisible(!m_body->isVisible());
            m_chev->setText(m_body->isVisible() ? QStringLiteral("▾") : QStringLiteral("▸"));
            return true;
        }
        return false;
    }

private:
    QWidget *m_body;
    QLabel *m_chev;
};

void setSwatch(QPushButton *b, const QColor &c)
{
    b->setStyleSheet(QStringLiteral("QPushButton { background: %1; border: 1px solid #555555; border-radius: 2px; }")
                         .arg(c.name()));
}

} // namespace

SettingsPanel::SettingsPanel(const Host &host, QWidget *parent) : QWidget(parent), m_host(host)
{
    setObjectName(QStringLiteral("settingsPanel"));
    QVBoxLayout *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("settingsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    QWidget *content = new QWidget(scroll);
    content->setObjectName(QStringLiteral("settingsContent"));
    QVBoxLayout *main = new QVBoxLayout(content);
    main->setContentsMargins(0, 0, 0, 0);
    main->setSpacing(0);
    scroll->setWidget(content);
    outer->addWidget(scroll);

    QSettings &s = app::settings();
    TrackLayer *layer = m_host.globe ? m_host.globe->trackLayer() : nullptr;

    // ---- 线条颜色（按数据源覆盖文件色）----
    QVBoxLayout *g = addGroup(main, int(LucideIcon::Palette), QStringLiteral("线条颜色"),
                              QStringLiteral("设置各数据源航迹线的颜色。点击色块选择颜色，点击右侧重置按钮恢复为默认颜色。"));
    for (TrackSource src : kSources) {
        const QString key = QStringLiteral("display.line_color.%1").arg(trackSourceKey(src));
        QWidget *row = new QWidget(g->parentWidget());
        row->setObjectName(QStringLiteral("settingRow"));
        QHBoxLayout *rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(8);
        QLabel *lab = new QLabel(srcLabel(src), row);
        lab->setObjectName(QStringLiteral("rowLabel"));
        lab->setStyleSheet(QStringLiteral("color: %1;").arg(trackSourceColor(src).name()));
        QPushButton *sw = new QPushButton(row);
        sw->setFixedSize(26, 18);
        sw->setCursor(Qt::PointingHandCursor);
        QLabel *hex = new QLabel(row);
        hex->setObjectName(QStringLiteral("rowValue"));
        QPushButton *reset = new QPushButton(row);
        reset->setObjectName(QStringLiteral("resetBtn"));
        reset->setFixedSize(20, 20);
        reset->setIcon(lucideQIcon(LucideIcon::RotateCcw, 12, QColor("#888888")));
        reset->setToolTip(QStringLiteral("重置 %1 为默认颜色").arg(srcLabel(src)));
        rl->addWidget(lab);
        rl->addWidget(sw);
        rl->addWidget(hex, 1);
        rl->addWidget(reset);
        g->addWidget(row);

        auto apply = [layer, sw, hex, src](const QColor &c, bool overrideOn) {
            setSwatch(sw, c);
            hex->setText(overrideOn ? c.name().toUpper() : QStringLiteral("默认"));
            if (layer) {
                if (overrideOn)
                    layer->setSourceColorOverride(src, c);
                else
                    layer->clearSourceColorOverride(src);
            }
        };
        const QColor saved(s.value(key).toString());
        apply(saved.isValid() ? saved : trackSourceColor(src), saved.isValid());
        connect(sw, &QPushButton::clicked, this, [this, key, apply, src]() {
            const QColor cur(app::settings().value(key, trackSourceColor(src).name()).toString());
            const QColor c = QColorDialog::getColor(cur, this, QStringLiteral("选择线条颜色"));
            if (!c.isValid())
                return;
            app::settings().setValue(key, c.name());
            apply(c, true);
            if (m_host.globe)
                m_host.globe->update();
        });
        connect(reset, &QPushButton::clicked, this, [this, key, apply, src]() {
            app::settings().remove(key);
            apply(trackSourceColor(src), false);
            if (m_host.globe)
                m_host.globe->update();
        });
    }

    // ---- 线宽调节（0.5–8，默认 2）----
    g = addGroup(main, int(LucideIcon::GripHorizontal), QStringLiteral("线宽调节"),
                 QStringLiteral("设置航迹线的像素宽度，范围 0.5-8 px。拖动滑块即时生效。"));
    addSlider(g, QStringLiteral("线宽"), themeColor("text-primary"), 0.5, 8.0, 0.5,
              s.value(QStringLiteral("display.line_width"), 2.0).toDouble(), QStringLiteral("px"),
              [this, layer](double v) {
                  app::settings().setValue(QStringLiteral("display.line_width"), v);
                  if (layer)
                      layer->setLineWidthPx(float(v));
                  if (m_host.globe)
                      m_host.globe->update();
              });

    // ---- 圆球直径（0.2–3.0，默认 1.0，按数据源）----
    g = addGroup(main, int(LucideIcon::CircleDot), QStringLiteral("圆球直径"),
                 QStringLiteral("设置各数据源终点圆球的缩放比例，范围 0.2-3.0 倍。"));
    for (TrackSource src : kSources) {
        const QString key = QStringLiteral("display.dot_scale.%1").arg(trackSourceKey(src));
        addSlider(g, srcLabel(src), trackSourceColor(src), 0.2, 3.0, 0.1, s.value(key, 1.0).toDouble(), QString(),
                  [this, layer, key, src](double v) {
                      app::settings().setValue(key, v);
                      if (layer)
                          layer->setDotScale(src, v);
                      if (m_host.globe)
                          m_host.globe->update();
                  });
    }

    // ---- 点迹圆球大小（0.2–5.0，基准 7px）----
    g = addGroup(main, int(LucideIcon::Dot), QStringLiteral("点迹显示"),
                 QStringLiteral("右键航迹「显示所有对应点迹」时每个采样点圆球的大小，范围 0.2-5.0 倍。"));
    addSlider(g, QStringLiteral("圆球大小"), themeColor("accent-primary"), 0.2, 5.0, 0.1,
              s.value(QStringLiteral("display.track_point_dot_scale"), 1.0).toDouble(), QString(),
              [this, layer](double v) {
                  app::settings().setValue(QStringLiteral("display.track_point_dot_scale"), v);
                  if (layer)
                      layer->setPointDotPx(float(7.0 * v));
                  if (m_host.globe)
                      m_host.globe->update();
              });

    // ---- 旗标大小（0.5–3.0，默认 1.2）----
    g = addGroup(main, int(LucideIcon::Flag), QStringLiteral("旗标大小"),
                 QStringLiteral("设置旗标图标和文字标签的整体缩放比例，范围 0.5-3.0 倍。"));
    addSlider(g, QStringLiteral("图标&文字"), themeColor("accent-primary"), 0.5, 3.0, 0.1,
              s.value(QStringLiteral("display.flag_scale"), 1.2).toDouble(), QString(),
              [this](double v) {
                  app::settings().setValue(QStringLiteral("display.flag_scale"), v);
                  if (m_host.globe)
                      m_host.globe->setFlagScale(v);
              });

    // ---- 字号大小（10–20 px，全局界面缩放）----
    g = addGroup(main, int(LucideIcon::Type), QStringLiteral("字号大小"),
                 QStringLiteral("设置应用界面文字的基础字号，范围 10-20 px。影响侧栏、菜单栏、状态栏、地图标签等所有文本。"));
    m_font = addSlider(g, QStringLiteral("应用字号"), themeColor("text-primary"), 10, 20, 1,
                       UiScale::instance()->basePx(), QStringLiteral("px"),
                       [](double v) {
                           UiScale::instance()->setBasePx(int(v));
                           app::settings().setValue(QStringLiteral("display.font_size"), int(v));
                       });

    // ---- 工具 ----
    g = addGroup(main, int(LucideIcon::Wrench), QStringLiteral("工具"),
                 QStringLiteral("常用功能的快捷入口。数据管理：打开航迹管理系统（含批量数据管理）。标签显示：切换航迹标签。"
                                "重置视角：恢复地图默认视角。清除显示：清空地图可见集合。"));
    QWidget *grid = new QWidget(g->parentWidget());
    QGridLayout *gl = new QGridLayout(grid);
    gl->setContentsMargins(0, 2, 0, 0);
    gl->setSpacing(4);
    auto tool = [this, grid, gl](LucideIcon ic, const QString &text, int r, int c, bool danger, void (SettingsPanel::*sig)()) {
        QPushButton *b = new QPushButton(text, grid);
        b->setObjectName(danger ? QStringLiteral("actionDanger") : QStringLiteral("actionBtn"));
        b->setCursor(Qt::PointingHandCursor);
        b->setIcon(lucideQIcon(ic, 13, danger ? themeColor("error") : themeColor("text-secondary")));
        gl->addWidget(b, r, c);
        connect(b, &QPushButton::clicked, this, sig);
    };
    tool(LucideIcon::Database, QStringLiteral("数据管理"), 0, 0, false, &SettingsPanel::openManageRequested);
    tool(LucideIcon::Eye, QStringLiteral("标签显示"), 0, 1, false, &SettingsPanel::toggleLabelsRequested);
    tool(LucideIcon::Maximize2, QStringLiteral("重置视角"), 1, 0, false, &SettingsPanel::resetViewRequested);
    tool(LucideIcon::Trash2, QStringLiteral("清除显示"), 1, 1, true, &SettingsPanel::clearDisplayRequested);
    m_batchLabel = new QLabel(grid);
    m_batchLabel->setObjectName(QStringLiteral("rowValue"));
    gl->addWidget(m_batchLabel, 2, 0, 1, 2);
    g->addWidget(grid);
    main->addStretch(1);

    setThemedStyle(this, QStringLiteral(
        "#settingsPanel, #settingsScroll, #settingsContent { background: transparent; }"
        "QLabel { background: transparent; }"
        "#groupHeader { color: var(--text-secondary); font-family: %1; font-size: 11px; font-weight: 600; }"
        "#groupHeader:hover { color: var(--text-primary); }"
        "#groupChevron, #groupIcon { color: var(--text-tertiary); font-size: 11px; }"
        "#settingsGroup { border-bottom: 1px solid var(--border-secondary); }"
        "#rowLabel { font-family: %1; font-size: 11px; min-width: 56px; }"
        "#rowValue { color: var(--text-tertiary); font-family: %2; font-size: 10px; }"
        "#resetBtn { border: 1px solid transparent; border-radius: 2px; background: transparent; }"
        "#resetBtn:hover { border-color: var(--accent-primary); }"
        "QSlider::groove:horizontal { height: 4px; background: var(--bg-tertiary); border-radius: 2px; }"
        "QSlider::handle:horizontal { width: 12px; height: 12px; margin: -4px 0px; border-radius: 6px;"
        " background: var(--accent-primary); }"
        "#actionBtn, #actionDanger { padding: 4px 10px; background: var(--button-bg); color: var(--button-fg);"
        " border: 1px solid transparent; border-radius: 2px; font-family: %1; font-size: 11px; }"
        "#actionBtn:hover { background: var(--button-hover); }"
        "#actionDanger { color: var(--error); background: transparent; }"
        "#actionDanger:hover { background: var(--error-bg); }")
        .arg(ui::uiFamilies(), ui::monoFamilies()));
    refresh();
}

QVBoxLayout *SettingsPanel::addGroup(QVBoxLayout *main, int icon, const QString &title, const QString &tip)
{
    QWidget *group = new QWidget(main->parentWidget());
    group->setObjectName(QStringLiteral("settingsGroup"));
    group->setAttribute(Qt::WA_StyledBackground);
    QVBoxLayout *gv = new QVBoxLayout(group);
    gv->setContentsMargins(0, 0, 0, 0);
    gv->setSpacing(0);

    QWidget *hdr = new QWidget(group);
    hdr->setCursor(Qt::PointingHandCursor);
    QHBoxLayout *hh = new QHBoxLayout(hdr);
    hh->setContentsMargins(12, 8, 12, 6);
    hh->setSpacing(6);
    QLabel *chev = new QLabel(QStringLiteral("▾"), hdr);
    chev->setObjectName(QStringLiteral("groupChevron"));
    QLabel *ic = new QLabel(hdr);
    ic->setPixmap(lucidePixmap(LucideIcon(icon), 13, themeColor("text-tertiary"), devicePixelRatioF()));
    QLabel *t = new QLabel(title, hdr);
    t->setObjectName(QStringLiteral("groupHeader"));
    hh->addWidget(chev);
    hh->addWidget(ic);
    hh->addWidget(t);
    hh->addWidget(new ui::HelpTip(tip, hdr));
    hh->addStretch(1);
    gv->addWidget(hdr);

    QWidget *body = new QWidget(group);
    QVBoxLayout *bv = new QVBoxLayout(body);
    bv->setContentsMargins(12, 2, 12, 8);
    bv->setSpacing(3);
    gv->addWidget(body);
    hdr->installEventFilter(new HeaderClick(body, chev, hdr));
    main->addWidget(group);
    return bv;
}

QSlider *SettingsPanel::addSlider(QVBoxLayout *body, const QString &label, const QColor &color, double minV,
                                  double maxV, double step, double value, const QString &unit,
                                  std::function<void(double)> onValue)
{
    QWidget *row = new QWidget(body->parentWidget());
    row->setMinimumHeight(26);
    QHBoxLayout *rl = new QHBoxLayout(row);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(8);
    QLabel *lab = new QLabel(label, row);
    lab->setObjectName(QStringLiteral("rowLabel"));
    lab->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
    QSlider *sl = new QSlider(Qt::Horizontal, row);
    sl->setRange(int(std::lround(minV / step)), int(std::lround(maxV / step)));
    sl->setValue(int(std::lround(qBound(minV, value, maxV) / step)));
    QLabel *val = new QLabel(row);
    val->setObjectName(QStringLiteral("rowValue"));
    val->setMinimumWidth(36);
    val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    const int dec = step < 1.0 ? 1 : 0;
    auto show = [val, dec, unit](double v) { val->setText(QString::number(v, 'f', dec) + unit); };
    show(sl->value() * step);
    rl->addWidget(lab);
    rl->addWidget(sl, 1);
    rl->addWidget(val);
    body->addWidget(row);
    connect(sl, &QSlider::valueChanged, this, [sl, step, show, onValue]() {
        const double v = sl->value() * step;
        show(v);
        onValue(v);
    });
    onValue(sl->value() * step);   // 启动时把保存的值应用到地图
    return sl;
}

void SettingsPanel::refresh()
{
    if (!m_batchLabel)
        return;
    const ManageStats st = trackdb::isOpen() ? trackdb::stats() : ManageStats();
    m_batchLabel->setText(QStringLiteral("数据库：%1 批次 · %2 条航迹").arg(st.totalBatches).arg(st.totalTracks));
}

int SettingsPanel::fontValue() const { return m_font ? m_font->value() : UiScale::instance()->basePx(); }
void SettingsPanel::setFontValue(int px) { if (m_font) m_font->setValue(px); }
