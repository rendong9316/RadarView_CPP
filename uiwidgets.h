#ifndef UIWIDGETS_H
#define UIWIDGETS_H

#include <QMenu>
#include <QDialog>
#include <QColor>
#include <QPushButton>
#include <QToolButton>
#include <QVector>
#include <QWidget>
#include <functional>

#include "lucide.h"

class QLabel;
class QLineEdit;
class QDoubleSpinBox;
class QSpinBox;

// 通用小部件，样式逐项对应 RadarView 的 ConfirmDialog / HelpTip / UndoToast / 地图右键菜单
namespace ui {

// 雷达导入前设置名称与标准色；取消时不启动后台导入。
class RadarImportDialog : public QDialog
{
public:
    RadarImportDialog(const QString &filePath, const QString &defaultName, const QColor &defaultColor,
                      const QStringList &usedNames, QWidget *parent = nullptr, bool nameOnly = false);
    QString trackName() const;
    QColor trackColor() const { return m_color; }

private:
    QLineEdit *m_name = nullptr;
    QColor m_color;
};

// 标准色盘（20 色）：雷达导入弹窗与「绘制距离圆」弹窗共用同一份实现，避免两处各写一遍。
// 色块 objectName 默认 radarColor_0..19 —— 自检按这个名字点色块，改名前先改自检。
class ColorSwatchGrid : public QWidget
{
    Q_OBJECT
public:
    explicit ColorSwatchGrid(const QColor &initial, QWidget *parent = nullptr,
                             const QString &objectPrefix = QStringLiteral("radarColor"));
    QColor color() const { return m_color; }
    void setColor(const QColor &c);      // 颜色不在色盘里时所有色块都不选中

signals:
    void colorChanged(const QColor &c);

private:
    void checkFor(const QColor &c);
    QColor m_color;
    QString m_prefix;
    QVector<QPushButton *> m_swatches;
    QVector<QColor> m_colors;
};

// 旗标右键「绘制距离圆」：圆心信息 + 半径（公里）+ 标准色盘，
// 版式与 RadarImportDialog 一致（标题/说明/输入/色盘/预览/错误行/按钮）。
class DistanceCircleDialog : public QDialog
{
    Q_OBJECT
public:
    DistanceCircleDialog(const QString &flagLabel, double lat, double lon, const QColor &defaultColor,
                         QWidget *parent = nullptr);
    double radiusKm() const;
    QColor color() const { return m_color; }

private:
    QColor m_color;
    QDoubleSpinBox *m_radius = nullptr;
};

// 油漆桶：填充色 + 不透明度（alpha 直接写进 QColor）
class FillDialog : public QDialog
{
    Q_OBJECT
public:
    explicit FillDialog(const QColor &initial, QWidget *parent = nullptr);
    QColor fillColor() const;          // 已带 alpha，可直接交给 DrawingManager::setFill
    int opacityPercent() const { return m_opacity; }

private:
    QColor m_color;
    int m_opacity = 35;
};

// 文本框：内容 + 字号 + 颜色，确认后放到点击位置
class TextDialog : public QDialog
{
    Q_OBJECT
public:
    explicit TextDialog(const QColor &initial, QWidget *parent = nullptr);
    QString text() const;
    QColor color() const { return m_color; }
    int fontPx() const;

private:
    QColor m_color;
    QLineEdit *m_text = nullptr;
    QSpinBox *m_size = nullptr;
};

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
