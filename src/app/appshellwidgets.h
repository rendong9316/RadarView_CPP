#ifndef APPSHELLWIDGETS_H
#define APPSHELLWIDGETS_H

#include <QAbstractButton>
#include <QList>
#include <QString>
#include <QWidget>

class QLabel;
class QPaintEvent;
class QStackedWidget;

// 侧栏面板（RadarView useActivityBar.ts PanelId）：Settings 保留原索引 3 以兼容旧设置持久化
enum class PanelId { Manage = 0, Flags, TimeFilter, Settings, Diagnostics, Count };

namespace appshell {

const int kSidebarDefault = 280;
const int kSidebarMin = 200;
const int kSidebarMax = 900;

QString panelTitle(PanelId id);

} // namespace appshell

// 左侧活动栏（ActivityBar.vue）：48px 宽，面板图标 + 底部「设置」，当前项左侧 2px 强调色竖条
class ActivityBar : public QWidget
{
    Q_OBJECT
public:
    explicit ActivityBar(QWidget *parent = nullptr);
    void highlight(int index);       // -1 表示都不高亮
    int activeIndex() const { return m_active; }

signals:
    void iconClicked(int index);

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    QList<QAbstractButton *> m_buttons;
    int m_active = -1;
};

// 侧栏（SideBar.vue）：32px 标题栏 + 关闭按钮，内容区内边距 8px
class SidePanel : public QWidget
{
    Q_OBJECT
public:
    explicit SidePanel(QWidget *parent = nullptr);
    void addPanel(QWidget *w);                    // 放进滚动区后加入
    void replacePanel(int index, QWidget *w);     // 替换某个面板（旧的延后销毁）
    void showPanel(int index, const QString &title);
    int currentPanel() const;
    QString title() const;

signals:
    void closeRequested();

private:
    QLabel *m_title;
    QStackedWidget *m_stack;
};

#endif // APPSHELLWIDGETS_H
