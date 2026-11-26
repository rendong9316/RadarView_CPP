#include "drawingcontroller.h"

#include <QAction>
#include <QActionGroup>
#include <QColorDialog>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QMainWindow>
#include <QMessageBox>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolBar>
#include <QUndoStack>

#include "apppaths.h"
#include "globewidget.h"
#include "maptools.h"
#include "spatiallasso.h"
#include "theme.h"
#include "uiwidgets.h"

namespace {

// 工具栏上的画笔颜色小方块
QIcon colorSwatchIcon(const QColor &c, int px)
{
    QPixmap pm(px, px);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(c);
    p.setPen(QPen(QColor(0x88, 0x88, 0x88), 1));
    p.drawRoundedRect(QRectF(0.5, 0.5, px - 1, px - 1), 3, 3);
    p.end();
    return QIcon(pm);
}

} // namespace

DrawingController::DrawingController(GlobeWidget *globe, SpatialLasso *lasso, RulerState *ruler,
                                     FlagStore *flags, QWidget *dialogParent, QObject *parent)
    : QObject(parent)
    , m_globe(globe)
    , m_lasso(lasso)
    , m_ruler(ruler)
    , m_flags(flags)
    , m_dialogParent(dialogParent)
{
    m_draw = new DrawingManager(this);
    m_undo = new QUndoStack(this);
    m_undo->setUndoLimit(200);          // 需求：最多保留 200 步操作历史
    m_draw->setUndoStack(m_undo);
    m_globe->setDrawingManager(m_draw);
    m_globe->setDrawColor(m_drawColor);

    m_drawSaveTimer = new QTimer(this);
    m_drawSaveTimer->setSingleShot(true);
    m_drawSaveTimer->setInterval(800);
    connect(m_drawSaveTimer, &QTimer::timeout, this, &DrawingController::saveAuto);

    connect(m_draw, &DrawingManager::changed, m_globe, [this]() { m_globe->update(); });
    // 绘图内容自动落盘（用户手动「保存绘图」是导出 GeoJSON，这里是不丢内容的自动保存）
    connect(m_draw, &DrawingManager::changed, this, [this]() {
        if (!m_restoringState && m_drawSaveTimer)
            m_drawSaveTimer->start();
    });
    connect(m_globe, &GlobeWidget::drawTextRequested, this, &DrawingController::onDrawText);
    connect(m_globe, &GlobeWidget::drawFillRequested, this, &DrawingController::onDrawFill);
    connect(m_globe, &GlobeWidget::drawFeatureEditRequested, this, &DrawingController::onDrawFeatureEdit);
    connect(m_globe, &GlobeWidget::drawSelectionChanged, this, [this](int id) {
        if (m_deleteDrawAction)
            m_deleteDrawAction->setEnabled(id > 0);
    });
    connect(m_globe, &GlobeWidget::drawMessage, this, &DrawingController::errorMessage);
}

void DrawingController::setToggleAction(QAction *action)
{
    m_drawToggle = action;
}

void DrawingController::setDeleteAction(QAction *action)
{
    m_deleteDrawAction = action;
    if (m_deleteDrawAction)
        m_deleteDrawAction->setEnabled(m_globe->selectedFeature() > 0);
}

// 绘图工具栏平时不显示：左上角菜单栏的「绘图」按钮点开才出现。
void DrawingController::buildToolBar(QMainWindow *window)
{
    const QString barQss = QStringLiteral(
        "QToolBar { background: var(--titlebar-bg); border: none;"
        " border-bottom: 1px solid var(--titlebar-border); padding: 3px 6px; spacing: 3px; }"
        "QToolBar::separator { background: var(--border-primary); width: 1px; margin: 4px 8px; }"
        "QToolButton { color: var(--text-secondary); background: transparent; border: 1px solid transparent;"
        " padding: 4px 10px; border-radius: 4px; font-family: %1; font-size: 12px; }"
        "QToolButton:hover { background: var(--button-hover); color: var(--text-primary); }"
        "QToolButton:checked { background: var(--accent-primary); color: var(--accent-fg);"
        " border-color: var(--accent-primary); font-weight: 600; }"
        "QToolButton:disabled { color: var(--text-muted); }").arg(ui::uiFamilies());

    m_drawBar = new QToolBar(tr("绘图"), window);
    m_drawBar->setObjectName(QStringLiteral("drawToolBar"));
    m_drawBar->setMovable(false);
    m_drawBar->setFloatable(false);
    m_drawBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_drawBar->setIconSize(QSize(ui::sz(14), ui::sz(14)));
    connect(UiScale::instance(), &UiScale::changed, m_drawBar, [this]() {
        m_drawBar->setIconSize(QSize(ui::sz(14), ui::sz(14)));
        updateDrawColorIcon();
    });
    window->addToolBar(Qt::TopToolBarArea, m_drawBar);
    setThemedStyle(m_drawBar, barQss);

    m_drawGroup = new QActionGroup(this);
    m_drawGroup->setExclusive(true);
    auto addTool = [this](DrawTool t, const QString &text, const QString &tip) {
        QAction *a = m_drawBar->addAction(text);
        a->setCheckable(true);
        a->setToolTip(tip);
        a->setData(int(t));
        m_drawGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, t]() { setDrawTool(t); });
        return a;
    };
    // 工具按钮用文字（lucide 里没有直线/矩形/油漆桶这些图标，硬凑反而看不懂）
    addTool(DrawTool::None, tr("漫游"), tr("左键拖拽旋转地球（默认）"))->setChecked(true);
    addTool(DrawTool::Select, tr("选择"), tr("单击图形选中：Delete 删除，双击改属性"));
    addTool(DrawTool::Line, tr("直线"), tr("按住左键拖出一条贴地直线，端点会吸附到已有顶点"));
    addTool(DrawTool::Polyline, tr("曲线"), tr("按住左键自由手绘"));
    addTool(DrawTool::Polygon, tr("多边形"), tr("单击放顶点，双击或右键闭合；吸到起点自动闭合"));
    addTool(DrawTool::Rect, tr("矩形"), tr("按住左键拖出矩形"));
    addTool(DrawTool::Text, tr("文本"), tr("单击放置文本框"));
    addTool(DrawTool::Fill, tr("填充"), tr("单击封闭图形内部，设置颜色与透明度"));
    addTool(DrawTool::Eraser, tr("橡皮"), tr("单击线条或图形删除"));
    addTool(DrawTool::Break, tr("裁剪/打断"),
            tr("拖拽划一条剪切线：裁掉中间那一段\n单击线段：在该点一分为二"));
    addTool(DrawTool::BoxSelect, tr("框选删除"), tr("拉框删除与框相交的图形（可撤销）"));

    // ---- 同一行继续：撤销/重做、颜色、存取 ----
    m_drawBar->addSeparator();
    // 撤销/重做：动作挂在主窗口上（工具栏收起时 Ctrl+Z / Ctrl+Y 照样有效）
    m_undoAction = new QAction(tr("撤销"), window);
    m_undoAction->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_Z));
    m_undoAction->setShortcutContext(Qt::WindowShortcut);
    m_undoAction->setToolTip(tr("撤销上一步绘图操作（Ctrl+Z）"));
    m_undoAction->setEnabled(false);
    window->addAction(m_undoAction);
    connect(m_undoAction, &QAction::triggered, m_undo, &QUndoStack::undo);
    connect(m_undo, &QUndoStack::canUndoChanged, m_undoAction, &QAction::setEnabled);
    m_drawBar->addAction(m_undoAction);

    m_redoAction = new QAction(tr("重做"), window);
    m_redoAction->setShortcuts(QList<QKeySequence>()
                               << QKeySequence(Qt::CTRL + Qt::Key_Y)
                               << QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_Z));
    m_redoAction->setShortcutContext(Qt::WindowShortcut);
    m_redoAction->setToolTip(tr("重做被撤销的操作（Ctrl+Y）"));
    m_redoAction->setEnabled(false);
    window->addAction(m_redoAction);
    connect(m_redoAction, &QAction::triggered, m_undo, &QUndoStack::redo);
    connect(m_undo, &QUndoStack::canRedoChanged, m_redoAction, &QAction::setEnabled);
    m_drawBar->addAction(m_redoAction);

    m_drawBar->addSeparator();
    m_drawColorAction = m_drawBar->addAction(tr("颜色"));
    m_drawColorAction->setToolTip(tr("新建图形使用的颜色"));
    connect(m_drawColorAction, &QAction::triggered, this, [this]() {
        const QColor c = QColorDialog::getColor(m_drawColor, m_dialogParent, tr("绘图颜色"));
        if (!c.isValid())
            return;
        app::setValue(QStringLiteral("drawing.color"), c.name());
        setDrawColor(c);
    });
    updateDrawColorIcon();

    m_drawBar->addSeparator();
    QAction *save = m_drawBar->addAction(tr("保存"));
    save->setToolTip(tr("把当前所有绘图导出为 GeoJSON"));
    connect(save, &QAction::triggered, this, &DrawingController::saveDrawings);
    QAction *load = m_drawBar->addAction(tr("导入"));
    load->setToolTip(tr("从 GeoJSON 导入绘图"));
    connect(load, &QAction::triggered, this, &DrawingController::importDrawings);
    QAction *clr = m_drawBar->addAction(tr("清空"));
    clr->setToolTip(tr("删除所有绘图（可撤销）"));
    connect(clr, &QAction::triggered, this, [this]() {
        if (m_draw->count() == 0)
            return;
        if (ui::confirm(m_dialogParent, tr("确定清空所有绘图（%1 个图形）？").arg(m_draw->count()),
                        tr("清空绘图"), true))
            m_draw->clear();
    });

    setBarVisible(false);
}

void DrawingController::setBarVisible(bool on)
{
    if (m_drawBar)
        m_drawBar->setVisible(on);
    if (m_drawToggle) {
        QSignalBlocker block(m_drawToggle);
        m_drawToggle->setChecked(on);
    }
    if (!on)
        setDrawTool(DrawTool::None);      // 收起工具条就回到漫游，鼠标不会还停在画图状态
    emit uiStateChanged();
}

bool DrawingController::isBarVisible() const
{
    return m_drawBar && !m_drawBar->isHidden();
}

void DrawingController::setDrawColor(const QColor &c)
{
    if (!c.isValid())
        return;
    m_drawColor = c;
    m_globe->setDrawColor(c);
    updateDrawColorIcon();
}

void DrawingController::updateDrawColorIcon()
{
    if (m_drawColorAction)
        m_drawColorAction->setIcon(colorSwatchIcon(m_drawColor, ui::sz(14)));
}

// 工具切换：绘图与套索/标尺互斥（否则会抢同一个左键）
void DrawingController::setDrawTool(DrawTool t)
{
    if (t != DrawTool::None) {
        if (m_lasso && m_lasso->isActive())
            m_lasso->deactivate();
        if (m_ruler && m_ruler->isActive())
            m_ruler->deactivate();
    }
    m_globe->setDrawTool(t);
    if (m_drawGroup) {
        for (QAction *a : m_drawGroup->actions())
            a->setChecked(a->data().toInt() == int(t));
    }
}

void DrawingController::onDrawText(double lat, double lon, const QPoint &globalPos)
{
    Q_UNUSED(globalPos);
    if (app::testMode())
        return;                       // 自检直接调 DrawingManager，不弹窗
    ui::TextDialog dlg(m_drawColor, m_dialogParent);
    if (dlg.exec() != QDialog::Accepted)
        return;
    if (m_draw->addText(lat, lon, dlg.text(), dlg.color(), dlg.fontPx()) <= 0)
        emit errorMessage(tr("文本内容为空，未插入"));
    else
        emit errorMessage(QString());
}

void DrawingController::onDrawFill(int featureId)
{
    if (featureId <= 0) {
        emit errorMessage(tr("该位置没有可填充的封闭图形（圆 / 多边形 / 矩形）"));
        return;
    }
    if (app::testMode())
        return;
    const DrawingFeature *f = m_draw->feature(featureId);
    const QColor cur = (f && f->fill.isValid()) ? f->fill : QColor(255, 192, 0, 90);
    ui::FillDialog dlg(cur, m_dialogParent);
    if (dlg.exec() != QDialog::Accepted)
        return;
    m_draw->setFill(featureId, dlg.fillColor());
    emit errorMessage(QString());
}

// 选择工具双击：改颜色 / 线宽 / 填充透明度；圆还能改半径，文本还能改内容和字号。
// 走 DrawingManager::updateFeature，压一条 ModifyFeatureCommand，撤销一步就整体回滚。
void DrawingController::onDrawFeatureEdit(int featureId)
{
    const DrawingFeature *f = m_draw->feature(featureId);
    if (!f)
        return;
    if (app::testMode())
        return;                       // 自检直接调 DrawingManager，不弹窗
    ui::FeatureStyleDialog dlg(*f, m_dialogParent);
    if (dlg.exec() != QDialog::Accepted)
        return;

    DrawingFeature after = *f;
    after.color = dlg.color();
    after.widthPx = dlg.widthPx();
    const int opacity = dlg.fillOpacityPercent();   // -1 = 该图形没有填充项
    if (opacity == 0) {
        after.fill = QColor();
    } else if (opacity > 0) {
        QColor c = after.fill.isValid() ? after.fill : after.color;
        c.setAlphaF(opacity / 100.0);
        after.fill = c;
    }
    if (f->isCircle() && dlg.radiusKm() > 0.0)
        after.radiusKm = dlg.radiusKm();
    if (f->isText()) {
        if (dlg.text().isEmpty()) {
            emit errorMessage(tr("文本内容不能为空，未修改"));
            return;
        }
        after.text = dlg.text();
        after.textSizePx = dlg.textPx();
    }
    m_draw->updateFeature(after);
    emit errorMessage(QString());
}

void DrawingController::saveDrawings()
{
    if (m_draw->count() == 0) {
        emit errorMessage(tr("还没有绘制任何图形"));
        return;
    }
    const QString path = QFileDialog::getSaveFileName(
        m_dialogParent, tr("保存绘图"),
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("drawing.json")),
        tr("GeoJSON 绘图 (*.json);;所有文件 (*)"));
    if (path.isEmpty())
        return;
    QString err;
    if (!m_draw->saveToFile(path, &err)) {
        QMessageBox::warning(m_dialogParent, tr("保存绘图"), err);
        return;
    }
    emit errorMessage(QString());
    QMessageBox::information(m_dialogParent, tr("保存绘图"),
                             tr("已保存 %1 个图形到：\n%2").arg(m_draw->count()).arg(path));
}

void DrawingController::importDrawings()
{
    const QString path = QFileDialog::getOpenFileName(
        m_dialogParent, tr("导入绘图"), QCoreApplication::applicationDirPath(),
        tr("GeoJSON 绘图 (*.json);;所有文件 (*)"));
    if (path.isEmpty())
        return;
    QString err;
    if (!m_draw->loadFromFile(path, &err)) {
        QMessageBox::warning(m_dialogParent, tr("导入绘图"), err);
        return;
    }
    emit errorMessage(QString());
    QMessageBox::information(m_dialogParent, tr("导入绘图"), tr("已导入 %1 个图形").arg(m_draw->count()));
}

// 绘图内容自动保存：与数据库同目录的 drawings.json，原子写入（QSaveFile）
void DrawingController::saveAuto()
{
    if (m_restoringState)
        return;
    QSaveFile file(app::drawingPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    file.write(QJsonDocument(m_draw->toJson()).toJson(QJsonDocument::Compact));
    file.commit();
}

void DrawingController::loadAuto()
{
    QFile file(app::drawingPath());
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    if (root.isEmpty())
        return;
    const bool wasRestoring = m_restoringState;
    m_restoringState = true;          // fromJson 会发 changed()，别让它反过来再存一次
    QString err;
    m_draw->fromJson(root, &err);
    m_restoringState = wasRestoring;
    if (m_undo)
        m_undo->clear();              // 恢复出来的内容不进撤销栈
}

// 旗标右键「绘制距离圆」：弹窗（版式与标准色盘复用导入弹窗那套）→ 以旗标为圆心生成测地圆。
// 圆的顶点由 geocalc::geodesicCircle 按 Vincenty 正算逐方位角求得，是椭球上的等距圆。
void DrawingController::drawDistanceCircle(const QString &flagId)
{
    const int i = m_flags->indexOf(flagId);
    if (i < 0)
        return;
    const MapFlag f = m_flags->flags().at(i);
    double radiusKm = 100.0;
    QColor color(QStringLiteral("#FFC000"));
    if (!app::testMode()) {
        ui::DistanceCircleDialog dlg(f.label, f.lat, f.lon, color, m_dialogParent);
        if (dlg.exec() != QDialog::Accepted)
            return;
        radiusKm = dlg.radiusKm();
        color = dlg.color();
    }
    if (m_draw->addCircle(f.lat, f.lon, radiusKm, color, f.label) <= 0)
        QMessageBox::warning(m_dialogParent, tr("绘制距离圆"), tr("半径无效，未能生成距离圆。"));
}
