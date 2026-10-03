#ifndef THEME_H
#define THEME_H

#include <QColor>
#include <QFont>
#include <QHash>
#include <QObject>
#include <QString>

class QWidget;
class QLayout;

// 界面主题色：逐项取自 RadarView src/themes/{dark,light,highContrast}.ts（CSS 变量 --xxx）。
// 状态栏切换主题时同步到这里，侧栏、面板、对话框都按当前主题重新套样式
class Theme : public QObject
{
    Q_OBJECT
public:
    static Theme *instance();

    QString id() const { return m_id; }
    void setId(const QString &id);            // dark / light / hc；不写 QSettings（由状态栏负责）

    // 变量名带不带前缀 "--" 都可以，如 "bg-primary"
    QColor color(const QString &var) const;
    QString value(const QString &var) const;  // 原始字符串（可能是 rgba(...)）

    // 把样式模板里的 var(--xxx) 换成当前主题值，并按全局字号缩放 px 尺寸
    QString qss(const QString &tmpl) const;
    // 只做 px 尺寸缩放（不经主题替换的样式表用）
    static QString scalePx(const QString &css);

signals:
    void changed();

private:
    explicit Theme(QObject *parent = nullptr);
    QString m_id;
    QHash<QString, QString> m_vars;
};

// 给控件套主题样式模板：立即生效，主题切换后自动重新套用
void setThemedStyle(QWidget *w, const QString &tmpl);
inline QColor themeColor(const char *var) { return Theme::instance()->color(QLatin1String(var)); }

namespace ui {
const int kFontMinPx = 10, kFontMaxPx = 50, kFontBasePx = 14;   // 根字号范围（RadarView 为 10–20，这里放宽到 50）
}

// 全局字号缩放（对应 RadarView useFontSize.ts：根字号可调，全 UI 按 rem 等比）。
// 基准 14px 时各尺寸与 RadarView 一致；文字经 ui::px()，控件尺寸经 ui::sz() / ui::bindSize()，
// QSS 里的 px 值（边框宽度除外）由 Theme::qss() 统一换算，所以字号变大时容器同步变大、互不重叠。
class UiScale : public QObject
{
    Q_OBJECT
public:
    static UiScale *instance();
    int basePx() const { return m_basePx; }      // 当前根字号（10–50）
    double scale() const { return m_basePx / double(ui::kFontBasePx); }

    // 逻辑 rem 值 -> 当前根字号下的像素值
    int px(double rem) const;
    // 基准（14px 根字号）下的像素尺寸 -> 当前尺寸
    int sz(double basePx) const;

    // 设根字号（自动钳制 10–50），立即发 changed()；持久化由调用方负责
    void setBasePx(int v);
    // 应用默认字体（没有单独设字号的控件、对话框、提示框）按比例缩放
    void applyAppFont();

signals:
    void changed();

private:
    explicit UiScale(QObject *parent = nullptr);
    int m_basePx = ui::kFontBasePx;
    bool m_haveAppFont = false;
    QFont m_appFont;          // 启动时的应用默认字体
};

namespace ui {
// 逻辑 rem 字号 -> 当前根字号下的像素值（原 kFont* 常量改成调用它）
inline int px(double rem) { return UiScale::instance()->px(rem); }
inline int sz(double basePx) { return UiScale::instance()->sz(basePx); }
inline double fontScale() { return UiScale::instance()->scale(); }
int defaultFontPx();      // 读 QSettings display.font_size，缺省 14

// 控件尺寸随字号缩放：立即按当前比例设置，之后字号每次变化自动重设（控件销毁后自动断开）
enum class SizeKind { Fixed, FixedWidth, FixedHeight, MinWidth, MinHeight, MaxWidth };
void bindSize(QWidget *w, SizeKind kind, int a, int b = 0);
// 布局边距 / 间距随字号缩放（基准 14px 下的值）
void bindMargins(QLayout *l, int left, int top, int right, int bottom);
void bindSpacing(QLayout *l, int spacing);
// 界面字体列表（RadarView: 'Segoe UI', 'PingFang SC', 'Microsoft YaHei', sans-serif；补 Linux 中文字体）
QString uiFamilies();
// 等宽：'Cascadia Code', 'JetBrains Mono', 'Consolas', 'Courier New', monospace
QString monoFamilies();
}

#endif // THEME_H
