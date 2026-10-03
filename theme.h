#ifndef THEME_H
#define THEME_H

#include <QColor>
#include <QHash>
#include <QObject>
#include <QString>

class QWidget;

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

    // 把样式模板里的 var(--xxx) 换成当前主题值，其余原样保留
    QString qss(const QString &tmpl) const;

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

// 全局字号缩放（对应 RadarView useFontSize.ts：根字号 10–20px，全 UI 文本按 rem 等比）。
// 基准 14px 时各 rem 字号与 RadarView 一致（0.571rem=8px … 1rem=14px）。
// 界面各控件/地图内文字统一经 ui::px() 取像素值，改这里即全局生效。
class UiScale : public QObject
{
    Q_OBJECT
public:
    static UiScale *instance();
    int basePx() const { return m_basePx; }      // 当前根字号（10–20）
    double scale() const { return m_basePx / 14.0; }

    // 逻辑 rem 值 -> 当前根字号下的像素值
    int px(double rem) const;

    // 设根字号（自动钳制 10–20），立即发 changed()；持久化由调用方负责
    void setBasePx(int v);

signals:
    void changed();

private:
    explicit UiScale(QObject *parent = nullptr);
    int m_basePx = 14;
};

namespace ui {
// 字号：RadarView 根字号 14px，各处用 rem
// 逻辑 rem 字号 -> 当前根字号下的像素值（原 kFont* 常量改成调用它）
inline int px(double rem) { return UiScale::instance()->px(rem); }
inline double fontScale() { return UiScale::instance()->scale(); }
int defaultFontPx();      // 读 QSettings display.font_size，缺省 14
// 界面字体列表（RadarView: 'Segoe UI', 'PingFang SC', 'Microsoft YaHei', sans-serif；补 Linux 中文字体）
QString uiFamilies();
// 等宽：'Cascadia Code', 'JetBrains Mono', 'Consolas', 'Courier New', monospace
QString monoFamilies();
}

#endif // THEME_H
