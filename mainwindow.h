#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QWidget>
#include <QPushButton>
#include <QLabel>
#include <QSize>
#include <QList>

class ActivityBar;
class SidePanel;
class GlobeWidget;
class QSplitter;
class QStackedWidget;

// 左侧活动栏（Activity Bar）：竖排 5 个图标，点击展开/收起右侧侧栏
class ActivityBar : public QWidget
{
    Q_OBJECT
public:
    explicit ActivityBar(QWidget *parent = nullptr);
    void highlight(int index);
    int activeIndex() const;

signals:
    void iconClicked(int index);

private:
    QPushButton *m_buttons[5];
    int m_activeIndex = -1;
};

// 侧栏：可折叠，内容为空占位（要求：可以"啥也没有"）
class SidePanel : public QWidget
{
    Q_OBJECT
public:
    explicit SidePanel(QWidget *parent = nullptr);
    void setHeaderText(const QString &text);

private:
    QLabel *m_title;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

    GlobeWidget *globe() const { return m_globe; }
    bool openTiles(const QString &path);
    void loadDefaultTiles();

private slots:
    void onMenuActionTriggered();
    void onIconClicked(int index);
    void toggleSidePanel();
    void openTileFile();

private:
    void buildMenuBar();
    QStackedWidget *buildEditorArea();

    ActivityBar   *m_activityBar;
    SidePanel     *m_sidePanel;
    QSplitter     *m_splitter;
    QStackedWidget *m_editor;
    GlobeWidget   *m_globe = nullptr;
    QLabel        *m_globeStatus = nullptr;
};

#endif // MAINWINDOW_H
