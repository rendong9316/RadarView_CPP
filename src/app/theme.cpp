#include "theme.h"

#include <QApplication>
#include <QLayout>
#include <QPointer>
#include <QRegularExpression>
#include <QWidget>
#include <cmath>

#include "apppaths.h"

namespace {

// 统一深色主题：中性暗炭色系（主窗口 / 点迹窗口 / 弹窗 / 状态栏一致）
const char *const kDark[][2] = {
    {"titlebar-bg", "#252526"}, {"titlebar-fg", "#d4d4d4"}, {"titlebar-border", "#383838"},
    {"menu-bg", "#252526"}, {"menu-fg", "#d4d4d4"}, {"menu-hover", "#094771"},
    {"menu-separator", "#3e3e42"}, {"menu-shortcut", "#888888"},
    {"activitybar-bg", "#2c2c2d"}, {"activitybar-fg", "#858585"},
    {"activitybar-active", "#ffffff"}, {"activitybar-active-border", "#007acc"},
    {"sidebar-bg", "#252526"}, {"sidebar-fg", "#cccccc"}, {"sidebar-border", "#383838"}, {"sidebar-header", "#d4d4d4"},
    {"statusbar-bg", "#252526"}, {"statusbar-fg", "#d4d4d4"}, {"statusbar-border", "#383838"},
    {"editor-bg", "#1e1e1e"},
    {"bg-primary", "#1e1e1e"}, {"bg-secondary", "#252526"}, {"bg-tertiary", "#2d2d30"},
    {"surface-preview", "#181a1f"},
    {"text-primary", "#d4d4d4"}, {"text-secondary", "#a6a6a6"}, {"text-tertiary", "#787878"}, {"text-muted", "#6e6e6e"},
    {"border-primary", "#3e3e42"}, {"border-secondary", "#323236"},
    {"accent-primary", "#007acc"}, {"accent-hover", "#1a8ad4"}, {"accent-fg", "#ffffff"},
    {"accent-subtle", "rgba(0,122,204,0.18)"}, {"accent-subtle-hover", "rgba(0,122,204,0.28)"},
    {"accent-subtle-border", "rgba(0,122,204,0.55)"},
    {"button-bg", "#333337"}, {"button-hover", "#424247"}, {"button-fg", "#d4d4d4"},
    {"input-bg", "#1e1e1e"}, {"input-border", "#45454a"}, {"input-fg", "#e0e0e0"}, {"input-disabled-bg", "#28282b"},
    {"dropdown-bg", "#252526"}, {"dropdown-hover", "#094771"}, {"dropdown-fg", "#d4d4d4"},
    {"scrollbar-bg", "#1e1e1e"}, {"scrollbar-thumb", "#4a4a50"}, {"scrollbar-thumb-hover", "#606068"},
    {"error", "#f14c4c"}, {"error-bg", "rgba(241,76,76,0.16)"}, {"danger-hover", "#d32f2f"},
    {"warning", "#e5a50a"}, {"warning-bg", "rgba(229,165,10,0.16)"}, {"warning-border", "rgba(229,165,10,0.45)"},
    {"info", "#3794ff"},
    {"source-adsb", "#00d4ff"}, {"source-radar", "#00ff88"}, {"source-radar_raw", "#ff8800"},
    {"source-simulation", "#aa88ff"},
    {"cesium-bg", "#1a1a2e"}, {"cesium-globe-base", "#1a1a2e"},
};

// 统一浅色主题：清爽中性灰白系（主窗口与点迹窗口状态栏/标题栏一致）
const char *const kLight[][2] = {
    {"titlebar-bg", "#e8e8ea"}, {"titlebar-fg", "#242424"}, {"titlebar-border", "#d0d0d4"},
    {"menu-bg", "#ffffff"}, {"menu-fg", "#242424"}, {"menu-hover", "#dbeafe"},
    {"menu-separator", "#e2e2e6"}, {"menu-shortcut", "#767676"},
    {"activitybar-bg", "#e8e8ea"}, {"activitybar-fg", "#616161"},
    {"activitybar-active", "#1f1f1f"}, {"activitybar-active-border", "#005fb8"},
    {"sidebar-bg", "#f3f3f5"}, {"sidebar-fg", "#242424"}, {"sidebar-border", "#dadade"}, {"sidebar-header", "#242424"},
    {"statusbar-bg", "#e8e8ea"}, {"statusbar-fg", "#242424"}, {"statusbar-border", "#d0d0d4"},
    {"editor-bg", "#ffffff"},
    {"bg-primary", "#ffffff"}, {"bg-secondary", "#f3f3f5"}, {"bg-tertiary", "#e8e8ec"},
    {"surface-preview", "#23272e"},
    {"text-primary", "#242424"}, {"text-secondary", "#525258"}, {"text-tertiary", "#76767e"}, {"text-muted", "#909098"},
    {"border-primary", "#d0d0d6"}, {"border-secondary", "#e2e2e6"},
    {"accent-primary", "#005fb8"}, {"accent-hover", "#0068cd"}, {"accent-fg", "#ffffff"},
    {"accent-subtle", "rgba(0,95,184,0.12)"}, {"accent-subtle-hover", "rgba(0,95,184,0.20)"},
    {"accent-subtle-border", "rgba(0,95,184,0.45)"},
    {"button-bg", "#e6e6ea"}, {"button-hover", "#d8d8de"}, {"button-fg", "#242424"},
    {"input-bg", "#ffffff"}, {"input-border", "#c4c4cc"}, {"input-fg", "#242424"}, {"input-disabled-bg", "#ededf0"},
    {"dropdown-bg", "#ffffff"}, {"dropdown-hover", "#dbeafe"}, {"dropdown-fg", "#242424"},
    {"scrollbar-bg", "#f3f3f5"}, {"scrollbar-thumb", "#c0c0c8"}, {"scrollbar-thumb-hover", "#a0a0aa"},
    {"error", "#d1242f"}, {"error-bg", "rgba(209,36,47,0.10)"}, {"danger-hover", "#b41d26"},
    {"warning", "#b4690e"}, {"warning-bg", "rgba(180,105,14,0.12)"}, {"warning-border", "rgba(180,105,14,0.45)"},
    {"info", "#005fb8"},
    {"source-adsb", "#0078d4"}, {"source-radar", "#00885a"}, {"source-radar_raw", "#d47300"},
    {"source-simulation", "#7744aa"},
    {"cesium-bg", "#d6e0ea"}, {"cesium-globe-base", "#d6e0ea"},
};

// 统一高对比度主题
const char *const kHc[][2] = {
    {"titlebar-bg", "#000000"}, {"titlebar-fg", "#ffffff"}, {"titlebar-border", "#6fc3df"},
    {"menu-bg", "#0a0a0a"}, {"menu-fg", "#ffffff"}, {"menu-hover", "#1aebff"},
    {"menu-separator", "#6fc3df"}, {"menu-shortcut", "#a0a0a0"},
    {"activitybar-bg", "#000000"}, {"activitybar-fg", "#999999"},
    {"activitybar-active", "#ffffff"}, {"activitybar-active-border", "#1aebff"},
    {"sidebar-bg", "#0a0a0a"}, {"sidebar-fg", "#ffffff"}, {"sidebar-border", "#6fc3df"}, {"sidebar-header", "#ffffff"},
    {"statusbar-bg", "#000000"}, {"statusbar-fg", "#ffffff"}, {"statusbar-border", "#6fc3df"},
    {"editor-bg", "#000000"},
    {"bg-primary", "#000000"}, {"bg-secondary", "#0a0a0a"}, {"bg-tertiary", "#141414"},
    {"surface-preview", "#000000"},
    {"text-primary", "#ffffff"}, {"text-secondary", "#e0e0e0"}, {"text-tertiary", "#b0b0b0"}, {"text-muted", "#999999"},
    {"border-primary", "#6fc3df"}, {"border-secondary", "#444444"},
    {"accent-primary", "#1aebff"}, {"accent-hover", "#6fc3df"}, {"accent-fg", "#000000"},
    {"accent-subtle", "rgba(26,235,255,0.18)"}, {"accent-subtle-hover", "rgba(26,235,255,0.32)"},
    {"accent-subtle-border", "#1aebff"},
    {"button-bg", "#141414"}, {"button-hover", "#263840"}, {"button-fg", "#ffffff"},
    {"input-bg", "#0a0a0a"}, {"input-border", "#6fc3df"}, {"input-fg", "#ffffff"}, {"input-disabled-bg", "#141414"},
    {"dropdown-bg", "#0a0a0a"}, {"dropdown-hover", "#263840"}, {"dropdown-fg", "#ffffff"},
    {"scrollbar-bg", "#000000"}, {"scrollbar-thumb", "#6fc3df"}, {"scrollbar-thumb-hover", "#1aebff"},
    {"error", "#f44747"}, {"error-bg", "rgba(244,71,71,0.22)"}, {"danger-hover", "#ff6666"},
    {"warning", "#ffcc00"}, {"warning-bg", "rgba(255,204,0,0.20)"}, {"warning-border", "#ffcc00"},
    {"info", "#1aebff"},
    {"source-adsb", "#1aebff"}, {"source-radar", "#00ff88"}, {"source-radar_raw", "#ffaa00"},
    {"source-simulation", "#cc88ff"},
    {"cesium-bg", "#000000"}, {"cesium-globe-base", "#000000"},
};

template <int N>
void fill(QHash<QString, QString> &out, const char *const (&table)[N][2])
{
    out.clear();
    for (int i = 0; i < N; ++i)
        out.insert(QLatin1String(table[i][0]), QLatin1String(table[i][1]));
}

QString stripVar(const QString &var)
{
    return var.startsWith(QLatin1String("--")) ? var.mid(2) : var;
}

// "rgba(244,71,71,0.15)" 也解析成 QColor
QColor parseColor(const QString &v)
{
    if (v.startsWith(QLatin1String("rgba("))) {
        const QStringList p = v.mid(5, v.size() - 6).split(QLatin1Char(','));
        if (p.size() == 4) {
            QColor c(p[0].trimmed().toInt(), p[1].trimmed().toInt(), p[2].trimmed().toInt());
            c.setAlphaF(qBound(0.0, p[3].trimmed().toDouble(), 1.0));
            return c;
        }
    }
    return QColor(v);
}

// QSS 的 rgba() 对 0..1 的透明度解析不可靠，统一写成 #AARRGGBB
QString qssColor(const QString &v)
{
    const QColor c = parseColor(v);
    if (!c.isValid())
        return v;
    return c.alpha() == 255 ? c.name(QColor::HexRgb) : c.name(QColor::HexArgb);
}

} // namespace

Theme *Theme::instance()
{
    static Theme *t = new Theme(qApp);
    return t;
}

Theme::Theme(QObject *parent) : QObject(parent)
{
    setId(QStringLiteral("dark"));
}

void Theme::setId(const QString &id)
{
    const QString nid = id == QLatin1String("light") || id == QLatin1String("hc") ? id : QStringLiteral("dark");
    if (nid == m_id && !m_vars.isEmpty())
        return;
    m_id = nid;
    if (m_id == QLatin1String("light"))
        fill(m_vars, kLight);
    else if (m_id == QLatin1String("hc"))
        fill(m_vars, kHc);
    else
        fill(m_vars, kDark);
    emit changed();
}

QString Theme::value(const QString &var) const
{
    return m_vars.value(stripVar(var));
}

QColor Theme::color(const QString &var) const
{
    return parseColor(value(var));
}

QString Theme::qss(const QString &tmpl) const
{
    static const QRegularExpression re(QStringLiteral("var\\(--([A-Za-z0-9_-]+)(?:\\s*,\\s*([^)]*))?\\)"));
    QString out;
    out.reserve(tmpl.size());
    int last = 0;
    QRegularExpressionMatchIterator it = re.globalMatch(tmpl);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += tmpl.midRef(last, m.capturedStart() - last);
        QString v = m_vars.value(m.captured(1));
        if (v.isEmpty())
            v = m.captured(2).trimmed();
        out += qssColor(v);
        last = m.capturedEnd();
    }
    out += tmpl.midRef(last);

    return scalePx(out);
}

// 全局缩放：QSS 里所有 Npx（字号、内边距、外边距、宽高、圆角）按当前根字号换算，
// 只有边框宽度（border / border-top ... 的线宽）保持原样。基准 14px 时不变
QString Theme::scalePx(const QString &css)
{
    const double s = UiScale::instance()->scale();
    if (qAbs(s - 1.0) < 1e-4)
        return css;
    static const QRegularExpression reDecl(QStringLiteral("([A-Za-z-]+)\\s*:\\s*([^;{}]*)"));
    static const QRegularExpression rePx(QStringLiteral("(-?\\d+(?:\\.\\d+)?)px"));
    QString out;
    out.reserve(css.size() + 64);
    int pos = 0;
    QRegularExpressionMatchIterator it = reDecl.globalMatch(css);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString prop = m.captured(1).toLower();
        const bool keep = prop.startsWith(QLatin1String("border")) && !prop.contains(QLatin1String("radius"));
        if (keep || !m.captured(2).contains(QLatin1String("px")))
            continue;
        out += css.midRef(pos, m.capturedStart(2) - pos);
        const QString val = m.captured(2);
        int vp = 0;
        QRegularExpressionMatchIterator pit = rePx.globalMatch(val);
        while (pit.hasNext()) {
            const QRegularExpressionMatch pm = pit.next();
            out += val.midRef(vp, pm.capturedStart() - vp);
            const double v = pm.captured(1).toDouble();
            int nv = int(std::lround(v * s));
            if (v > 0 && nv < 1)
                nv = 1;
            out += QString::number(nv) + QStringLiteral("px");
            vp = pm.capturedEnd();
        }
        out += val.midRef(vp);
        pos = m.capturedEnd(2);
    }
    out += css.midRef(pos);
    return out;
}

void Theme::installGlobalStyle()
{
    if (!m_globalInstalled) {
        m_globalInstalled = true;
        connect(this, &Theme::changed, this, &Theme::applyGlobalStyle);
        connect(UiScale::instance(), &UiScale::changed, this, &Theme::applyGlobalStyle);
    }
    applyGlobalStyle();
}

void Theme::applyGlobalStyle()
{
    if (!qApp)
        return;
    const QString tmpl = QStringLiteral(
        "QToolTip { background: var(--menu-bg); color: var(--text-primary);"
        " border: 1px solid var(--border-primary); border-radius: 4px; padding: 4px 8px;"
        " font-family: %1; font-size: 11px; }"
        "QMessageBox { background: var(--bg-primary); color: var(--text-primary); font-family: %1; }"
        "QMessageBox QLabel { color: var(--text-primary); background: transparent; font-family: %1; font-size: 12px; }"
        "QMessageBox QPushButton { padding: 5px 16px; min-width: 64px; background: var(--button-bg);"
        " color: var(--button-fg); border: 1px solid var(--border-primary); border-radius: 4px;"
        " font-family: %1; font-size: 12px; }"
        "QMessageBox QPushButton:hover { background: var(--button-hover); color: var(--text-primary); }"
        "QMessageBox QPushButton:default { background: var(--accent-primary); color: var(--accent-fg);"
        " border-color: var(--accent-primary); }"
        "QMessageBox QPushButton:default:hover { background: var(--accent-hover); border-color: var(--accent-hover); }"
        "QProgressBar { background: var(--bg-tertiary); border: 1px solid var(--border-primary);"
        " border-radius: 3px; height: 8px; text-align: center; }"
        "QProgressBar::chunk { background: var(--accent-primary); border-radius: 2px; }")
        .arg(ui::uiFamilies()) + ui::scrollbarQss();
    qApp->setStyleSheet(qss(tmpl));
}

void setThemedStyle(QWidget *w, const QString &tmpl)
{
    const bool first = !w->property("_themeQss").isValid();
    w->setProperty("_themeQss", tmpl);
    // 样式模板里可写 ui-scale: 让字号按全局根字号换算（见 Theme::qss 的占位替换）
    w->setStyleSheet(Theme::instance()->qss(tmpl));
    if (first) {
        QPointer<QWidget> guard(w);
        auto reap = [guard]() {
            if (guard)
                guard->setStyleSheet(Theme::instance()->qss(guard->property("_themeQss").toString()));
        };
        QObject::connect(Theme::instance(), &Theme::changed, w, reap);
        QObject::connect(UiScale::instance(), &UiScale::changed, w, reap);
    }
}

namespace ui {

int defaultFontPx()
{
    // 启动时读一次持久化的根字号；后续运行时改走 UiScale::setBasePx
    app::Settings &s = app::settings();
    int px = s.value(QStringLiteral("display.font_size"), kFontDefaultPx).toInt();
    // 首次更新时升级旧的小字号；之后保留用户在设置面板里的选择。
    if (!s.value(QStringLiteral("display.readable_ui_v1"), false).toBool()) {
        px = qMax(px, kFontDefaultPx);
        app::setValue(QStringLiteral("display.font_size"), px);
        app::setValue(QStringLiteral("display.readable_ui_v1"), true);
    }
    return px;
}

QString uiFamilies()
{
    return QStringLiteral("\"Segoe UI\", \"Microsoft YaHei\", \"PingFang SC\", \"WenQuanYi Micro Hei\", "
                          "\"Noto Sans CJK SC\", sans-serif");
}

QString monoFamilies()
{
    return QStringLiteral("\"Cascadia Code\", \"JetBrains Mono\", Consolas, \"DejaVu Sans Mono\", "
                          "\"Liberation Mono\", \"Courier New\", monospace");
}

QString menuBarQss()
{
    return QStringLiteral(
        "QMenuBar { background: var(--titlebar-bg); color: var(--titlebar-fg); font-family: %1; font-size: 12px;"
        " border-bottom: 1px solid var(--titlebar-border); spacing: 2px; }"
        "QMenuBar::item { padding: 5px 10px; background: transparent; border-radius: 3px; }"
        "QMenuBar::item:selected, QMenuBar::item:pressed { background: var(--menu-hover); color: var(--text-primary); }"
        "QMenu { min-width: 200px; background: var(--menu-bg); color: var(--menu-fg);"
        " border: 1px solid var(--border-primary); border-radius: 4px; padding: 4px 0px;"
        " font-family: %1; font-size: 12px; }"
        "QMenu::item { padding: 5px 20px; min-height: 18px; }"
        "QMenu::item:selected { background: var(--menu-hover); color: var(--text-primary); }"
        "QMenu::item:disabled { color: var(--menu-shortcut); }"
        "QMenu::separator { height: 1px; margin: 4px 10px; background: var(--menu-separator); }")
        .arg(uiFamilies());
}

QString scrollbarQss()
{
    return QStringLiteral(
        "QScrollBar:vertical { background: var(--scrollbar-bg); width: 8px; margin: 0px; border: none; }"
        "QScrollBar::handle:vertical { background: var(--scrollbar-thumb); min-height: 24px; border-radius: 4px; }"
        "QScrollBar::handle:vertical:hover { background: var(--scrollbar-thumb-hover); }"
        "QScrollBar:horizontal { background: var(--scrollbar-bg); height: 8px; margin: 0px; border: none; }"
        "QScrollBar::handle:horizontal { background: var(--scrollbar-thumb); min-width: 24px; border-radius: 4px; }"
        "QScrollBar::handle:horizontal:hover { background: var(--scrollbar-thumb-hover); }"
        "QScrollBar::add-line, QScrollBar::sub-line { width: 0px; height: 0px; border: none; background: none; }"
        "QScrollBar::add-page, QScrollBar::sub-page { background: none; }");
}

QString dialogQss()
{
    return QStringLiteral(
        "QDialog { background: var(--bg-primary); color: var(--text-primary); font-family: %1; }"
        "QLabel { color: var(--text-primary); background: transparent; font-family: %1; font-size: 12px; }"
        "QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox { background: var(--input-bg); color: var(--input-fg);"
        " padding: 6px 8px; border: 1px solid var(--input-border); border-radius: 4px;"
        " font-family: %1; font-size: 12px; }"
        "QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus { border-color: var(--accent-primary); }"
        "QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled, QComboBox:disabled {"
        " background: var(--input-disabled-bg); color: var(--text-muted); border-color: var(--border-secondary); }"
        "QComboBox QAbstractItemView { background: var(--dropdown-bg); color: var(--dropdown-fg);"
        " border: 1px solid var(--border-primary); selection-background-color: var(--dropdown-hover);"
        " selection-color: var(--text-primary); }"
        "QSlider::groove:horizontal { height: 4px; background: var(--bg-tertiary); border-radius: 2px; }"
        "QSlider::sub-page:horizontal { background: var(--accent-primary); border-radius: 2px; }"
        "QSlider::handle:horizontal { width: 12px; height: 12px; margin: -4px 0px; border-radius: 6px;"
        " background: var(--accent-primary); }"
        "QPushButton { padding: 6px 14px; background: var(--button-bg); color: var(--button-fg);"
        " border: 1px solid var(--border-primary); border-radius: 4px; font-family: %1; font-size: 12px; }"
        "QPushButton:hover { background: var(--button-hover); color: var(--text-primary); }"
        "QPushButton:disabled { color: var(--text-muted); border-color: var(--border-secondary); }"
        "QDialogButtonBox QPushButton { padding: 6px 18px; min-height: 24px; min-width: 72px; }"
        "QDialogButtonBox QPushButton:default { background: var(--accent-primary); color: var(--accent-fg);"
        " border-color: var(--accent-primary); font-weight: 600; }"
        "QDialogButtonBox QPushButton:default:hover { background: var(--accent-hover); border-color: var(--accent-hover); }"
        "QDialogButtonBox QPushButton:default:disabled { background: var(--button-bg); color: var(--text-muted);"
        " border-color: var(--border-secondary); }")
        .arg(uiFamilies()) + scrollbarQss();
}

QString segmentedQss()
{
    return QStringLiteral(
        "QWidget[segmented=\"true\"] { background: var(--bg-tertiary); border: 1px solid var(--border-primary);"
        " border-radius: 4px; }"
        "QWidget[segmented=\"true\"] QPushButton { background: transparent; color: var(--text-secondary);"
        " border: none; border-radius: 3px; padding: 3px 10px; font-family: %1; font-size: 11px; }"
        "QWidget[segmented=\"true\"] QPushButton:hover { background: var(--button-hover); color: var(--text-primary); }"
        "QWidget[segmented=\"true\"] QPushButton:checked { background: var(--accent-primary); color: var(--accent-fg);"
        " font-weight: 600; }"
        "QWidget[segmented=\"true\"] QPushButton:disabled { color: var(--text-muted); background: transparent; }")
        .arg(uiFamilies());
}

} // namespace ui

UiScale *UiScale::instance()
{
    static UiScale *s = new UiScale(qApp);
    return s;
}

UiScale::UiScale(QObject *parent) : QObject(parent)
{
    m_basePx = qBound(ui::kFontMinPx, ui::defaultFontPx(), ui::kFontMaxPx);
}

int UiScale::px(double rem) const
{
    return qMax(1, int(std::lround(rem * m_basePx)));
}

int UiScale::sz(double basePx) const
{
    return qMax(1, int(std::lround(basePx * scale())));
}

void UiScale::setBasePx(int v)
{
    const int nv = qBound(ui::kFontMinPx, v, ui::kFontMaxPx);
    if (nv == m_basePx)
        return;
    m_basePx = nv;
    applyAppFont();
    emit changed();
}

// 没有单独指定字号的控件（对话框、表格、提示框等）跟随应用默认字体一起缩放
void UiScale::applyAppFont()
{
    if (!m_haveAppFont) {
        m_appFont = QApplication::font();
        m_haveAppFont = true;
    }
    QFont f = m_appFont;
    f.setFamilies(QStringList() << QStringLiteral("Microsoft YaHei") << QStringLiteral("Segoe UI")
                               << QStringLiteral("WenQuanYi Micro Hei") << QStringLiteral("Noto Sans CJK SC")
                               << m_appFont.family());
    if (m_appFont.pixelSize() > 0)
        f.setPixelSize(qMax(1, int(std::lround(m_appFont.pixelSize() * scale()))));
    else
        f.setPointSizeF(qMax(1.0, m_appFont.pointSizeF() * scale()));
    QApplication::setFont(f);
}

namespace ui {

void bindSize(QWidget *w, SizeKind kind, int a, int b)
{
    auto apply = [w, kind, a, b]() {
        switch (kind) {
        case SizeKind::Fixed: w->setFixedSize(sz(a), sz(b)); break;
        case SizeKind::FixedWidth: w->setFixedWidth(sz(a)); break;
        case SizeKind::FixedHeight: w->setFixedHeight(sz(a)); break;
        case SizeKind::MinWidth: w->setMinimumWidth(sz(a)); break;
        case SizeKind::MinHeight: w->setMinimumHeight(sz(a)); break;
        case SizeKind::MaxWidth: w->setMaximumWidth(sz(a)); break;
        }
    };
    apply();
    QObject::connect(UiScale::instance(), &UiScale::changed, w, apply);   // 以 w 为上下文，销毁后自动断开
}

void bindMargins(QLayout *l, int left, int top, int right, int bottom)
{
    auto apply = [l, left, top, right, bottom]() {
        l->setContentsMargins(sz(left), sz(top), sz(right), sz(bottom));
    };
    apply();
    QObject::connect(UiScale::instance(), &UiScale::changed, l, apply);
}

void bindSpacing(QLayout *l, int spacing)
{
    auto apply = [l, spacing]() { l->setSpacing(sz(spacing)); };
    apply();
    QObject::connect(UiScale::instance(), &UiScale::changed, l, apply);
}

} // namespace ui
