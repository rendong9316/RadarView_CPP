#include "replaybar.h"
#include "replaycontroller.h"
#include "track.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QDoubleValidator>

namespace {

const int kSliderMax = 10000;

// 点击轨道直接跳到点击处（QSlider 默认是按页步进）
class SeekSlider : public QSlider
{
public:
    explicit SeekSlider(QWidget *parent) : QSlider(Qt::Horizontal, parent) {}

protected:
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton) {
            QStyleOptionSlider opt;
            initStyleOption(&opt);
            const QRect handle = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
            if (!handle.contains(e->pos())) {
                const int span = width() - handle.width();
                const int v = QStyle::sliderValueFromPosition(minimum(), maximum(),
                                                              e->pos().x() - handle.width() / 2, qMax(1, span));
                setSliderDown(true);
                setValue(v);
                triggerAction(SliderMove);
                setSliderDown(false);
                emit sliderReleased();
            }
        }
        QSlider::mousePressEvent(e);
    }
};

} // namespace

ReplayBar::ReplayBar(ReplayController *ctrl, QWidget *parent)
    : QWidget(parent), m_ctrl(ctrl)
{
    QHBoxLayout *lay = new QHBoxLayout(this);
    lay->setContentsMargins(4, 0, 4, 0);
    lay->setSpacing(8);

    m_play = new QPushButton(this);
    m_play->setFixedSize(28, 22);
    m_play->setFocusPolicy(Qt::NoFocus);
    m_play->setToolTip(tr("播放 / 暂停（空格）"));
    lay->addWidget(m_play);

    m_slider = new SeekSlider(this);
    m_slider->setRange(0, kSliderMax);
    m_slider->setFixedWidth(300);
    m_slider->setFocusPolicy(Qt::NoFocus);
    lay->addWidget(m_slider);

    m_time = new QLabel(this);
    m_time->setStyleSheet(QStringLiteral("font-family: Consolas, 'Cascadia Code', monospace;"));
    lay->addWidget(m_time);

    m_speed = new QComboBox(this);
    m_speed->setEditable(true);
    m_speed->setInsertPolicy(QComboBox::NoInsert);
    for (int s : { 1, 10, 50, 100, 300, 500, 2000 })
        m_speed->addItem(QStringLiteral("%1x").arg(s), s);
    m_speed->setFixedWidth(84);
    m_speed->setToolTip(tr("回放倍速（可输入自定义值）"));
    lay->addWidget(m_speed);

    connect(m_play, &QPushButton::clicked, m_ctrl, &ReplayController::togglePlay);
    connect(m_slider, &QSlider::sliderPressed, this, [this]() { m_dragging = true; });
    connect(m_slider, &QSlider::sliderMoved, this, [this](int v) { m_ctrl->seek(double(v) / kSliderMax); });
    connect(m_slider, &QSlider::sliderReleased, this, [this]() {
        m_dragging = false;
        m_ctrl->seek(double(m_slider->value()) / kSliderMax);
    });
    connect(m_speed, QOverload<int>::of(&QComboBox::activated), this, [this](int) { applySpeedText(); });
    connect(m_speed->lineEdit(), &QLineEdit::editingFinished, this, &ReplayBar::applySpeedText);
    connect(m_ctrl, &ReplayController::stateChanged, this, &ReplayBar::syncState);
    connect(m_ctrl, &ReplayController::timeChanged, this, &ReplayBar::syncTime);

    syncState();
    syncTime(m_ctrl->current());
}

void ReplayBar::applySpeedText()
{
    QString s = m_speed->currentText().trimmed();
    if (s.endsWith(QLatin1Char('x'), Qt::CaseInsensitive))
        s.chop(1);
    bool ok = false;
    const double v = s.toDouble(&ok);
    if (ok && v > 0.0)
        m_ctrl->setSpeed(v);
    m_speed->setEditText(QStringLiteral("%1x").arg(m_ctrl->speed()));
}

void ReplayBar::syncState()
{
    const bool hasData = m_ctrl->end() > m_ctrl->start();
    setEnabled(hasData);
    m_play->setIcon(style()->standardIcon(m_ctrl->isPlaying() ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    if (!m_speed->lineEdit()->hasFocus())
        m_speed->setEditText(QStringLiteral("%1x").arg(m_ctrl->speed()));
}

void ReplayBar::syncTime(qint64 ms)
{
    if (!m_dragging) {
        const QSignalBlocker block(m_slider);
        m_slider->setValue(int(m_ctrl->progress() * kSliderMax));
    }
    if (m_ctrl->end() > m_ctrl->start())
        m_time->setText(formatBeijingTime(ms) + QStringLiteral(" / ") + formatBeijingTime(m_ctrl->end()));
    else
        m_time->setText(tr("-- / --"));
}
