#include "drawcommands.h"

AddFeatureCommand::AddFeatureCommand(DrawingManager *mgr, const DrawingFeature &f, QUndoCommand *parent)
    : QUndoCommand(parent), m_mgr(mgr), m_feature(f)
{
    setText(QStringLiteral("绘制图形"));
}

void AddFeatureCommand::redo()
{
    if (!m_mgr)
        return;
    m_mgr->insertFeature(m_index < 0 ? m_mgr->count() : m_index, m_feature);
    if (m_index < 0)
        m_index = m_mgr->indexOf(m_feature.id);
}

void AddFeatureCommand::undo()
{
    if (m_mgr)
        m_mgr->takeFeature(m_feature.id);
}

RemoveFeaturesCommand::RemoveFeaturesCommand(DrawingManager *mgr, const QVector<int> &indices,
                                             const QVector<DrawingFeature> &features,
                                             const QString &text, QUndoCommand *parent)
    : QUndoCommand(parent), m_mgr(mgr), m_indices(indices), m_features(features)
{
    setText(text);
}

void RemoveFeaturesCommand::redo()
{
    if (!m_mgr)
        return;
    for (int i = m_features.size() - 1; i >= 0; --i)   // 从后往前删，下标不会串
        m_mgr->takeFeature(m_features.at(i).id);
}

void RemoveFeaturesCommand::undo()
{
    if (!m_mgr)
        return;
    for (int i = 0; i < m_features.size(); ++i)        // 从前往后插，位置逐个还原
        m_mgr->insertFeature(m_indices.value(i, m_mgr->count()), m_features.at(i));
}

ModifyFeatureCommand::ModifyFeatureCommand(DrawingManager *mgr, const DrawingFeature &before,
                                           const DrawingFeature &after, const QString &text,
                                           QUndoCommand *parent)
    : QUndoCommand(parent), m_mgr(mgr), m_before(before), m_after(after)
{
    setText(text);
}

void ModifyFeatureCommand::redo()
{
    if (m_mgr)
        m_mgr->replaceFeature(m_after);
}

void ModifyFeatureCommand::undo()
{
    if (m_mgr)
        m_mgr->replaceFeature(m_before);
}
