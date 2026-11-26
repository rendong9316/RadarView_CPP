#ifndef REPLAYCONTROLLER_H
#define REPLAYCONTROLLER_H

#include <QObject>
#include <QElapsedTimer>
#include <QTimer>

// 回放时钟：当前时刻按「真实流逝时间 × 倍速」推进（与 RadarView useReplay 一致）
// - play()：已到结尾则从头开始
// - seek()：暂停并跳到指定位置，回放保持激活
// - 到达结尾：自动暂停并退出回放，地图恢复显示全部航迹
class ReplayController : public QObject
{
    Q_OBJECT
public:
    explicit ReplayController(QObject *parent = nullptr);

    void setRange(qint64 startMs, qint64 endMs);   // 数据变化时调用，会复位
    qint64 start() const { return m_start; }
    qint64 end() const { return m_end; }
    qint64 current() const { return m_current; }
    double progress() const;                       // 0..1

    bool isPlaying() const { return m_playing; }
    bool isActive() const { return m_active; }     // 回放模式（含暂停）
    double speed() const { return m_speed; }
    void setSpeed(double speed);

    void play();
    void pause();
    void togglePlay();
    void seek(double fraction);                    // 0..1
    void seekTime(qint64 ms);
    void stop();                                   // 退出回放，回到起点

    // 推进 wallMs 毫秒的真实时间（定时器内部调用；自检可直接调用以获得确定结果）
    void advance(qint64 wallMs);

signals:
    void timeChanged(qint64 ms);
    void stateChanged();

private:
    void onTick();
    void setPlaying(bool p);

    qint64 m_start = 0, m_end = 0, m_current = 0;
    double m_speed = 500.0;
    bool m_playing = false;
    bool m_active = false;
    QTimer m_timer;
    QElapsedTimer m_wall;
    qint64 m_lastWall = 0;
};

#endif // REPLAYCONTROLLER_H
