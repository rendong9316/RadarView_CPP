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

// 字号：RadarView 根字号 14px，各处用 rem
namespace ui {
const int kFont571 = 8;     // 0.571rem
const int kFont643 = 9;     // 0.643rem
const int kFont714 = 10;    // 0.714rem
const int kFont786 = 11;    // 0.786rem
const int kFont857 = 12;    // 0.857rem
const int kFont1 = 14;      // 1rem
// 界面字体列表（RadarView: 'Segoe UI', 'PingFang SC', 'Microsoft YaHei', sans-serif；补 Linux 中文字体）
QString uiFamilies();
// 等宽：'Cascadia Code', 'JetBrains Mono', 'Consolas', 'Courier New', monospace
QString monoFamilies();
}

#endif // THEME_H
