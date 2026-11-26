#include "point3dadvancedpanel.h"

#include "apppaths.h"
#include "theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace {

QVariant setting(const char *key, const QVariant &def)
{
    return app::settings().value(QString::fromLatin1(key), def);
}

} // namespace

Point3dAdvancedPanel::Point3dAdvancedPanel(QWidget *parent)
    : QScrollArea(parent)
{
    buildUi();
    connectSignals();
}

void Point3dAdvancedPanel::buildUi()
{
    setObjectName(QStringLiteral("point3dAdvanced"));
    setFrameShape(QFrame::NoFrame);
    setWidgetResizable(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    QWidget *page = new QWidget(this);
    page->setObjectName(QStringLiteral("point3dAdvancedPage"));
    QVBoxLayout *lay = new QVBoxLayout(page);
    lay->setContentsMargins(ui::sz(8), ui::sz(8), ui::sz(8), ui::sz(8));
    lay->setSpacing(ui::sz(8));

    auto mkLine = [&](const QString &text) {
        QLineEdit *e = new QLineEdit(page);
        e->setText(text);
        return e;
    };
    auto mkSpin = [&](int lo, int hi, int value) {
        QSpinBox *sp = new QSpinBox(page);
        sp->setRange(lo, hi);
        sp->setValue(value);
        return sp;
    };
    auto mkDSpin = [&](double lo, double hi, int decimals, double value, double step) {
        QDoubleSpinBox *sp = new QDoubleSpinBox(page);
        sp->setRange(lo, hi);
        sp->setDecimals(decimals);
        sp->setSingleStep(step);
        sp->setValue(value);
        return sp;
    };
    auto addForm = [&](const QString &title) {
        QGroupBox *g = new QGroupBox(title, page);
        QFormLayout *f = new QFormLayout(g);
        f->setContentsMargins(ui::sz(8), ui::sz(10), ui::sz(8), ui::sz(8));
        f->setSpacing(ui::sz(6));
        f->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        lay->addWidget(g);
        return f;
    };

    {
        QFormLayout *f = addForm(QStringLiteral("解析（下次导入生效）"));
        m_detVarEdit = mkLine(setting("point3d.det_variable", QStringLiteral("DetPoint")).toString());
        m_sysVarEdit = mkLine(setting("point3d.syspara_variable", QStringLiteral("sysPara")).toString());
        m_azFieldCombo = new QComboBox(page);
        m_azFieldCombo->setEditable(true);
        m_azFieldCombo->addItems(QStringList() << QStringLiteral("az") << QStringLiteral("theta"));
        m_azFieldCombo->setCurrentText(
            setting("point3d.azimuth_field", QStringLiteral("az")).toString());
        m_trackVarEdit = mkLine(setting("point3d.track_variable", QStringLiteral("trackList")).toString());
        m_patternEdit = mkLine(setting("point3d.file_pattern", QStringLiteral("detPoint_*.mat")).toString());
        m_maxFramesSpin = mkSpin(0, 1000000, setting("point3d.max_frames", 0).toInt());
        m_maxFramesSpin->setSpecialValueText(QStringLiteral("全部"));
        f->addRow(QStringLiteral("DetPoint 变量"), m_detVarEdit);
        f->addRow(QStringLiteral("sysPara 变量"), m_sysVarEdit);
        f->addRow(QStringLiteral("方位字段"), m_azFieldCombo);
        f->addRow(QStringLiteral("航迹变量"), m_trackVarEdit);
        f->addRow(QStringLiteral("文件匹配模式"), m_patternEdit);
        f->addRow(QStringLiteral("最多帧数"), m_maxFramesSpin);
    }
    {
        QFormLayout *f = addForm(QStringLiteral("航迹"));
        m_minTrackLenSpin = mkSpin(1, 1000, setting("point3d.min_track_length", 4).toInt());
        m_rangeScaleSpin = mkDSpin(0.001, 1000.0, 3,
                                   setting("point3d.track_range_scale", 1.0).toDouble(), 0.1);
        f->addRow(QStringLiteral("最小航迹长度"), m_minTrackLenSpin);
        f->addRow(QStringLiteral("航迹距离缩放"), m_rangeScaleSpin);
    }
    {
        QFormLayout *f = addForm(QStringLiteral("连续性评分"));
        m_gapSpin = mkSpin(1, 20, setting("point3d.continuity_frame_gap", 3).toInt());
        m_rangeCSpin = mkDSpin(0.1, 100000.0, 1,
                               setting("point3d.continuity_range_scale", 80.0).toDouble(), 1.0);
        m_dopplerCSpin = mkDSpin(0.1, 1000.0, 1,
                                 setting("point3d.continuity_doppler_scale", 10.0).toDouble(), 0.5);
        m_azimuthCSpin = mkDSpin(0.1, 180.0, 1,
                                 setting("point3d.continuity_azimuth_scale", 6.0).toDouble(), 0.5);
        m_thresholdSpin = mkDSpin(0.0, 1.0, 2,
                                  setting("point3d.strong_threshold", 0.70).toDouble(), 0.05);
        f->addRow(QStringLiteral("帧间隔"), m_gapSpin);
        f->addRow(QStringLiteral("距离尺度"), m_rangeCSpin);
        f->addRow(QStringLiteral("速度尺度"), m_dopplerCSpin);
        f->addRow(QStringLiteral("方位尺度"), m_azimuthCSpin);
        f->addRow(QStringLiteral("强连续阈值"), m_thresholdSpin);
    }
    m_autoReloadCheck = new QCheckBox(QStringLiteral("启动时恢复上次的窗口与数据"), page);
    m_autoReloadCheck->setChecked(setting("point3d.auto_reload", true).toBool());
    lay->addWidget(m_autoReloadCheck);

    QLabel *note = new QLabel(QStringLiteral(
        "改动即时生效并写入设置：\n"
        "· 最小航迹长度 / 距离缩放 / 强连续阈值 立即作用于当前画面；\n"
        "· 连续性参数（帧间隔/尺度）停止输入约 0.4 秒后自动重算；\n"
        "· 变量名、方位字段、文件匹配模式、最多帧数在下次导入时生效；\n"
        "· 「启动时恢复」在下次启动时生效。"), page);
    note->setWordWrap(true);
    lay->addWidget(note);

    lay->addStretch(1);
    setWidget(page);
}

void Point3dAdvancedPanel::connectSignals()
{
    // 高级页：即时持久化；连续性参数防抖 ~0.4s 后重算；航迹长度/距离缩放立即重建叠加
    m_advTimer = new QTimer(this);
    m_advTimer->setSingleShot(true);
    m_advTimer->setInterval(400);
    connect(m_advTimer, &QTimer::timeout, this, [this]() {
        emit continuityConfigChanged(continuityConfig());
    });

    auto advPersist = [this]() { persistAdvanced(); };
    connect(m_detVarEdit, &QLineEdit::textChanged, this, advPersist);
    connect(m_sysVarEdit, &QLineEdit::textChanged, this, advPersist);
    connect(m_trackVarEdit, &QLineEdit::textChanged, this, advPersist);
    connect(m_patternEdit, &QLineEdit::textChanged, this, advPersist);
    connect(m_azFieldCombo, &QComboBox::currentTextChanged, this, advPersist);
    connect(m_maxFramesSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, advPersist);
    connect(m_autoReloadCheck, &QCheckBox::toggled, this, advPersist);

    auto advContinuity = [this]() {
        persistAdvanced();
        m_advTimer->start();
    };
    connect(m_gapSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, advContinuity);
    connect(m_rangeCSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, advContinuity);
    connect(m_dopplerCSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, advContinuity);
    connect(m_azimuthCSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, advContinuity);

    // 最小航迹长度 / 距离缩放立即重建叠加航迹；强连续阈值只影响状态统计
    connect(m_minTrackLenSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) {
        persistAdvanced();
        emit overlayParamsChanged();
    });
    connect(m_rangeScaleSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) {
        persistAdvanced();
        emit overlayParamsChanged();
    });
    connect(m_thresholdSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) {
        persistAdvanced();
        emit strongThresholdChanged(v);
    });
}

ContinuityConfig Point3dAdvancedPanel::continuityConfig() const
{
    ContinuityConfig cfg;
    cfg.frameGap = m_gapSpin->value();
    cfg.rangeScale = m_rangeCSpin->value();
    cfg.dopplerScale = m_dopplerCSpin->value();
    cfg.azimuthScale = m_azimuthCSpin->value();
    return cfg;
}

void Point3dAdvancedPanel::persistAdvanced()
{
    if (!m_detVarEdit)
        return;
    app::setValue(QStringLiteral("point3d.det_variable"), m_detVarEdit->text().trimmed());
    app::setValue(QStringLiteral("point3d.syspara_variable"), m_sysVarEdit->text().trimmed());
    app::setValue(QStringLiteral("point3d.azimuth_field"), m_azFieldCombo->currentText().trimmed());
    app::setValue(QStringLiteral("point3d.track_variable"), m_trackVarEdit->text().trimmed());
    app::setValue(QStringLiteral("point3d.file_pattern"), m_patternEdit->text().trimmed());
    app::setValue(QStringLiteral("point3d.max_frames"), m_maxFramesSpin->value());
    app::setValue(QStringLiteral("point3d.min_track_length"), m_minTrackLenSpin->value());
    app::setValue(QStringLiteral("point3d.track_range_scale"), m_rangeScaleSpin->value());
    app::setValue(QStringLiteral("point3d.continuity_frame_gap"), m_gapSpin->value());
    app::setValue(QStringLiteral("point3d.continuity_range_scale"), m_rangeCSpin->value());
    app::setValue(QStringLiteral("point3d.continuity_doppler_scale"), m_dopplerCSpin->value());
    app::setValue(QStringLiteral("point3d.continuity_azimuth_scale"), m_azimuthCSpin->value());
    app::setValue(QStringLiteral("point3d.strong_threshold"), m_thresholdSpin->value());
    app::setValue(QStringLiteral("point3d.auto_reload"), m_autoReloadCheck->isChecked());
}
