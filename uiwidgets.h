#ifndef UIWIDGETS_H
#define UIWIDGETS_H

#include <QMenu>
#include <QPushButton>
#include <QToolButton>
#include <QWidget>
#include <functional>

#include "lucide.h"

class QLabel;

// 通用小部件，样式逐项对应 RadarView 的 ConfirmDialog / HelpTip / UndoToast / 地图右键菜单
namespace ui {

// 自检时自动确认，不弹对话框
void setAutoConfirm(bool on);
bool autoConfirm();

// ConfirmDialog.vue：遮罩 rgba(0,0,0,.5)，居中卡片；danger 时图标为警告、确认按钮 #d32f2f
bool confirm(QWidget *parent, const QString &message, const QString &title = QStringLiteral("确认操作"),
             bool danger = false, const QString &confirmText = QStringLiteral("确认"),
             const QString &cancelText = QStringLiteral("取消"));
// PromptModal.vue：单行输入；取消返回 false
bool prompt(QWidget *parent, const QString &message, const QString &defaultValue, QString *out);

// 图标按钮：lucide 图标 + 主题色，悬停换色
class IconButton : public QToolButton
{
    Q_OBJECT
public:
    IconButton(LucideIcon icon, int iconPx, const char *colorVar, const char *hoverVar, QWidget *parent = nullptr);
    void setIconColors(const char *colorVar, const char *hoverVar);

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    LucideIcon m_icon;
    int m_px;
    QByteArray m_color, m_hover;
};

// HelpTip.vue：16px 圆形问号，悬停显示说明
class HelpTip : public QToolButton
{
    Q_OBJECT
public:
    HelpTip(const QString &text, QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *e) override;
    void enterEvent(QEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    QString m_text;
    bool m_hover = false;
};

// CesiumMap.vue 右键菜单：#1e1e2e 底、#3a3a5c 边框、圆角 6、13px，项内边距 8px 16px，
// 悬停为主题强调色，危险项悬停 #ef4444
class ContextMenu : public QMenu
{
    Q_OBJECT
public:
    explicit ContextMenu(QWidget *parent = nullptr);
    // 返回该项的 action（自检用 trigger()）
    QAction *addItem(LucideIcon icon, const QString &text, std::function<void()> fn, bool danger = false);
};

// 表格行右键菜单（ManageDataTable.vue .context-menu）：bg-primary 底、0.714rem
class SmallMenu : public QMenu
{
    Q_OBJECT
public:
    explicit SmallMenu(QWidget *parent = nullptr);
    QAction *addItem(LucideIcon icon, const QString &text, std::function<void()> fn, bool danger = false);
};

// UndoToast.vue：窗口底部居中「已删除 <label>[ 等 N 组]  撤销」
class UndoToast : public QWidget
{
    Q_OBJECT
public:
    explicit UndoToast(QWidget *parent);
    void setState(const QString &label, int count);
    QString text() const;

signals:
    void undoClicked();

protected:
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    void place();
    QLabel *m_icon = nullptr;
    QLabel *m_text = nullptr;
    QPushButton *m_btn = nullptr;
};

} // namespace ui

#endif // UIWIDGETS_H
