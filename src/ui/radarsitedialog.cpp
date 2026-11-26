#include "radarsitedialog.h"
#include "theme.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <cmath>

RadarSiteDialog::RadarSiteDialog(RadarSiteStore *store, QWidget *parent)
    : QDialog(parent)
    , m_store(store)
{
    setWindowTitle(QStringLiteral("雷达站址与 PPI 极坐标网格管理"));
    resize(780, 560);

    QHBoxLayout *main = new QHBoxLayout;
    ui::bindSpacing(main, 12);

    // 左侧站点列表
    QVBoxLayout *left = new QVBoxLayout;
    left->addWidget(new QLabel(QStringLiteral("已配置的雷达站（自动从 MAT sysPara 提取或手动添加）："), this));
    m_list = new QListWidget(this);
    m_list->setMinimumWidth(240);
    left->addWidget(m_list, 1);

    QHBoxLayout *listBtns = new QHBoxLayout;
    QPushButton *addBtn = new QPushButton(QStringLiteral("新增"), this);
    QPushButton *delBtn = new QPushButton(QStringLiteral("删除"), this);
    QPushButton *actBtn = new QPushButton(QStringLiteral("设为参考站"), this);
    QPushButton *flyBtn = new QPushButton(QStringLiteral("飞至站点"), this);
    listBtns->addWidget(addBtn);
    listBtns->addWidget(delBtn);
    listBtns->addWidget(actBtn);
    listBtns->addWidget(flyBtn);
    left->addLayout(listBtns);
    main->addLayout(left, 2);

    // 右侧属性编辑区
    QVBoxLayout *right = new QVBoxLayout;
    auto mkSpin = [this](double lo, double hi, int dec, double step, const QString &suffix = QString()) {
        QDoubleSpinBox *sp = new QDoubleSpinBox(this);
        sp->setRange(lo, hi);
        sp->setDecimals(dec);
        sp->setSingleStep(step);
        if (!suffix.isEmpty())
            sp->setSuffix(suffix);
        return sp;
    };

    QGroupBox *gBasic = new QGroupBox(QStringLiteral("站址信息（WGS84）"), this);
    QFormLayout *fb = new QFormLayout(gBasic);
    m_nameEdit = new QLineEdit(gBasic);
    m_visibleChk = new QCheckBox(QStringLiteral("在地图显示该站与 PPI 网格"), gBasic);
    fb->addRow(QStringLiteral("站点名称:"), m_nameEdit);
    fb->addRow(QString(), m_visibleChk);

    m_hasRxChk = new QCheckBox(QStringLiteral("接收站 Rx"), gBasic);
    m_rxLat = mkSpin(-90.0, 90.0, 6, 0.01, QStringLiteral(" °"));
    m_rxLon = mkSpin(-180.0, 180.0, 6, 0.01, QStringLiteral(" °"));
    QHBoxLayout *rxRow = new QHBoxLayout;
    rxRow->addWidget(new QLabel(QStringLiteral("纬"), gBasic));
    rxRow->addWidget(m_rxLat, 1);
    rxRow->addWidget(new QLabel(QStringLiteral("经"), gBasic));
    rxRow->addWidget(m_rxLon, 1);
    fb->addRow(m_hasRxChk, rxRow);

    m_hasTxChk = new QCheckBox(QStringLiteral("发射站 Tx"), gBasic);
    m_txLat = mkSpin(-90.0, 90.0, 6, 0.01, QStringLiteral(" °"));
    m_txLon = mkSpin(-180.0, 180.0, 6, 0.01, QStringLiteral(" °"));
    QHBoxLayout *txRow = new QHBoxLayout;
    txRow->addWidget(new QLabel(QStringLiteral("纬"), gBasic));
    txRow->addWidget(m_txLat, 1);
    txRow->addWidget(new QLabel(QStringLiteral("经"), gBasic));
    txRow->addWidget(m_txLon, 1);
    fb->addRow(m_hasTxChk, txRow);
    right->addWidget(gBasic);

    QGroupBox *gSec = new QGroupBox(QStringLiteral("探测扇区与 PPI 网格"), this);
    QFormLayout *fs = new QFormLayout(gSec);
    m_hasSectorChk = new QCheckBox(QStringLiteral("启用探测扇区范围"), gSec);
    fs->addRow(QString(), m_hasSectorChk);

    m_minR = mkSpin(0.0, 15000.0, 1, 50.0, QStringLiteral(" km"));
    m_maxR = mkSpin(0.0, 15000.0, 1, 50.0, QStringLiteral(" km"));
    QHBoxLayout *rRow = new QHBoxLayout;
    rRow->addWidget(m_minR, 1);
    rRow->addWidget(new QLabel(QStringLiteral("~"), gSec));
    rRow->addWidget(m_maxR, 1);
    fs->addRow(QStringLiteral("距离范围:"), rRow);

    m_startAz = mkSpin(-360.0, 360.0, 1, 5.0, QStringLiteral(" °"));
    m_endAz = mkSpin(-360.0, 360.0, 1, 5.0, QStringLiteral(" °"));
    QHBoxLayout *azRow = new QHBoxLayout;
    azRow->addWidget(m_startAz, 1);
    azRow->addWidget(new QLabel(QStringLiteral("~"), gSec));
    azRow->addWidget(m_endAz, 1);
    fs->addRow(QStringLiteral("方位扇区:"), azRow);

    m_beamDir = mkSpin(-1.0, 360.0, 1, 5.0, QStringLiteral(" °"));
    m_beamDir->setSpecialValueText(QStringLiteral("无"));
    fs->addRow(QStringLiteral("波束中心:"), m_beamDir);

    m_ringStep = mkSpin(0.0, 5000.0, 0, 50.0, QStringLiteral(" km"));
    m_ringStep->setSpecialValueText(QStringLiteral("自动"));
    m_azStep = mkSpin(1.0, 90.0, 1, 5.0, QStringLiteral(" °"));
    QHBoxLayout *gridRow = new QHBoxLayout;
    gridRow->addWidget(new QLabel(QStringLiteral("距离环"), gSec));
    gridRow->addWidget(m_ringStep, 1);
    gridRow->addWidget(new QLabel(QStringLiteral("方位线"), gSec));
    gridRow->addWidget(m_azStep, 1);
    fs->addRow(QStringLiteral("网格间隔:"), gridRow);

    m_anchorCombo = new QComboBox(gSec);
    m_anchorCombo->addItem(QStringLiteral("接收站 Rx（默认）"), 0);
    m_anchorCombo->addItem(QStringLiteral("发射站 Tx"), 1);
    m_anchorCombo->addItem(QStringLiteral("收发中点"), 2);
    fs->addRow(QStringLiteral("测距参考基准:"), m_anchorCombo);

    m_fullRingsChk = new QCheckBox(QStringLiteral("绘制 360° 整圆距离环（不勾选则仅画扇区弧段）"), gSec);
    fs->addRow(QString(), m_fullRingsChk);

    m_colorBtn = new QPushButton(QStringLiteral("选择颜色..."), gSec);
    fs->addRow(QStringLiteral("网格颜色:"), m_colorBtn);
    right->addWidget(gSec);

    QHBoxLayout *saveRow = new QHBoxLayout;
    QPushButton *applyBtn = new QPushButton(QStringLiteral("保存修改"), this);
    saveRow->addStretch(1);
    saveRow->addWidget(applyBtn);
    right->addLayout(saveRow);
    right->addStretch(1);
    main->addLayout(right, 3);

    QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
    bb->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);

    QVBoxLayout *root = new QVBoxLayout(this);
    ui::bindMargins(root, 12, 12, 12, 12);
    ui::bindSpacing(root, 10);
    root->addLayout(main, 1);
    root->addWidget(bb);

    setThemedStyle(this, ui::dialogQss() + QStringLiteral(
        "QGroupBox { color: var(--text-primary); border: 1px solid var(--border-secondary);"
        " border-radius: 4px; margin-top: 10px; padding-top: 6px; font-weight: 600; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; color: var(--text-secondary); }"
        "QListWidget { background: var(--bg-secondary); color: var(--text-primary); border: 1px solid var(--border-primary); border-radius: 4px; }"
        "QLineEdit, QDoubleSpinBox, QComboBox { background: var(--input-bg); color: var(--input-fg);"
        " border: 1px solid var(--input-border); border-radius: 4px; padding: 3px 6px; }"
        "QPushButton { background: var(--button-bg); color: var(--button-fg); border: 1px solid var(--border-primary);"
        " border-radius: 4px; padding: 4px 10px; }"
        "QPushButton:hover { background: var(--button-hover); color: var(--text-primary); }"));

    connect(m_list, &QListWidget::currentRowChanged, this, [this](int) {
        if (!m_syncing)
            loadSelectedIntoForm();
    });
    connect(addBtn, &QPushButton::clicked, this, &RadarSiteDialog::onAddSite);
    connect(delBtn, &QPushButton::clicked, this, &RadarSiteDialog::onRemoveSite);
    connect(actBtn, &QPushButton::clicked, this, &RadarSiteDialog::onSetActive);
    connect(flyBtn, &QPushButton::clicked, this, &RadarSiteDialog::onFlyTo);
    connect(applyBtn, &QPushButton::clicked, this, &RadarSiteDialog::saveFormToCurrent);
    connect(m_colorBtn, &QPushButton::clicked, this, [this]() {
        const QColor c = QColorDialog::getColor(m_color, this, QStringLiteral("选择 PPI 网格颜色"));
        if (c.isValid()) {
            m_color = c;
            updateColorBtn();
        }
    });
    if (m_store)
        connect(m_store, &RadarSiteStore::changed, this, &RadarSiteDialog::rebuildList);

    rebuildList();
}

void RadarSiteDialog::updateColorBtn()
{
    m_colorBtn->setStyleSheet(QStringLiteral("background: %1; color: #000; font-weight: 600;").arg(m_color.name()));
    m_colorBtn->setText(m_color.name());
}

void RadarSiteDialog::rebuildList()
{
    m_syncing = true;
    m_list->clear();
    if (m_store) {
        const QString activeId = m_store->activeSiteId();
        int targetRow = 0;
        const QVector<RadarSite> &sites = m_store->sites();
        for (int i = 0; i < sites.size(); ++i) {
            const RadarSite &s = sites[i];
            QString label = s.name;
            if (s.id == activeId)
                label += QStringLiteral("  [参考站]");
            if (!s.visible)
                label += QStringLiteral("  (隐)");
            QListWidgetItem *it = new QListWidgetItem(label, m_list);
            it->setData(Qt::UserRole, s.id);
            if (s.id == m_currentId)
                targetRow = i;
        }
        if (m_list->count() > 0)
            m_list->setCurrentRow(qBound(0, targetRow, m_list->count() - 1));
    }
    m_syncing = false;
    loadSelectedIntoForm();
}

void RadarSiteDialog::loadSelectedIntoForm()
{
    QListWidgetItem *it = m_list->currentItem();
    const RadarSite *s = (it && m_store) ? m_store->site(it->data(Qt::UserRole).toString()) : nullptr;
    m_currentId = s ? s->id : QString();
    if (!s) {
        m_nameEdit->clear();
        return;
    }
    m_nameEdit->setText(s->name);
    m_visibleChk->setChecked(s->visible);
    m_hasRxChk->setChecked(s->hasRx);
    m_rxLat->setValue(s->rxLat);
    m_rxLon->setValue(s->rxLon);
    m_hasTxChk->setChecked(s->hasTx);
    m_txLat->setValue(s->txLat);
    m_txLon->setValue(s->txLon);
    m_hasSectorChk->setChecked(s->hasSector);
    m_minR->setValue(s->minRangeKm);
    m_maxR->setValue(s->maxRangeKm);
    m_startAz->setValue(s->startAzDeg);
    m_endAz->setValue(s->endAzDeg);
    m_beamDir->setValue(std::isfinite(s->beamDirDeg) ? s->beamDirDeg : -1.0);
    m_ringStep->setValue(s->ringStepKm);
    m_azStep->setValue(s->azStepDeg);
    m_fullRingsChk->setChecked(s->showFullRings);
    m_anchorCombo->setCurrentIndex(qBound(0, s->refAnchor, 2));
    m_color = s->color;
    updateColorBtn();
}

void RadarSiteDialog::saveFormToCurrent()
{
    if (!m_store || m_currentId.isEmpty())
        return;
    const RadarSite *orig = m_store->site(m_currentId);
    if (!orig)
        return;
    RadarSite s = *orig;
    s.name = m_nameEdit->text().trimmed().isEmpty() ? s.name : m_nameEdit->text().trimmed();
    s.visible = m_visibleChk->isChecked();
    s.hasRx = m_hasRxChk->isChecked();
    s.rxLat = m_rxLat->value();
    s.rxLon = m_rxLon->value();
    s.hasTx = m_hasTxChk->isChecked();
    s.txLat = m_txLat->value();
    s.txLon = m_txLon->value();
    s.hasSector = m_hasSectorChk->isChecked();
    s.minRangeKm = m_minR->value();
    s.maxRangeKm = qMax(m_minR->value(), m_maxR->value());
    s.startAzDeg = m_startAz->value();
    s.endAzDeg = m_endAz->value();
    s.beamDirDeg = m_beamDir->value() >= 0.0 ? m_beamDir->value() : std::numeric_limits<double>::quiet_NaN();
    s.ringStepKm = m_ringStep->value();
    s.azStepDeg = m_azStep->value();
    s.showFullRings = m_fullRingsChk->isChecked();
    s.refAnchor = m_anchorCombo->currentIndex();
    s.color = m_color;
    m_store->updateSite(s);
}

void RadarSiteDialog::onAddSite()
{
    if (!m_store)
        return;
    RadarSite s;
    s.name = QStringLiteral("自定义雷达 %1").arg(m_store->sites().size() + 1);
    s.hasRx = true;
    s.rxLat = 33.0108;
    s.rxLon = 112.6503;
    s.hasSector = true;
    s.minRangeKm = 0.0;
    s.maxRangeKm = 500.0;
    s.startAzDeg = 0.0;
    s.endAzDeg = 360.0;
    s.showFullRings = true;
    m_currentId = m_store->addSite(s);
}

void RadarSiteDialog::onRemoveSite()
{
    if (!m_store || m_currentId.isEmpty())
        return;
    m_store->removeSite(m_currentId);
}

void RadarSiteDialog::onSetActive()
{
    if (!m_store || m_currentId.isEmpty())
        return;
    m_store->setActiveSiteId(m_currentId);
}

void RadarSiteDialog::onFlyTo()
{
    if (!m_store || m_currentId.isEmpty())
        return;
    const RadarSite *s = m_store->site(m_currentId);
    double lat = 0.0, lon = 0.0;
    if (s && s->anchorLatLon(&lat, &lon)) {
        const double altKm = s->maxRangeKm > 0.0 ? qBound(200.0, s->maxRangeKm * 1.4, 7000.0) : 1200.0;
        emit flyToSiteRequested(lon, lat, altKm);
    }
}
