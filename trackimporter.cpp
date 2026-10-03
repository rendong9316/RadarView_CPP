#include "trackimporter.h"
#include "trackparsers.h"

#include <QElapsedTimer>

TrackImporter::TrackImporter(TrackSource kind, const QString &path, const QString &displayName,
                             QObject *parent)
    : QThread(parent), m_kind(kind), m_path(path), m_displayName(displayName)
{
    // 线程结束后在主线程通知（QThread::finished 由工作线程发出，这里转成排队信号更直观）
    connect(this, &QThread::finished, this, &TrackImporter::importDone, Qt::QueuedConnection);
}

void TrackImporter::run()
{
    QElapsedTimer clock;
    clock.start();
    int last = -1;
    const ProgressFn progress = [this, &last](int pct) {
        if (pct != last) {
            last = pct;
            emit progressChanged(pct);   // 跨线程：自动排队到主线程
        }
    };
    if (m_kind == TrackSource::Adsb)
        m_ok = parseAdsbCsv(m_path, &m_tracks, &m_error, progress);
    else
        m_ok = parseRadarMat(m_path, m_kind == TrackSource::RadarRaw, m_displayName,
                             &m_tracks, &m_error, progress);
    m_elapsed = clock.elapsed();
}
