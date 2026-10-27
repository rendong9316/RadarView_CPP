#ifndef DRAWCOMMANDS_H
#define DRAWCOMMANDS_H

#include <QUndoCommand>
#include <QString>
#include <QVector>

#include "drawingmanager.h"

// 绘图的撤销/重做命令。DrawingManager 的每个改动都包成一条命令压进 QUndoStack，
// 主窗口只负责把栈交给它（DrawingManager::setUndoStack），其余调用方什么都不用改。
//   AddFeatureCommand     画图（直线/曲线/多边形/矩形/圆/文本）
//   RemoveFeaturesCommand 删除 / 框选删除 / 清空（一次可删一批）
//   ModifyFeatureCommand  填充、改颜色线宽、改文本、改半径
// 打断 / 裁剪是「删一个 + 加两个」，用 QUndoStack 的宏命令包起来，撤销仍然只算一步。
class AddFeatureCommand : public QUndoCommand
{
public:
    AddFeatureCommand(DrawingManager *mgr, const DrawingFeature &f, QUndoCommand *parent = nullptr);
    void undo() override;
    void redo() override;

private:
    DrawingManager *m_mgr;
    DrawingFeature m_feature;
    int m_index = -1;          // 首次插入的位置，重做时插回原处（保持叠放次序）
};

class RemoveFeaturesCommand : public QUndoCommand
{
public:
    RemoveFeaturesCommand(DrawingManager *mgr, const QVector<int> &indices,
                          const QVector<DrawingFeature> &features, const QString &text,
                          QUndoCommand *parent = nullptr);
    void undo() override;
    void redo() override;

private:
    DrawingManager *m_mgr;
    QVector<int> m_indices;            // 删除前的位置（升序）
    QVector<DrawingFeature> m_features;
};

class ModifyFeatureCommand : public QUndoCommand
{
public:
    ModifyFeatureCommand(DrawingManager *mgr, const DrawingFeature &before,
                         const DrawingFeature &after, const QString &text,
                         QUndoCommand *parent = nullptr);
    void undo() override;
    void redo() override;

private:
    DrawingManager *m_mgr;
    DrawingFeature m_before, m_after;
};

#endif // DRAWCOMMANDS_H
