#ifndef REPLAYCONTROLLER_H
#define REPLAYCONTROLLER_H

#include <QObject>
#include <QElapsedTimer>
#include <QTimer>
#include <QVector>

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
    double trailSeconds() const { return m_trailSeconds; }   // <= 0 表示全量历史
    void setTrailSeconds(double seconds);

    // 可选 A-B 循环区间（默认关闭，不改变原有到头退出行为）
    void setLoopRange(bool enabled, qint64 aMs, qint64 bMs);
    void clearLoop();
    bool isLoopEnabled() const { return m_loopEnabled && m_loopEnd > m_loopStart; }
    qint64 loopStart() const { return m_loopStart; }
    qint64 loopEnd() const { return m_loopEnd; }

    void play();
    void pause();
    void togglePlay();
    void seek(double fraction);                    // 0..1
    void seekTime(qint64 ms);
    void stepByMs(qint64 deltaMs);                 // 暂停并按毫秒前后步进
    void stepToTimestamp(const QVector<qint64> &sortedTimes, int dir);   // 按采样时刻单步前后跳
    void stop();                                   // 退出回放，回到起点

    // 推进 wallMs 毫秒的真实时间（定时器内部调用；自检可直接调用以获得确定结果）
    void advance(qint64 wallMs);

signals:
    void timeChanged(qint64 ms);
    void stateChanged();
    void trailSecondsChanged(double seconds);

private:
    void onTick();
    void setPlaying(bool p);

    qint64 m_start = 0, m_end = 0, m_current = 0;
    double m_speed = 500.0;
    double m_trailSeconds = 0.0;
    bool m_loopEnabled = false;
    qint64 m_loopStart = 0, m_loopEnd = 0;
    bool m_playing = false;
    bool m_active = false;
    QTimer m_timer;
    QElapsedTimer m_wall;
    qint64 m_lastWall = 0;
};

#endif // REPLAYCONTROLLER_H
