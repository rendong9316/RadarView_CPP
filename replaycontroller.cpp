#include "replaycontroller.h"

#include <cmath>

ReplayController::ReplayController(QObject *parent)
    : QObject(parent)
{
    m_timer.setInterval(16);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &ReplayController::onTick);
}

void ReplayController::setRange(qint64 startMs, qint64 endMs)
{
    setPlaying(false);
    m_start = startMs;
    m_end = qMax(startMs, endMs);
    m_current = m_start;
    m_active = false;
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
        emit stateChanged();
    }
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
