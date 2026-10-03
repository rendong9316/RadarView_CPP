#include "theme.h"

#include <QApplication>
#include <QPointer>
#include <QRegularExpression>
#include <QWidget>
#include <cmath>
#include <QSettings>

#include "apppaths.h"

namespace {

// RadarView src/themes/dark.ts
const char *const kDark[][2] = {
    {"titlebar-bg", "#2d2d2d"}, {"titlebar-fg", "#cccccc"}, {"titlebar-border", "#3c3c3c"},
    {"menu-bg", "#252526"}, {"menu-fg", "#cccccc"}, {"menu-hover", "#094771"},
    {"menu-separator", "#454545"}, {"menu-shortcut", "#888888"},
    {"activitybar-bg", "#333333"}, {"activitybar-fg", "#858585"},
    {"activitybar-active", "#ffffff"}, {"activitybar-active-border", "#007acc"},
    {"sidebar-bg", "#252526"}, {"sidebar-fg", "#cccccc"}, {"sidebar-border", "#3c3c3c"}, {"sidebar-header", "#cccccc"},
    {"statusbar-bg", "#1f2330"}, {"statusbar-fg", "#e0e0e0"}, {"statusbar-border", "#2d3240"},
    {"editor-bg", "#1e1e1e"},
    {"bg-primary", "#1e1e1e"}, {"bg-secondary", "#252526"}, {"bg-tertiary", "#2d2d2d"},
    {"text-primary", "#cccccc"}, {"text-secondary", "#a0a0a0"}, {"text-tertiary", "#6a6a6a"},
    {"border-primary", "#3e3e3e"}, {"border-secondary", "#2d2d2d"},
    {"accent-primary", "#007acc"}, {"accent-hover", "#1a8ad4"},
    {"button-bg", "#3c3c3c"}, {"button-hover", "#4c4c4c"}, {"button-fg", "#cccccc"},
    {"input-bg", "#3c3c3c"}, {"input-border", "#555555"}, {"input-fg", "#cccccc"},
    {"dropdown-bg", "#252526"}, {"dropdown-hover", "#094771"}, {"dropdown-fg", "#cccccc"},
    {"scrollbar-bg", "#1e1e1e"}, {"scrollbar-thumb", "#424242"},
    {"error", "#f44747"}, {"error-bg", "rgba(244,71,71,0.15)"}, {"warning", "#cca700"}, {"info", "#3794ff"},
    {"source-adsb", "#00d4ff"}, {"source-radar", "#00ff88"}, {"source-radar_raw", "#ff8800"},
    {"source-simulation", "#aa88ff"},
    {"cesium-bg", "#1a1a2e"}, {"cesium-globe-base", "#1a1a2e"},
};

// RadarView src/themes/light.ts
const char *const kLight[][2] = {
    {"titlebar-bg", "#dddddd"}, {"titlebar-fg", "#333333"}, {"titlebar-border", "#c8c8c8"},
    {"menu-bg", "#f0f0f0"}, {"menu-fg", "#333333"}, {"menu-hover", "#cce5ff"},
    {"menu-separator", "#d4d4d4"}, {"menu-shortcut", "#888888"},
    {"activitybar-bg", "#dddddd"}, {"activitybar-fg", "#666666"},
    {"activitybar-active", "#333333"}, {"activitybar-active-border", "#005fb8"},
    {"sidebar-bg", "#f3f3f3"}, {"sidebar-fg", "#333333"}, {"sidebar-border", "#e5e5e5"}, {"sidebar-header", "#333333"},
    {"statusbar-bg", "#005fb8"}, {"statusbar-fg", "#ffffff"}, {"statusbar-border", "#004c9a"},
    {"editor-bg", "#ffffff"},
    {"bg-primary", "#ffffff"}, {"bg-secondary", "#f3f3f3"}, {"bg-tertiary", "#ececec"},
    {"text-primary", "#333333"}, {"text-secondary", "#555555"}, {"text-tertiary", "#888888"},
    {"border-primary", "#e5e5e5"}, {"border-secondary", "#d4d4d4"},
    {"accent-primary", "#005fb8"}, {"accent-hover", "#0068cd"},
    {"button-bg", "#e0e0e0"}, {"button-hover", "#d0d0d0"}, {"button-fg", "#333333"},
    {"input-bg", "#ffffff"}, {"input-border", "#c8c8c8"}, {"input-fg", "#333333"},
    {"dropdown-bg", "#f0f0f0"}, {"dropdown-hover", "#cce5ff"}, {"dropdown-fg", "#333333"},
    {"scrollbar-bg", "#f3f3f3"}, {"scrollbar-thumb", "#c1c1c1"},
    {"error", "#e51400"}, {"error-bg", "rgba(229,20,0,0.1)"}, {"warning", "#bf8803"}, {"info", "#0066cc"},
    {"source-adsb", "#0078d4"}, {"source-radar", "#00885a"}, {"source-radar_raw", "#d47300"},
    {"source-simulation", "#7744aa"},
    {"cesium-bg", "#d6e0ea"}, {"cesium-globe-base", "#d6e0ea"},
};

// RadarView src/themes/highContrast.ts
const char *const kHc[][2] = {
    {"titlebar-bg", "#000000"}, {"titlebar-fg", "#ffffff"}, {"titlebar-border", "#6fc3df"},
    {"menu-bg", "#0a0a0a"}, {"menu-fg", "#ffffff"}, {"menu-hover", "#1aebff"},
    {"menu-separator", "#6fc3df"}, {"menu-shortcut", "#a0a0a0"},
    {"activitybar-bg", "#000000"}, {"activitybar-fg", "#999999"},
    {"activitybar-active", "#ffffff"}, {"activitybar-active-border", "#1aebff"},
    {"sidebar-bg", "#0a0a0a"}, {"sidebar-fg", "#ffffff"}, {"sidebar-border", "#6fc3df"}, {"sidebar-header", "#ffffff"},
    {"statusbar-bg", "#000000"}, {"statusbar-fg", "#ffffff"}, {"statusbar-border", "#6fc3df"},
    {"editor-bg", "#000000"},
    {"bg-primary", "#000000"}, {"bg-secondary", "#0a0a0a"}, {"bg-tertiary", "#111111"},
    {"text-primary", "#ffffff"}, {"text-secondary", "#e0e0e0"}, {"text-tertiary", "#a0a0a0"},
    {"border-primary", "#6fc3df"}, {"border-secondary", "#444444"},
    {"accent-primary", "#1aebff"}, {"accent-hover", "#6fc3df"},
    {"button-bg", "#111111"}, {"button-hover", "#1aebff"}, {"button-fg", "#ffffff"},
    {"input-bg", "#0a0a0a"}, {"input-border", "#6fc3df"}, {"input-fg", "#ffffff"},
    {"dropdown-bg", "#0a0a0a"}, {"dropdown-hover", "#1aebff"}, {"dropdown-fg", "#ffffff"},
    {"scrollbar-bg", "#000000"}, {"scrollbar-thumb", "#6fc3df"},
    {"error", "#f44747"}, {"error-bg", "rgba(244,71,71,0.2)"}, {"warning", "#ffcc00"}, {"info", "#1aebff"},
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

    // 全局字号缩放：把 QSS 里的 font-size: Npx 按当前根字号放大/缩小（基准 14px 时不变）
    const double s = UiScale::instance()->scale();
    if (qAbs(s - 1.0) > 1e-4) {
        static const QRegularExpression reSize(QStringLiteral("(font-size:\\s*)(\\d+)px"));
        QString scaled;
        scaled.reserve(out.size());
        int pos = 0;
        QRegularExpressionMatchIterator it = reSize.globalMatch(out);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            scaled += out.mid(pos, m.capturedStart() - pos);
            const int pxv = m.captured(2).toInt();
            scaled += QStringLiteral("font-size: %1px").arg(int(std::lround(double(pxv) * s)));
            pos = m.capturedEnd();
        }
        scaled += out.mid(pos);
        out = scaled;
    }
    return out;
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
    return app::settings().value(QStringLiteral("display.font_size"), 14).toInt();
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

} // namespace ui

UiScale *UiScale::instance()
{
    static UiScale *s = new UiScale(qApp);
    return s;
}

UiScale::UiScale(QObject *parent) : QObject(parent)
{
    m_basePx = qBound(10, ui::defaultFontPx(), 20);
}

int UiScale::px(double rem) const
{
    return qMax(1, int(std::lround(rem * m_basePx)));
}

void UiScale::setBasePx(int v)
{
    const int nv = qBound(10, v, 20);
    if (nv == m_basePx)
        return;
    m_basePx = nv;
    emit changed();
}
