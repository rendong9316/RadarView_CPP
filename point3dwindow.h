#ifndef POINT3DWINDOW_H
#define POINT3DWINDOW_H

#include <QStringList>
#include <QWidget>
#include <QJsonObject>

#include <functional>

#include "point3dtypes.h"

class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSlider;
class QSpinBox;

class Point3dCanvas;
class Point3dImportJob;
class ReplayController;

// 点迹三维可视化独立窗口（可多开；每个窗口承载一组点迹数据）
class Point3dWindow : public QWidget
{
    Q_OBJECT
public:
    explicit Point3dWindow(QWidget *parent = nullptr);
    ~Point3dWindow() override;

    void setNumber(int n);
    int number() const { return m_number; }

    // 接入主窗口的全局回放时钟（所有点迹窗口共用一套时间轴）
    void attachReplay(ReplayController *replay);
    qint64 currentTimeMs() const;

    // 航迹颜色提供者：与主窗口地图上的文件颜色保持一致
    void setTrackColorProvider(const std::function<QColor(TrackSource, const QString &)> &provider);
    // 从数据库选择航迹（按 BatchNo 与主窗口联动）
    void chooseTracksFromDb();

    // 多窗口现场：保存 / 恢复本窗口的数据、航迹选择与几何
    QJsonObject sessionJson() const;
    void restoreSession(const QJsonObject &obj);

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
    void openAdvancedSettings();
    void exportCanvasImage();
    void onViewModeChanged();
    void onWindowLengthChanged();
    void applyWindowLength(int len);
    void setLoadingUi(bool loading, int percent);
    void updateStatus();
    void saveSettings();
    void onReplayTime(qint64 ms);
    void seekFromSlider(int value);
    void updatePlaybackUi();
    int nearestFrame(qint64 ms) const;
    void reloadOverlaysFromKeys();
    void startPendingLoad();

    Point3dCanvas *m_canvas = nullptr;
    QWidget *m_toolbar = nullptr;
    QWidget *m_statusBar = nullptr;
    QComboBox *m_viewCombo = nullptr;
    QComboBox *m_windowCombo = nullptr;
    QComboBox *m_sourceCombo = nullptr;
    QSpinBox *m_windowSpin = nullptr;
    QPushButton *m_importBtn = nullptr;
    QPushButton *m_pointBtn = nullptr;
    QPushButton *m_trackBtn = nullptr;
    QPushButton *m_labelBtn = nullptr;
    QLabel *m_status = nullptr;
    QProgressBar *m_progress = nullptr;

    // 播放控制（驱动主窗口的 ReplayController，全局共用时间轴）
    ReplayController *m_replay = nullptr;
    QPushButton *m_playBtn = nullptr;
    QSlider *m_seekSlider = nullptr;
    QComboBox *m_speedCombo = nullptr;
    QLabel *m_timeLabel = nullptr;
    bool m_syncing = false;
    bool m_outOfRange = false;

    Point3dDataset m_data;
    Point3dImportJob *m_job = nullptr;
    QStringList m_pendingFiles;
    QStringList m_files;
    QStringList m_trackKeys;      // 已选航迹的 Track::key()（从数据库选择）
    std::function<QColor(TrackSource, const QString &)> m_colorProvider;
    int m_number = 1;
    bool m_restoring = false;
};

#endif // POINT3DWINDOW_H
