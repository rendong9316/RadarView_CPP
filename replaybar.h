#ifndef REPLAYBAR_H
#define REPLAYBAR_H

#include <QWidget>

class QPushButton;
class QSlider;
class QLabel;
class QComboBox;
class ReplayController;

// 状态栏里的回放控件：播放/暂停、进度条（点击/拖动跳转）、当前/结束时间、倍速
class ReplayBar : public QWidget
{
    Q_OBJECT
public:
    explicit ReplayBar(ReplayController *ctrl, QWidget *parent = nullptr);

private:
    void syncState();
    void syncTime(qint64 ms);
    void applySpeedText();

    ReplayController *m_ctrl;
    QPushButton *m_play;
    QSlider *m_slider;
    QLabel *m_time;
    QComboBox *m_speed;
    bool m_dragging = false;
};

#endif // REPLAYBAR_H
