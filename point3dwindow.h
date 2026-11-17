#ifndef POINT3DWINDOW_H
#define POINT3DWINDOW_H

#include <QStringList>
#include <QWidget>

#include "point3dtypes.h"

class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;

class Point3dCanvas;
class Point3dImportJob;

// 点迹三维可视化独立窗口（可多开；每个窗口承载一组点迹数据）
class Point3dWindow : public QWidget
{
    Q_OBJECT
public:
    explicit Point3dWindow(QWidget *parent = nullptr);
    ~Point3dWindow() override;

    void setNumber(int n);
    int number() const { return m_number; }

    Point3dCanvas *canvas() const { return m_canvas; }
    const Point3dDataset *dataset() const { return &m_data; }
    qint64 minTime() const { return m_data.minTime; }
    qint64 maxTime() const { return m_data.maxTime; }
    bool isLoading() const { return m_job != nullptr; }
    void cancelImport();
    void loadDetectionFiles(const QStringList &paths);
    QStringList importedFiles() const { return m_files; }

    void setHighlightedBatch(int batchNo);
    int highlightedBatch() const;
    int currentFrame() const;
    void setCurrentFrame(int index);

signals:
    void closed();
    void dataChanged();
    void trackFocusChanged(int batchNo, TrackSource source, qint64 t0, qint64 t1);
    void trackActivated(int batchNo, TrackSource source, qint64 t0, qint64 t1);

protected:
    void closeEvent(QCloseEvent *e) override;

private:
    void buildUi();
    void importDetections();
    void onImportDone();
    void onViewModeChanged();
    void onWindowLengthChanged();
    void applyWindowLength(int len);
    void setLoadingUi(bool loading, int percent);
    void updateStatus();
    void saveSettings();

    Point3dCanvas *m_canvas = nullptr;
    QWidget *m_toolbar = nullptr;
    QWidget *m_statusBar = nullptr;
    QComboBox *m_viewCombo = nullptr;
    QComboBox *m_windowCombo = nullptr;
    QSpinBox *m_windowSpin = nullptr;
    QPushButton *m_importBtn = nullptr;
    QPushButton *m_pointBtn = nullptr;
    QPushButton *m_trackBtn = nullptr;
    QPushButton *m_labelBtn = nullptr;
    QLabel *m_status = nullptr;
    QProgressBar *m_progress = nullptr;

    Point3dDataset m_data;
    Point3dImportJob *m_job = nullptr;
    QStringList m_pendingFiles;
    QStringList m_files;
    int m_number = 1;
    bool m_restoring = false;
};

#endif // POINT3DWINDOW_H
