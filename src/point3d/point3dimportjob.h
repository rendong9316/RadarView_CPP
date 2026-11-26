#ifndef POINT3DIMPORTJOB_H
#define POINT3DIMPORTJOB_H

#include <QAtomicInt>
#include <QStringList>
#include <QThread>

#include "point3dtypes.h"

// 后台批量解析点迹 MAT（仿 trackimporter：finished 以队列连接转 importDone）。
// 单文件失败不中断整批，错误收集在 errors()。
struct Point3dImportOptions {
    QString detVariable = QStringLiteral("DetPoint");
    QString sysVariable = QStringLiteral("sysPara");
    QString azimuthField = QStringLiteral("az");
    int maxFrames = 0;              // 0 = 全部
    ContinuityConfig continuity;    // 连续性评分参数
};

class Point3dImportJob : public QThread
{
    Q_OBJECT
public:
    explicit Point3dImportJob(const QStringList &paths, const Point3dImportOptions &options = Point3dImportOptions(),
                              QObject *parent = nullptr);

    void requestCancel() { m_cancel.storeRelease(1); }
    bool cancelled() const { return m_cancel.loadAcquire() != 0; }

    const Point3dDataset &dataset() const { return m_data; }
    const QStringList &errors() const { return m_errors; }
    qint64 elapsedMs() const { return m_elapsedMs; }
    int parsedFiles() const { return m_parsedFiles; }

signals:
    void progressChanged(int percent);
    void importDone();

protected:
    void run() override;

private:
    QStringList m_paths;
    Point3dImportOptions m_options;
    Point3dDataset m_data;
    QStringList m_errors;
    QAtomicInt m_cancel;
    qint64 m_elapsedMs = 0;
    int m_parsedFiles = 0;
};

#endif // POINT3DIMPORTJOB_H
