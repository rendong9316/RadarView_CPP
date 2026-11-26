#ifndef POINT3DADVANCEDPANEL_H
#define POINT3DADVANCEDPANEL_H

#include <QScrollArea>

#include "point3dtypes.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QSpinBox;
class QTimer;

// 左侧「高级」参数页：所有控件改动即时写设置；连续性参数防抖 400ms 后通知重算。
class Point3dAdvancedPanel : public QScrollArea
{
    Q_OBJECT
public:
    explicit Point3dAdvancedPanel(QWidget *parent = nullptr);

    QSize minimumSizeHint() const override { return QSize(160, 160); }

    ContinuityConfig continuityConfig() const;

signals:
    void continuityConfigChanged(const ContinuityConfig &cfg);
    void overlayParamsChanged();
    void strongThresholdChanged(double threshold);

private:
    void buildUi();
    void connectSignals();
    void persistAdvanced();

    QLineEdit *m_detVarEdit = nullptr;
    QLineEdit *m_sysVarEdit = nullptr;
    QComboBox *m_azFieldCombo = nullptr;
    QLineEdit *m_trackVarEdit = nullptr;
    QLineEdit *m_patternEdit = nullptr;
    QSpinBox *m_maxFramesSpin = nullptr;
    QSpinBox *m_minTrackLenSpin = nullptr;
    QDoubleSpinBox *m_rangeScaleSpin = nullptr;
    QSpinBox *m_gapSpin = nullptr;
    QDoubleSpinBox *m_rangeCSpin = nullptr;
    QDoubleSpinBox *m_dopplerCSpin = nullptr;
    QDoubleSpinBox *m_azimuthCSpin = nullptr;
    QDoubleSpinBox *m_thresholdSpin = nullptr;
    QCheckBox *m_autoReloadCheck = nullptr;
    QTimer *m_advTimer = nullptr;
};

#endif // POINT3DADVANCEDPANEL_H
