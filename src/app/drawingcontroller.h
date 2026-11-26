#ifndef DRAWINGCONTROLLER_H
#define DRAWINGCONTROLLER_H

#include <QColor>
#include <QObject>
#include <QPoint>
#include <QString>

#include "drawingmanager.h"

class FlagStore;
class GlobeWidget;
class QAction;
class QActionGroup;
class QMainWindow;
class QTimer;
class QToolBar;
class QUndoStack;
class QWidget;
class RulerState;
class SpatialLasso;

// 地图绘图控制器：持有 DrawingManager、撤销栈、绘图工具栏、交互弹窗与 GeoJSON 存取/自动落盘
class DrawingController : public QObject
{
    Q_OBJECT
public:
    DrawingController(GlobeWidget *globe, SpatialLasso *lasso, RulerState *ruler,
                      FlagStore *flags, QWidget *dialogParent, QObject *parent = nullptr);

    void buildToolBar(QMainWindow *window);
    void setToggleAction(QAction *action);
    void setDeleteAction(QAction *action);

    DrawingManager *drawing() const { return m_draw; }
    QUndoStack *undoStack() const { return m_undo; }

    void setDrawTool(DrawTool t);
    void setBarVisible(bool on);
    bool isBarVisible() const;

    QColor drawColor() const { return m_drawColor; }
    void setDrawColor(const QColor &c);
    void setRestoringState(bool restoring) { m_restoringState = restoring; }

    void saveDrawings();
    void importDrawings();
    void saveAuto();
    void loadAuto();
    void drawDistanceCircle(const QString &flagId);

signals:
    void errorMessage(const QString &text);
    void uiStateChanged();

private:
    void updateDrawColorIcon();
    void onDrawText(double lat, double lon, const QPoint &globalPos);
    void onDrawFill(int featureId);
    void onDrawFeatureEdit(int featureId);

    GlobeWidget *m_globe = nullptr;
    SpatialLasso *m_lasso = nullptr;
    RulerState *m_ruler = nullptr;
    FlagStore *m_flags = nullptr;
    QWidget *m_dialogParent = nullptr;

    DrawingManager *m_draw = nullptr;
    QUndoStack *m_undo = nullptr;            // 绘图撤销栈（最多 200 步）
    QToolBar *m_drawBar = nullptr;           // 绘图工具条（一行）
    QAction *m_drawToggle = nullptr;         // 标题栏「帮助」右边的「绘图」入口
    QActionGroup *m_drawGroup = nullptr;
    QAction *m_drawColorAction = nullptr;
    QAction *m_undoAction = nullptr;
    QAction *m_redoAction = nullptr;
    QAction *m_deleteDrawAction = nullptr;   // Delete：删除选中的图形
    QColor m_drawColor = QColor(0xf5, 0x9e, 0x0b);
    QTimer *m_drawSaveTimer = nullptr;       // 绘图内容合并保存
    bool m_restoringState = false;
};

#endif // DRAWINGCONTROLLER_H
