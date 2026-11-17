#ifndef POINT3DWINDOW_H
#define POINT3DWINDOW_H

#include <QElapsedTimer>
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
class QTimer;

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

    // 窗口自带的回放：只回放本窗口的点迹/航迹，按帧间隔逐帧推进（不插值、不做时间换算）
    bool isPlaying() const { return m_playing; }
    void togglePlay();                 // 到头就从头播
    void setSpeed(double speed);       // 倍速（帧间隔 / 倍速 = 每帧停留的真实毫秒）
    double speed() const { return m_speed; }
    void seekToFrame(int index);       // 拖动定位：吸附到整帧
    int currentFrame() const;
    int frameCount() const { return m_data.frames.size(); }
    int frameHoldMs() const;           // 当前帧应停留的真实毫秒（自检用）

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
    void setPlaying(bool on);
    void onPlayTick();
    void setFrameInternal(int index);
    void syncPlaybackUi();               // 把当前帧同步到进度条/时间标签
    void updatePlaybackUi();             // 播放/暂停按钮文字
    int nearestFrame(qint64 ms) const;
    void reloadOverlaysFromKeys();
    void startPendingLoad();

    Point3dCanvas *m_canvas = nullptr;
    QWidget *m_toolbar = nullptr;
    QWidget *m_statusBar = nullptr;
    QComboBox *m_viewCombo = nullptr;
    QComboBox *m_windowCombo = nullptr;
    QSpinBox *m_windowSpin = nullptr;
    QPushButton *m_importBtn = nullptr;
    QPushButton *m_sourceBtn = nullptr;   // 点源切换：弹起=平滑，按下=关联
    QPushButton *m_panBtn = nullptr;      // 拖拽方式：弹起=旋转，按下=平移（手爪）
    QPushButton *m_pointBtn = nullptr;
    QPushButton *m_trackBtn = nullptr;
    QPushButton *m_labelBtn = nullptr;
    QLabel *m_status = nullptr;
    QProgressBar *m_progress = nullptr;

    // 本窗口自带的回放（独立于主窗口时间轴）
    QPushButton *m_playBtn = nullptr;
    QSlider *m_seekSlider = nullptr;
    QComboBox *m_speedCombo = nullptr;
    QLabel *m_timeLabel = nullptr;
    QTimer *m_playTimer = nullptr;
    QElapsedTimer m_frameClock;          // 当前帧已经显示的真实时长
    double m_speed = 100.0;
    bool m_playing = false;
    bool m_resumeAfterSeek = false;
    bool m_syncing = false;

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
