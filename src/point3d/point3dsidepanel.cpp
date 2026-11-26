#include "point3dsidepanel.h"

#include "apppaths.h"
#include "point3dcanvas.h"
#include "theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <limits>

namespace {

QVariant setting(const char *key, const QVariant &def)
{
    return app::settings().value(QString::fromLatin1(key), def);
}

// 视图 -> 设置键后缀（三套区域筛选参数各存一份）
QString regionKey(int mode)
{
    return mode == int(Point3dViewMode::RAF) ? QStringLiteral("raf")
         : mode == int(Point3dViewMode::VAF) ? QStringLiteral("vaf")
                                             : QStringLiteral("rvf");
}

// 区域筛选三行的轴名（随视图模式变化）
void regionAxisTitles(Point3dViewMode mode, QString *x, QString *y, QString *z)
{
    switch (mode) {
    case Point3dViewMode::RVF:
        *x = QStringLiteral("X 距离 / m");
        *y = QStringLiteral("Y 速度 / (m/s)");
        break;
    case Point3dViewMode::RAF:
        *x = QStringLiteral("X 距离 / m");
        *y = QStringLiteral("Y 方位 / (°)");
        break;
    case Point3dViewMode::VAF:
        *x = QStringLiteral("X 速度 / (m/s)");
        *y = QStringLiteral("Y 方位 / (°)");
        break;
    }
    *z = QStringLiteral("Z 帧号");
}

} // namespace

Point3dSidePanel::Point3dSidePanel(Point3dCanvas *canvas, const Point3dDataset *data,
                                   QWidget *parent)
    : QScrollArea(parent), m_canvas(canvas), m_data(data)
{
    buildUi();
    connectSignals();
}

void Point3dSidePanel::buildUi()
{
    setObjectName(QStringLiteral("point3dSide"));
    setFrameShape(QFrame::NoFrame);
    setWidgetResizable(true);
    // 允许横向滚动：否则侧栏内容的最小宽度会顶到窗口最小宽度上，窗口又拉不窄了
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setFixedWidth(ui::sz(300));

    QWidget *page = new QWidget(this);
    page->setObjectName(QStringLiteral("point3dSidePage"));
    QVBoxLayout *lay = new QVBoxLayout(page);
    lay->setContentsMargins(ui::sz(8), ui::sz(8), ui::sz(8), ui::sz(8));
    lay->setSpacing(ui::sz(8));

    auto mkSpin = [](QWidget *parent, double lo, double hi, double value,
                     double step, int decimals) {
        QDoubleSpinBox *s = new QDoubleSpinBox(parent);
        s->setRange(lo, hi);
        s->setDecimals(decimals);
        s->setSingleStep(step);
        s->setValue(value);
        return s;
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

    // ---- 视图 ----
    {
        QFormLayout *f = addForm(QStringLiteral("视图"));
        m_viewCombo = new QComboBox(page);
        m_viewCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        m_viewCombo->setMinimumContentsLength(6);
        m_viewCombo->addItem(QStringLiteral("距离–速度–帧号"), int(Point3dViewMode::RVF));
        m_viewCombo->addItem(QStringLiteral("距离–方位–帧号"), int(Point3dViewMode::RAF));
        m_viewCombo->addItem(QStringLiteral("速度–方位–帧号"), int(Point3dViewMode::VAF));
        m_viewCombo->setToolTip(QStringLiteral("三维视图的 X/Y 轴组合，Z 轴恒为帧号"));
        f->addRow(QStringLiteral("三轴视图"), m_viewCombo);

        m_windowCombo = new QComboBox(page);
        m_windowCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        m_windowCombo->setMinimumContentsLength(4);
        const int presets[] = { 20, 40, 70, 150 };
        for (int v : presets)
            m_windowCombo->addItem(QStringLiteral("%1 帧").arg(v), v);
        m_windowCombo->addItem(QStringLiteral("全部"), 0);
        m_windowCombo->addItem(QStringLiteral("自定义…"), -1);
        m_windowCombo->setToolTip(QStringLiteral("同时显示最近多少帧（「全部」= 整个数据集）"));
        m_windowSpin = new QSpinBox(page);
        m_windowSpin->setRange(1, 100000);
        m_windowSpin->setValue(70);
        m_windowSpin->setVisible(false);
        m_windowSpin->setToolTip(QStringLiteral("自定义滑动窗口帧数"));
        QVBoxLayout *winCol = new QVBoxLayout;
        winCol->setContentsMargins(0, 0, 0, 0);
        winCol->setSpacing(ui::sz(4));
        winCol->addWidget(m_windowCombo);
        winCol->addWidget(m_windowSpin);
        f->addRow(QStringLiteral("滑动窗口"), winCol);
    }

    // ---- 显示 ----
    {
        QFormLayout *f = addForm(QStringLiteral("显示"));
        m_sourceBtn = new QPushButton(QStringLiteral("平滑"), page);
        m_sourceBtn->setCheckable(true);
        m_sourceBtn->setToolTip(QStringLiteral(
            "切换叠加航迹的点列：平滑（smoothPointList）↔ 关联（asscPointList）"));
        f->addRow(QStringLiteral("航迹点源"), m_sourceBtn);

        QHBoxLayout *row = new QHBoxLayout;
        row->setSpacing(ui::sz(4));
        m_pointBtn = new QPushButton(QStringLiteral("点迹 [P]"), page);
        m_pointBtn->setCheckable(true);
        m_pointBtn->setChecked(true);
        m_pointBtn->setToolTip(QStringLiteral("显示 / 隐藏点迹（P）"));
        m_trackBtn = new QPushButton(QStringLiteral("航迹 [T]"), page);
        m_trackBtn->setCheckable(true);
        m_trackBtn->setChecked(true);
        m_trackBtn->setToolTip(QStringLiteral("显示 / 隐藏航迹（T）"));
        m_labelBtn = new QPushButton(QStringLiteral("标签 [L]"), page);
        m_labelBtn->setCheckable(true);
        m_labelBtn->setChecked(true);
        m_labelBtn->setToolTip(QStringLiteral("显示 / 隐藏航迹编号（L）"));
        row->addWidget(m_pointBtn);
        row->addWidget(m_trackBtn);
        row->addWidget(m_labelBtn);
        f->addRow(QStringLiteral("图层"), row);
    }

    // ---- 尺寸 ----
    {
        QFormLayout *f = addForm(QStringLiteral("尺寸"));
        m_pointSizeSpin = mkSpin(page, 0.2, 5.0, 1.0, 0.1, 2);
        m_pointSizeSpin->setToolTip(QStringLiteral("点迹点的大小倍数（0.2–5）"));
        m_lineWidthSpin = mkSpin(page, 0.5, 12.0, 2.0, 0.1, 2);
        m_lineWidthSpin->setToolTip(QStringLiteral("叠加航迹的线宽（0.5–12）"));
        m_labelSizeSpin = mkSpin(page, 0.5, 3.0, 1.0, 0.1, 2);
        m_labelSizeSpin->setToolTip(QStringLiteral("航迹编号与坐标轴刻度数字的大小倍数（0.5–3）"));
        m_hoverSizeSpin = mkSpin(page, 0.5, 4.0, 1.0, 0.1, 2);
        m_hoverSizeSpin->setToolTip(QStringLiteral("鼠标悬停/点击信息浮窗的字号倍数（0.5–4）"));
        f->addRow(QStringLiteral("点迹尺寸"), m_pointSizeSpin);
        f->addRow(QStringLiteral("航迹线宽"), m_lineWidthSpin);
        f->addRow(QStringLiteral("标签大小"), m_labelSizeSpin);
        f->addRow(QStringLiteral("提示字号"), m_hoverSizeSpin);
    }

    // ---- 色标 SNR ----
    {
        QFormLayout *f = addForm(QStringLiteral("色标 SNR"));
        m_snrAuto = new QCheckBox(QStringLiteral("自动（按数据 min/max）"), page);
        m_snrLoSpin = mkSpin(page, -100.0, 300.0, 0.0, 0.5, 2);
        m_snrHiSpin = mkSpin(page, -100.0, 300.0, 30.0, 0.5, 2);
        f->addRow(m_snrAuto);
        f->addRow(QStringLiteral("下限 dB"), m_snrLoSpin);
        f->addRow(QStringLiteral("上限 dB"), m_snrHiSpin);
    }

    // ---- 区域筛选 ----
    {
        QGroupBox *g = new QGroupBox(QStringLiteral("区域筛选"), page);
        QVBoxLayout *gl = new QVBoxLayout(g);
        gl->setContentsMargins(ui::sz(8), ui::sz(10), ui::sz(8), ui::sz(8));
        gl->setSpacing(ui::sz(5));
        for (int i = 0; i < 3; ++i) {
            // 轴名一行、区间控件一行：侧栏不宽，挤一行会把滚动区撑出横向滚动条
            m_regionAxisLabel[i] = new QLabel(g);
            gl->addWidget(m_regionAxisLabel[i]);
            QHBoxLayout *row = new QHBoxLayout;
            row->setSpacing(ui::sz(4));
            m_regionMinSpin[i] = mkSpin(g, -1.0e7, 1.0e7, 0.0, 1.0, 2);
            m_regionMaxSpin[i] = mkSpin(g, -1.0e7, 1.0e7, 1.0, 1.0, 2);
            QPushButton *fullBtn = new QPushButton(QStringLiteral("满"), g);
            fullBtn->setToolTip(QStringLiteral("该轴设为满量程"));
            fullBtn->setFixedWidth(ui::sz(28));
            connect(fullBtn, &QPushButton::clicked, this, [this, i]() {
                const Point3dRegion ext = point3dRegionExtent(*m_data, m_canvas->viewMode());
                if (!ext.valid)
                    return;
                m_regionMinSpin[i]->setValue(i == 0 ? ext.x0 : i == 1 ? ext.y0 : ext.z0);
                m_regionMaxSpin[i]->setValue(i == 0 ? ext.x1 : i == 1 ? ext.y1 : ext.z1);
            });
            row->addWidget(m_regionMinSpin[i], 1);
            row->addWidget(new QLabel(QStringLiteral("至"), g));
            row->addWidget(m_regionMaxSpin[i], 1);
            row->addWidget(fullBtn);
            gl->addLayout(row);
        }
        QHBoxLayout *brow = new QHBoxLayout;
        brow->setSpacing(ui::sz(6));
        m_regionApplyBtn = new QPushButton(QStringLiteral("应用"), g);
        m_regionApplyBtn->setToolTip(QStringLiteral("只显示框内数据，并把视图放大到框内"));
        m_regionResetBtn = new QPushButton(QStringLiteral("重置"), g);
        m_regionResetBtn->setToolTip(QStringLiteral("清除区域筛选，恢复全量数据"));
        brow->addWidget(m_regionApplyBtn);
        brow->addWidget(m_regionResetBtn);
        brow->addStretch(1);
        gl->addLayout(brow);
        lay->addWidget(g);
    }

    lay->addStretch(1);
    setWidget(page);
}

void Point3dSidePanel::connectSignals()
{
    connect(m_sourceBtn, &QPushButton::toggled, this, [this](bool associated) {
        m_sourceBtn->setText(associated ? QStringLiteral("关联") : QStringLiteral("平滑"));
        m_canvas->setTrackSource(associated ? Point3dTrackSource::Associated
                                            : Point3dTrackSource::Smooth);
        if (!m_restoring)
            app::setValue(QStringLiteral("point3d.track_source"),
                          associated ? QStringLiteral("associated") : QStringLiteral("smooth"));
        emit statusChanged();
    });

    connect(m_pointSizeSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double v) {
                m_canvas->setPointScale(v);
                if (!m_restoring)
                    app::setValue(QStringLiteral("point3d.point_scale"), v);
            });
    connect(m_lineWidthSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double v) {
                m_canvas->setTrackLineWidth(v);
                if (!m_restoring)
                    app::setValue(QStringLiteral("point3d.track_line_width"), v);
            });
    connect(m_labelSizeSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double v) {
                m_canvas->setLabelScale(v);
                if (!m_restoring)
                    app::setValue(QStringLiteral("point3d.label_scale"), v);
            });
    connect(m_hoverSizeSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double v) {
                m_canvas->setHoverTextScale(v);
                if (!m_restoring)
                    app::setValue(QStringLiteral("point3d.hover_text_scale"), v);
            });

    connect(m_snrAuto, &QCheckBox::toggled, this, [this](bool on) {
        m_snrLoSpin->setEnabled(!on);
        m_snrHiSpin->setEnabled(!on);
        if (m_restoring)
            return;
        app::setValue(QStringLiteral("point3d.snr_auto"), on);
        applySnrRange();
    });
    auto snrEdited = [this](double) {
        if (m_restoring)
            return;
        app::setValue(QStringLiteral("point3d.snr_min"), m_snrLoSpin->value());
        app::setValue(QStringLiteral("point3d.snr_max"), m_snrHiSpin->value());
        applySnrRange();
    };
    connect(m_snrLoSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, snrEdited);
    connect(m_snrHiSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, snrEdited);

    for (int i = 0; i < 3; ++i) {
        connect(m_regionMinSpin[i], QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, &Point3dSidePanel::onRegionSpinEdited);
        connect(m_regionMaxSpin[i], QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, &Point3dSidePanel::onRegionSpinEdited);
    }
    connect(m_regionApplyBtn, &QPushButton::clicked, this, &Point3dSidePanel::applyRegion);
    connect(m_regionResetBtn, &QPushButton::clicked, this, &Point3dSidePanel::resetRegion);

    connect(m_viewCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &Point3dSidePanel::onViewModeChanged);
    connect(m_windowCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &Point3dSidePanel::onWindowLengthChanged);
    connect(m_windowSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int v) {
                if (!m_restoring)
                    applyWindowLength(v);
            });

    connect(m_pointBtn, &QPushButton::toggled, this, [this](bool on) {
        m_canvas->setShowDetections(on);
        if (!m_restoring) {
            app::setValue(QStringLiteral("point3d.show_detections"), on);
            emit statusChanged();
        }
    });
    connect(m_trackBtn, &QPushButton::toggled, this, [this](bool on) {
        m_canvas->setShowTracks(on);
        if (!m_restoring) {
            app::setValue(QStringLiteral("point3d.show_tracks"), on);
            emit statusChanged();
        }
    });
    connect(m_labelBtn, &QPushButton::toggled, this, [this](bool on) {
        m_canvas->setShowLabels(on);
        if (!m_restoring)
            app::setValue(QStringLiteral("point3d.show_labels"), on);
    });
}

void Point3dSidePanel::restoreDefaults()
{
    const bool prevRestoring = m_restoring;
    m_restoring = true;

    const QString view = setting("point3d.view_mode", QStringLiteral("rvf")).toString();
    const int viewIdx = view == QLatin1String("raf") ? 1 : view == QLatin1String("vaf") ? 2 : 0;
    m_viewCombo->setCurrentIndex(viewIdx);
    m_canvas->setViewMode(Point3dViewMode(m_viewCombo->currentData().toInt()));

    m_pointBtn->setChecked(setting("point3d.show_detections", true).toBool());
    m_trackBtn->setChecked(setting("point3d.show_tracks", true).toBool());
    m_labelBtn->setChecked(setting("point3d.show_labels", true).toBool());
    m_canvas->setShowDetections(m_pointBtn->isChecked());
    m_canvas->setShowTracks(m_trackBtn->isChecked());
    m_canvas->setShowLabels(m_labelBtn->isChecked());

    // 点源（平滑/关联）：按钮状态即设置值，按下时自动写回
    m_sourceBtn->setChecked(setting("point3d.track_source", QStringLiteral("smooth")).toString()
                            == QLatin1String("associated"));
    m_pointSizeSpin->setValue(setting("point3d.point_scale", 1.0).toDouble());
    m_lineWidthSpin->setValue(setting("point3d.track_line_width", 2.0).toDouble());
    m_labelSizeSpin->setValue(setting("point3d.label_scale", 1.0).toDouble());
    m_hoverSizeSpin->setValue(setting("point3d.hover_text_scale", 1.0).toDouble());

    // 色标 SNR 映射控件
    syncSnrControls();
    applySnrRange();

    // 滑动窗口
    setWindowLength(setting("point3d.window_length", 0).toInt());

    // 区域筛选：恢复三个视图各自的区间与是否已应用
    for (int i = 0; i < 3; ++i) {
        const QString key = QStringLiteral("point3d.region.%1").arg(regionKey(i));
        const QString joined = app::settings().value(key, QString()).toString();
        if (!joined.isEmpty()) {
            const QStringList parts = joined.split(QLatin1Char(','));
            if (parts.size() == 6) {
                m_viewRegions[i].region.valid = true;
                m_viewRegions[i].region.x0 = parts.at(0).toDouble();
                m_viewRegions[i].region.x1 = parts.at(1).toDouble();
                m_viewRegions[i].region.y0 = parts.at(2).toDouble();
                m_viewRegions[i].region.y1 = parts.at(3).toDouble();
                m_viewRegions[i].region.z0 = parts.at(4).toDouble();
                m_viewRegions[i].region.z1 = parts.at(5).toDouble();
            }
        }
        m_viewRegions[i].applied =
            app::settings().value(QStringLiteral("point3d.region_applied.%1").arg(regionKey(i)),
                                  false).toBool()
            && m_viewRegions[i].region.valid;
    }
    loadRegionForCurrentView();

    m_restoring = prevRestoring;
}

Point3dViewMode Point3dSidePanel::viewMode() const
{
    return Point3dViewMode(m_viewCombo->currentData().toInt());
}

void Point3dSidePanel::setViewMode(Point3dViewMode mode)
{
    const int idx = m_viewCombo->findData(int(mode));
    if (idx >= 0)
        m_viewCombo->setCurrentIndex(idx);
}

void Point3dSidePanel::setWindowLength(int len)
{
    int presetIdx = -1;
    for (int i = 0; i < m_windowCombo->count(); ++i)
        if (m_windowCombo->itemData(i).toInt() == len && len > 0) {
            presetIdx = i;
            break;
        }
    if (len == 0) {
        presetIdx = m_windowCombo->count() - 2;          // 全部
    } else if (presetIdx < 0) {
        presetIdx = m_windowCombo->count() - 1;          // 自定义…
        m_windowSpin->setValue(len);
        m_windowSpin->setVisible(true);
    }
    m_windowCombo->setCurrentIndex(presetIdx);
    m_canvas->setWindowLength(len);
}

void Point3dSidePanel::toggleTrackSource()
{
    m_sourceBtn->toggle();
}

void Point3dSidePanel::toggleDetections()
{
    m_pointBtn->toggle();
}

void Point3dSidePanel::toggleTracks()
{
    m_trackBtn->toggle();
}

void Point3dSidePanel::toggleLabels()
{
    m_labelBtn->toggle();
}

void Point3dSidePanel::syncSnrControls()
{
    m_snrAuto->setChecked(setting("point3d.snr_auto", true).toBool());
    m_snrLoSpin->setValue(setting("point3d.snr_min", 0.0).toDouble());
    m_snrHiSpin->setValue(setting("point3d.snr_max", 30.0).toDouble());
}

void Point3dSidePanel::applySnrRange()
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    if (setting("point3d.snr_auto", true).toBool()) {
        m_canvas->setSnrRange(nan, nan);
        return;
    }
    double lo = setting("point3d.snr_min", 0.0).toDouble();
    double hi = setting("point3d.snr_max", 30.0).toDouble();
    if (!(hi > lo))
        hi = lo + 1.0;
    m_canvas->setSnrRange(lo, hi);
}

void Point3dSidePanel::onViewModeChanged()
{
    const int mode = m_viewCombo->currentData().toInt();
    m_canvas->setViewMode(Point3dViewMode(mode));
    emit viewModeChanged(Point3dViewMode(mode));
    // 切视图：加载该视图自己的区域筛选（三行标签 + 数值 + 是否已应用）
    loadRegionForCurrentView();
    if (m_restoring)
        return;
    const char *key = mode == int(Point3dViewMode::RAF) ? "raf"
                    : mode == int(Point3dViewMode::VAF) ? "vaf" : "rvf";
    app::setValue(QStringLiteral("point3d.view_mode"), QString::fromLatin1(key));
    emit statusChanged();
}

void Point3dSidePanel::onWindowLengthChanged()
{
    const int data = m_windowCombo->currentData().toInt();
    if (data == -1) {
        m_windowSpin->setVisible(true);
        if (!m_restoring)
            applyWindowLength(m_windowSpin->value());
        return;
    }
    m_windowSpin->setVisible(false);
    if (!m_restoring)
        applyWindowLength(data);
}

void Point3dSidePanel::applyWindowLength(int len)
{
    m_canvas->setWindowLength(len);
    app::setValue(QStringLiteral("point3d.window_length"), len);
    emit statusChanged();
}

// ---------------------------------------------------------------------------
//  区域筛选：三行 = 当前视图的三个轴；每个视图各存一套，切换视图原样恢复
// ---------------------------------------------------------------------------
Point3dRegion Point3dSidePanel::regionForView(Point3dViewMode mode) const
{
    return m_viewRegions[int(mode)].region;
}

bool Point3dSidePanel::regionAppliedForView(Point3dViewMode mode) const
{
    return m_viewRegions[int(mode)].applied;
}

void Point3dSidePanel::loadRegionForCurrentView()
{
    const int vi = int(m_canvas->viewMode());
    ViewRegion &st = m_viewRegions[vi];
    if (!st.region.valid)
        st.region = point3dRegionExtent(*m_data, Point3dViewMode(vi));

    QString xl, yl, zl;
    regionAxisTitles(Point3dViewMode(vi), &xl, &yl, &zl);

    m_regionLoading = true;
    m_regionAxisLabel[0]->setText(xl);
    m_regionAxisLabel[1]->setText(yl);
    m_regionAxisLabel[2]->setText(zl);
    m_regionMinSpin[0]->setValue(st.region.x0);
    m_regionMaxSpin[0]->setValue(st.region.x1);
    m_regionMinSpin[1]->setValue(st.region.y0);
    m_regionMaxSpin[1]->setValue(st.region.y1);
    m_regionMinSpin[2]->setValue(st.region.z0);
    m_regionMaxSpin[2]->setValue(st.region.z1);
    m_regionLoading = false;

    m_canvas->setRegionFilter(st.applied ? st.region : Point3dRegion());
    m_canvas->setRegionBox(regionBoxVisible() ? st.region : Point3dRegion());
    updateRegionButtons();
    emit statusChanged();
}

void Point3dSidePanel::onRegionSpinEdited()
{
    if (m_restoring || m_regionLoading)
        return;
    const int vi = int(m_canvas->viewMode());
    ViewRegion &st = m_viewRegions[vi];
    st.region.valid = true;
    st.region.x0 = m_regionMinSpin[0]->value();
    st.region.x1 = m_regionMaxSpin[0]->value();
    st.region.y0 = m_regionMinSpin[1]->value();
    st.region.y1 = m_regionMaxSpin[1]->value();
    st.region.z0 = m_regionMinSpin[2]->value();
    st.region.z1 = m_regionMaxSpin[2]->value();

    // 已应用后再改数字：自动退回全量视图，只显示候选框（点「应用」才重新放大+裁剪）
    if (st.applied) {
        st.applied = false;
        m_canvas->setRegionFilter(Point3dRegion());
        app::setValue(QStringLiteral("point3d.region_applied.%1").arg(regionKey(vi)), false);
    }
    m_canvas->setRegionBox(regionBoxVisible() ? st.region : Point3dRegion());
    persistRegion(vi);
    updateRegionButtons();
    emit statusChanged();
}

void Point3dSidePanel::applyRegion()
{
    if (m_data->frames.isEmpty())
        return;
    const int vi = int(m_canvas->viewMode());
    ViewRegion &st = m_viewRegions[vi];
    Point3dRegion r = st.region;
    r.valid = true;
    const Point3dRegion ext = point3dRegionExtent(*m_data, Point3dViewMode(vi));
    if (ext.valid) {                        // 夹到数据量程内
        r.x0 = qMax(r.x0, ext.x0);
        r.x1 = qMin(r.x1, ext.x1);
        r.y0 = qMax(r.y0, ext.y0);
        r.y1 = qMin(r.y1, ext.y1);
        r.z0 = qMax(r.z0, ext.z0);
        r.z1 = qMin(r.z1, ext.z1);
    }
    if (!(r.x1 > r.x0) || !(r.y1 > r.y0) || !(r.z1 > r.z0)) {
        emit statusMessage(QStringLiteral("区域筛选：区间无效（每行下限必须小于上限）"));
        return;
    }
    st.region = r;
    st.applied = true;

    m_regionLoading = true;
    m_regionMinSpin[0]->setValue(r.x0);
    m_regionMaxSpin[0]->setValue(r.x1);
    m_regionMinSpin[1]->setValue(r.y0);
    m_regionMaxSpin[1]->setValue(r.y1);
    m_regionMinSpin[2]->setValue(r.z0);
    m_regionMaxSpin[2]->setValue(r.z1);
    m_regionLoading = false;

    m_canvas->setRegionBox(r);
    m_canvas->setRegionFilter(r);           // 归一化改为区间、数据裁剪 -> 视图放大到框内
    persistRegion(vi);
    updateRegionButtons();
    m_canvas->repaint();
    emit statusChanged();
    if (m_canvas->visiblePointCount() == 0)
        emit statusMessage(QStringLiteral("区域筛选：框内没有点迹，请放宽区间"));
}

void Point3dSidePanel::resetRegion()
{
    const int vi = int(m_canvas->viewMode());
    ViewRegion &st = m_viewRegions[vi];
    st.region = point3dRegionExtent(*m_data, Point3dViewMode(vi));
    st.applied = false;

    m_regionLoading = true;
    m_regionMinSpin[0]->setValue(st.region.x0);
    m_regionMaxSpin[0]->setValue(st.region.x1);
    m_regionMinSpin[1]->setValue(st.region.y0);
    m_regionMaxSpin[1]->setValue(st.region.y1);
    m_regionMinSpin[2]->setValue(st.region.z0);
    m_regionMaxSpin[2]->setValue(st.region.z1);
    m_regionLoading = false;

    m_canvas->setRegionFilter(Point3dRegion());
    m_canvas->setRegionBox(regionBoxVisible() ? st.region : Point3dRegion());
    persistRegion(vi);
    updateRegionButtons();
    emit statusChanged();
}

void Point3dSidePanel::persistRegion(int mode)
{
    const ViewRegion &st = m_viewRegions[mode];
    const QString key = QStringLiteral("point3d.region.%1").arg(regionKey(mode));
    if (st.region.valid) {
        app::setValue(key, QStringLiteral("%1,%2,%3,%4,%5,%6")
                               .arg(st.region.x0, 0, 'g', 12)
                               .arg(st.region.x1, 0, 'g', 12)
                               .arg(st.region.y0, 0, 'g', 12)
                               .arg(st.region.y1, 0, 'g', 12)
                               .arg(st.region.z0, 0, 'g', 12)
                               .arg(st.region.z1, 0, 'g', 12));
    } else {
        app::removeValue(key);
    }
    app::setValue(QStringLiteral("point3d.region_applied.%1").arg(regionKey(mode)), st.applied);
}

void Point3dSidePanel::revalidateRegions()
{
    for (int i = 0; i < 3; ++i) {
        ViewRegion &st = m_viewRegions[i];
        const Point3dRegion ext = point3dRegionExtent(*m_data, Point3dViewMode(i));
        if (!ext.valid)
            continue;
        if (!st.region.valid) {
            st.region = ext;
            st.applied = false;
            continue;
        }
        st.region.x0 = qBound(ext.x0, st.region.x0, ext.x1);
        st.region.x1 = qBound(ext.x0, st.region.x1, ext.x1);
        st.region.y0 = qBound(ext.y0, st.region.y0, ext.y1);
        st.region.y1 = qBound(ext.y0, st.region.y1, ext.y1);
        st.region.z0 = qBound(ext.z0, st.region.z0, ext.z1);
        st.region.z1 = qBound(ext.z0, st.region.z1, ext.z1);
        if (!(st.region.x1 > st.region.x0) || !(st.region.y1 > st.region.y0)
                || !(st.region.z1 > st.region.z0)) {
            st.region = ext;                 // 新数据完全不在原区间里：复位
            st.applied = false;
        }
        persistRegion(i);
    }
}

// 候选框只在"用户真的改过区间/已经应用"时显示；满量程时立方体本身就是边界，不用再描框
bool Point3dSidePanel::regionBoxVisible() const
{
    if (!m_panelActive)
        return false;
    const int vi = int(m_canvas->viewMode());
    const ViewRegion &st = m_viewRegions[vi];
    if (st.applied)
        return true;
    if (!st.region.valid)
        return false;
    const Point3dRegion ext = point3dRegionExtent(*m_data, Point3dViewMode(vi));
    if (!ext.valid)
        return false;
    const double eps = 1e-9;
    return st.region.x0 > ext.x0 + eps || st.region.x1 < ext.x1 - eps
        || st.region.y0 > ext.y0 + eps || st.region.y1 < ext.y1 - eps
        || st.region.z0 > ext.z0 + eps || st.region.z1 < ext.z1 - eps;
}

void Point3dSidePanel::updateRegionButtons()
{
    const bool haveData = !m_data->frames.isEmpty();
    m_regionApplyBtn->setEnabled(haveData);
    m_regionResetBtn->setEnabled(haveData && m_viewRegions[int(m_canvas->viewMode())].applied);
}

bool Point3dSidePanel::setRegionForCurrentView(const Point3dRegion &r, bool apply)
{
    const int vi = int(m_canvas->viewMode());
    ViewRegion &st = m_viewRegions[vi];
    st.region = r;
    st.region.valid = true;
    m_regionLoading = true;
    m_regionMinSpin[0]->setValue(r.x0);
    m_regionMaxSpin[0]->setValue(r.x1);
    m_regionMinSpin[1]->setValue(r.y0);
    m_regionMaxSpin[1]->setValue(r.y1);
    m_regionMinSpin[2]->setValue(r.z0);
    m_regionMaxSpin[2]->setValue(r.z1);
    m_regionLoading = false;
    if (!apply) {
        st.applied = false;
        m_canvas->setRegionFilter(Point3dRegion());
        m_canvas->setRegionBox(r);
        persistRegion(vi);
        updateRegionButtons();
        emit statusChanged();
        return true;
    }
    applyRegion();
    return st.applied;
}

QJsonArray Point3dSidePanel::regionsToJson() const
{
    QJsonArray regions;
    for (int i = 0; i < 3; ++i) {
        const ViewRegion &st = m_viewRegions[i];
        QJsonObject ro;
        ro.insert(QStringLiteral("valid"), st.region.valid);
        ro.insert(QStringLiteral("applied"), st.applied);
        ro.insert(QStringLiteral("x0"), st.region.x0);
        ro.insert(QStringLiteral("x1"), st.region.x1);
        ro.insert(QStringLiteral("y0"), st.region.y0);
        ro.insert(QStringLiteral("y1"), st.region.y1);
        ro.insert(QStringLiteral("z0"), st.region.z0);
        ro.insert(QStringLiteral("z1"), st.region.z1);
        regions.append(ro);
    }
    return regions;
}

void Point3dSidePanel::restoreRegionsFromJson(const QJsonArray &regions)
{
    for (int i = 0; i < 3 && i < regions.size(); ++i) {
        const QJsonObject ro = regions.at(i).toObject();
        m_viewRegions[i].region.valid = ro.value(QStringLiteral("valid")).toBool();
        m_viewRegions[i].applied = ro.value(QStringLiteral("applied")).toBool()
                                   && m_viewRegions[i].region.valid;
        m_viewRegions[i].region.x0 = ro.value(QStringLiteral("x0")).toDouble();
        m_viewRegions[i].region.x1 = ro.value(QStringLiteral("x1")).toDouble();
        m_viewRegions[i].region.y0 = ro.value(QStringLiteral("y0")).toDouble();
        m_viewRegions[i].region.y1 = ro.value(QStringLiteral("y1")).toDouble();
        m_viewRegions[i].region.z0 = ro.value(QStringLiteral("z0")).toDouble();
        m_viewRegions[i].region.z1 = ro.value(QStringLiteral("z1")).toDouble();
    }
}
