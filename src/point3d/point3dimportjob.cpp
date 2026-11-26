#include "point3dimportjob.h"

#include "point3dparsers.h"

#include <QElapsedTimer>

Point3dImportJob::Point3dImportJob(const QStringList &paths, const Point3dImportOptions &options,
                                   QObject *parent)
    : QThread(parent), m_paths(paths), m_options(options)
{
    // 线程结束后在主线程通知（与 trackimporter.cpp 相同做法）
    connect(this, &QThread::finished, this, &Point3dImportJob::importDone, Qt::QueuedConnection);
}

void Point3dImportJob::run()
{
    QElapsedTimer clock;
    clock.start();

    int last = -1;
    const Point3dProgressFn progress = [this, &last](int pct) {
        if (pct != last) {
            last = pct;
            emit progressChanged(pct);
        }
    };
    const Point3dCancelFn cancelled = [this]() { return m_cancel.loadAcquire() != 0; };

    // 解析占 0..70%
    const bool parsed = parseDetPointFiles(
        m_paths, &m_data, &m_errors,
        [&progress](int pct) { progress(pct * 70 / 100); }, cancelled,
        m_options.detVariable, m_options.sysVariable, m_options.azimuthField, m_options.maxFrames);
    m_parsedFiles = m_data.frames.size();

    // 连续性评分占 70..100%
    if (parsed && !cancelled())
        computeContinuity(m_data.frames, m_options.continuity,
                          [&progress](int pct) { progress(70 + pct * 30 / 100); }, cancelled);

    m_elapsedMs = clock.elapsed();
}
