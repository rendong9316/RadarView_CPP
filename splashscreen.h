#ifndef SPLASHSCREEN_H
#define SPLASHSCREEN_H

#include <QElapsedTimer>
#include <QWidget>

class QTimer;

// 启动页：双击 exe 后立刻显示，主窗口画好第一帧后淡出。
// 全部用 QPainter 现画（不依赖图片资源、不碰 OpenGL），
// Windows 7 / RHEL 7.6 以及软件渲染下表现一致。
class SplashScreen : public QWidget
{
    Q_OBJECT
public:
    explicit SplashScreen(QWidget *parent = nullptr);

    // 显示并立刻画一帧：主窗口构造会阻塞事件循环，先把它刷出来
    void showNow();
    // 通知「主窗口已就绪」：过了最短展示时间就淡出并关闭
    void finish();

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    void tick();
    void startFade();

    QTimer *m_timer = nullptr;
    QElapsedTimer m_clock;
    bool m_ready = false;
    bool m_fading = false;
    int m_phase = 0;            // 进度条扫光动画的相位
};

#endif // SPLASHSCREEN_H
