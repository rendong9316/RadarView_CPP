#ifndef TRACKIMPORTER_H
#define TRACKIMPORTER_H

#include <QThread>
#include <QVector>

#include "track.h"

// 后台解析一个数据文件；完成后在主线程发 importDone，由调用方把结果合并进 TrackStore
class TrackImporter : public QThread
{
    Q_OBJECT
public:
    TrackImporter(TrackSource kind, const QString &path, const QString &displayName,
                  QObject *parent = nullptr);

    TrackSource kind() const { return m_kind; }
    QString path() const { return m_path; }
    QString displayName() const { return m_displayName; }
    // 仅在 importDone 之后读取
    QVector<Track> &tracks() { return m_tracks; }
    QString errorString() const { return m_error; }
    bool ok() const { return m_ok; }
    qint64 elapsedMs() const { return m_elapsed; }

signals:
    void progressChanged(int percent);
    void importDone();

protected:
    void run() override;

private:
    TrackSource m_kind;
    QString m_path, m_displayName, m_error;
    QVector<Track> m_tracks;
    bool m_ok = false;
    qint64 m_elapsed = 0;
};

#endif // TRACKIMPORTER_H
