#include "replaycontroller.h"
#include "apppaths.h"

#include <algorithm>
#include <cmath>

ReplayController::ReplayController(QObject *parent)
    : QObject(parent)
{
    m_timer.setInterval(16);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &ReplayController::onTick);

    // 倍速与尾迹时长跨会话保留（RadarView replay.speed / replay.trail_seconds）
    const double saved = app::settings().value(QStringLiteral("replay.speed"), m_speed).toDouble();
    if (saved > 0.0 && std::isfinite(saved))
        m_speed = saved;
    const double trail = app::settings().value(QStringLiteral("replay.trail_seconds"), 0.0).toDouble();
    if (trail >= 0.0 && std::isfinite(trail))
        m_trailSeconds = trail;
}

// 回放范围变化时**不轻易重置当前时刻**：地图显示集合变化（单独显示某条航迹、筛选、增删数据）
// 都会重算范围，若每次都跳回起点，点迹窗口会跟着跳到第一帧，看起来像"点迹全没了"。
// 规则：范围没变则什么都不做；范围变了但当前时刻仍在新范围内则原地保留，否则夹回起点。
void ReplayController::setRange(qint64 startMs, qint64 endMs)
{
    const qint64 newEnd = qMax(startMs, endMs);
    if (startMs == m_start && newEnd == m_end && m_current >= m_start && m_current <= m_end)
        return;
    const bool keepCurrent = m_end > m_start && m_current >= startMs && m_current <= newEnd;
    m_start = startMs;
    m_end = newEnd;
    m_current = keepCurrent ? m_current : m_start;
    if (!keepCurrent) {
        setPlaying(false);
        m_active = false;
    }
    emit stateChanged();
    emit timeChanged(m_current);
}

double ReplayController::progress() const
{
    return m_end > m_start ? double(m_current - m_start) / double(m_end - m_start) : 0.0;
}

void ReplayController::setSpeed(double speed)
{
    if (speed > 0.0 && std::isfinite(speed) && speed != m_speed) {
        m_speed = speed;
        app::setValue(QStringLiteral("replay.speed"), m_speed);
        emit stateChanged();
    }
}

void ReplayController::setTrailSeconds(double seconds)
{
    const double clamped = (std::isfinite(seconds) && seconds > 0.0) ? seconds : 0.0;
    if (qFuzzyCompare(clamped + 1.0, m_trailSeconds + 1.0))
        return;
    m_trailSeconds = clamped;
    app::setValue(QStringLiteral("replay.trail_seconds"), m_trailSeconds);
    emit trailSecondsChanged(m_trailSeconds);
    emit stateChanged();
}

void ReplayController::setLoopRange(bool enabled, qint64 aMs, qint64 bMs)
{
    const qint64 lo = qMin(aMs, bMs);
    const qint64 hi = qMax(aMs, bMs);
    m_loopEnabled = enabled && hi > lo;
    m_loopStart = m_loopEnabled ? qBound(m_start, lo, m_end) : 0;
    m_loopEnd = m_loopEnabled ? qBound(m_start, hi, m_end) : 0;
    if (m_loopEnd <= m_loopStart)
        m_loopEnabled = false;
    emit stateChanged();
}

void ReplayController::clearLoop()
{
    if (!m_loopEnabled)
        return;
    m_loopEnabled = false;
    m_loopStart = m_loopEnd = 0;
    emit stateChanged();
}

void ReplayController::setPlaying(bool p)
{
    if (p == m_playing)
        return;
    m_playing = p;
    if (p) {
        m_wall.start();
        m_lastWall = 0;
        m_timer.start();
    } else {
        m_timer.stop();
    }
}

void ReplayController::play()
{
    if (m_end <= m_start)
        return;
    if (m_current >= m_end)
        m_current = m_start;
    m_active = true;
    setPlaying(true);
    emit stateChanged();
    emit timeChanged(m_current);
}

void ReplayController::pause()
{
    setPlaying(false);
    emit stateChanged();
}

void ReplayController::togglePlay()
{
    if (m_playing)
        pause();
    else
        play();
}

void ReplayController::seek(double fraction)
{
    const double f = qBound(0.0, fraction, 1.0);
    seekTime(m_start + qint64(std::llround(f * double(m_end - m_start))));
}

void ReplayController::seekTime(qint64 ms)
{
    if (m_end <= m_start)
        return;
    setPlaying(false);
    m_current = qBound(m_start, ms, m_end);
    m_active = true;
    emit stateChanged();
    emit timeChanged(m_current);
}

void ReplayController::stepByMs(qint64 deltaMs)
{
    if (m_end <= m_start || deltaMs == 0)
        return;
    const qint64 base = m_active ? m_current : (deltaMs > 0 ? m_start : m_end);
    seekTime(qBound(m_start, base + deltaMs, m_end));
}

void ReplayController::stepToTimestamp(const QVector<qint64> &sortedTimes, int dir)
{
    if (m_end <= m_start || dir == 0)
        return;
    if (sortedTimes.isEmpty()) {
        stepByMs(dir > 0 ? 1000 : -1000);
        return;
    }
    const qint64 cur = m_active ? m_current : (dir > 0 ? m_start - 1 : m_end + 1);
    if (dir > 0) {
        auto it = std::upper_bound(sortedTimes.constBegin(), sortedTimes.constEnd(), cur);
        if (it != sortedTimes.constEnd())
            seekTime(*it);
        else
            seekTime(m_end);
    } else {
        auto it = std::lower_bound(sortedTimes.constBegin(), sortedTimes.constEnd(), cur);
        if (it != sortedTimes.constBegin())
            seekTime(*(it - 1));
        else
            seekTime(m_start);
    }
}

void ReplayController::stop()
{
    setPlaying(false);
    m_active = false;
    m_current = m_start;
    emit stateChanged();
    emit timeChanged(m_current);
}

void ReplayController::advance(qint64 wallMs)
{
    if (!m_active || wallMs <= 0)
        return;
    qint64 next = m_current + qint64(std::llround(double(wallMs) * m_speed));
    if (isLoopEnabled() && next >= m_loopEnd) {
        const qint64 span = qMax<qint64>(1, m_loopEnd - m_loopStart);
        m_current = m_loopStart + ((next - m_loopStart) % span);
        emit timeChanged(m_current);
        return;
    }
    if (next >= m_end) {
        // 放完：停在结尾并退出回放
        m_current = m_end;
        m_active = false;
        setPlaying(false);
        emit stateChanged();
        emit timeChanged(m_current);
        return;
    }
    m_current = next;
    emit timeChanged(m_current);
}

void ReplayController::onTick()
{
    const qint64 now = m_wall.elapsed();
    const qint64 dt = now - m_lastWall;
    m_lastWall = now;
    advance(qMin<qint64>(dt, 250));   // 窗口被拖动等造成的长停顿不一次性跳过太多
}
