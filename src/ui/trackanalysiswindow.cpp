#include "trackanalysiswindow.h"
#include "theme.h"
#include "timeserieschart.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>

namespace {

const QColor kColSmooth(0x38, 0xbd, 0xf8);
const QColor kColAssc(0x22, 0xc5, 0x5e);
const QColor kColPred(0xf5, 0x9e, 0x0b);
const QColor kColRes(0xf4, 0x3f, 0x5e);

QVector<QPointF> buildSeriesPts(const QVector<TrackPoint> &pts,
                                std::function<double(const TrackPoint &, int)> extractor)
{
    QVector<QPointF> out;
    out.reserve(pts.size());
    for (int i = 0; i < pts.size(); ++i) {
        const double v = extractor(pts[i], i);
        if (std::isfinite(v))
            out.append(QPointF(double(pts[i].t), v));
    }
    return out;
}

QString fmtMetric(const QString &name, const trackanalysis::ErrorMetric &m, const QString &unit, int prec = 2)
{
    if (!m.valid())
        return QString();
    return QStringLiteral("%1: RMSE=%2  MAE=%3  P95=%4  Max=%5 %6 (N=%7)")
        .arg(name)
        .arg(m.rmse, 0, 'f', prec)
        .arg(m.mae, 0, 'f', prec)
        .arg(m.p95, 0, 'f', prec)
        .arg(m.maxAbs, 0, 'f', prec)
        .arg(unit)
        .arg(m.count);
}

} // namespace

TrackAnalysisWindow::TrackAnalysisWindow(const TrackStore *store, QWidget *parent)
    : QWidget(parent)
    , m_store(store)
{
    setWindowFlag(Qt::Window, true);
    setWindowTitle(QStringLiteral("航迹时序分析与定量对比"));
    buildUi();
    refreshTracks();
    resize(1080, 740);
}

TrackAnalysisWindow::~TrackAnalysisWindow() = default;

void TrackAnalysisWindow::closeEvent(QCloseEvent *e)
{
    emit compareOverlayChanged(QVector<trackanalysis::TrackDiffSample>(), -1, false);
    QWidget::closeEvent(e);
}

QString TrackAnalysisWindow::trackDisplayText(const Track &t) const
{
    const QString title = t.flightNo.isEmpty() ? t.id : QStringLiteral("%1 (%2)").arg(t.flightNo, t.id);
    return QStringLiteral("%1 · %2 · %3 [%4点]")
        .arg(title, trackSourceName(t.source), t.fileName).arg(t.points.size());
}

void TrackAnalysisWindow::buildUi()
{
    QVBoxLayout *root = new QVBoxLayout(this);
    ui::bindMargins(root, 8, 8, 8, 8);
    ui::bindSpacing(root, 6);

    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("analysisTabs"));
    m_tabs->addTab(buildSeriesTab(), QStringLiteral("时序曲线分析"));
    m_tabs->addTab(buildCompareTab(), QStringLiteral("两航迹定量误差对比"));
    root->addWidget(m_tabs, 1);

    setThemedStyle(this, ui::dialogQss() + ui::scrollbarQss() + QStringLiteral(
        "QWidget { background: var(--bg-primary); color: var(--text-primary); font-family: %1; font-size: 12px; }"
        "QTabWidget::pane { border: 1px solid var(--border-primary); background: var(--bg-primary); }"
        "QTabBar::tab { background: var(--bg-secondary); color: var(--text-secondary); padding: 6px 14px;"
        " border: 1px solid var(--border-primary); border-bottom: none; margin-right: 2px; }"
        "QTabBar::tab:selected { background: var(--bg-primary); color: var(--text-primary); font-weight: 600; }"
        "QComboBox, QSpinBox { background: var(--input-bg); color: var(--input-fg);"
        " border: 1px solid var(--input-border); border-radius: 4px; padding: 3px 6px; }"
        "QPushButton { background: var(--button-bg); color: var(--button-fg);"
        " border: 1px solid var(--border-primary); border-radius: 4px; padding: 4px 10px; }"
        "QPushButton:hover { background: var(--button-hover); color: var(--text-primary); }"
        "#summaryBox { background: var(--bg-secondary); border: 1px solid var(--border-secondary);"
        " border-radius: 4px; padding: 6px 10px; color: var(--text-primary); font-family: %2; font-size: 11px; }")
        .arg(ui::uiFamilies(), ui::monoFamilies()));
}

QWidget *TrackAnalysisWindow::buildSeriesTab()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *lay = new QVBoxLayout(page);
    ui::bindMargins(lay, 8, 8, 8, 8);
    ui::bindSpacing(lay, 6);

    QHBoxLayout *bar = new QHBoxLayout;
    bar->addWidget(new QLabel(QStringLiteral("航迹:"), page));
    m_trackCombo = new QComboBox(page);
    m_trackCombo->setMinimumWidth(280);
    bar->addWidget(m_trackCombo, 1);

    m_chkSmooth = new QCheckBox(QStringLiteral("平滑"), page);
    m_chkAssc = new QCheckBox(QStringLiteral("关联"), page);
    m_chkPred = new QCheckBox(QStringLiteral("预测"), page);
    m_chkResiduals = new QCheckBox(QStringLiteral("新息残差"), page);
    m_chkSmooth->setChecked(true);
    m_chkAssc->setChecked(true);
    m_chkPred->setChecked(true);
    m_chkResiduals->setChecked(true);
    for (QCheckBox *c : { m_chkSmooth, m_chkAssc, m_chkPred, m_chkResiduals }) {
        bar->addWidget(c);
        connect(c, &QCheckBox::toggled, this, &TrackAnalysisWindow::rebuildSeriesCharts);
    }
    QPushButton *resetBtn = new QPushButton(QStringLiteral("重置缩放"), page);
    connect(resetBtn, &QPushButton::clicked, this, [this]() {
        for (TimeSeriesChart *ch : qAsConst(m_seriesCharts))
            ch->resetZoom(false);
    });
    bar->addWidget(resetBtn);
    lay->addLayout(bar);

    m_seriesSummary = new QLabel(page);
    m_seriesSummary->setObjectName(QStringLiteral("summaryBox"));
    m_seriesSummary->setWordWrap(true);
    lay->addWidget(m_seriesSummary);

    QScrollArea *scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    QWidget *holder = new QWidget(scroll);
    m_chartsLay = new QVBoxLayout(holder);
    m_chartsLay->setContentsMargins(0, 0, 0, 0);
    ui::bindSpacing(m_chartsLay, 6);
    m_chartsLay->addStretch(1);
    scroll->setWidget(holder);
    lay->addWidget(scroll, 1);

    connect(m_trackCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (!m_populating)
            rebuildSeriesCharts();
    });
    return page;
}

QWidget *TrackAnalysisWindow::buildCompareTab()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *lay = new QVBoxLayout(page);
    ui::bindMargins(lay, 8, 8, 8, 8);
    ui::bindSpacing(lay, 6);

    auto makeModeCombo = [page]() {
        QComboBox *c = new QComboBox(page);
        c->addItem(QStringLiteral("平滑"), int(RadarPointMode::Smooth));
        c->addItem(QStringLiteral("关联"), int(RadarPointMode::Associated));
        c->addItem(QStringLiteral("预测"), int(RadarPointMode::Predicted));
        c->setFixedWidth(74);
        return c;
    };

    QHBoxLayout *row1 = new QHBoxLayout;
    row1->addWidget(new QLabel(QStringLiteral("基准 A:"), page));
    m_cmpTrackA = new QComboBox(page);
    m_cmpModeA = makeModeCombo();
    row1->addWidget(m_cmpTrackA, 1);
    row1->addWidget(m_cmpModeA);
    row1->addSpacing(8);
    row1->addWidget(new QLabel(QStringLiteral("待测 B:"), page));
    m_cmpTrackB = new QComboBox(page);
    m_cmpModeB = makeModeCombo();
    row1->addWidget(m_cmpTrackB, 1);
    row1->addWidget(m_cmpModeB);
    lay->addLayout(row1);

    QHBoxLayout *row2 = new QHBoxLayout;
    row2->addWidget(new QLabel(QStringLiteral("插值容差(s):"), page));
    m_maxGapSpin = new QSpinBox(page);
    m_maxGapSpin->setRange(1, 600);
    m_maxGapSpin->setValue(30);
    m_maxGapSpin->setFixedWidth(70);
    row2->addWidget(m_maxGapSpin);

    QPushButton *presetSA = new QPushButton(QStringLiteral("同航迹: 平滑 vs 关联"), page);
    QPushButton *presetPA = new QPushButton(QStringLiteral("同航迹: 预测 vs 关联"), page);
    QPushButton *presetSP = new QPushButton(QStringLiteral("同航迹: 平滑 vs 预测"), page);
    connect(presetSA, &QPushButton::clicked, this, [this]() { applyPresetCompare(RadarPointMode::Smooth, RadarPointMode::Associated); });
    connect(presetPA, &QPushButton::clicked, this, [this]() { applyPresetCompare(RadarPointMode::Predicted, RadarPointMode::Associated); });
    connect(presetSP, &QPushButton::clicked, this, [this]() { applyPresetCompare(RadarPointMode::Smooth, RadarPointMode::Predicted); });
    row2->addWidget(presetSA);
    row2->addWidget(presetPA);
    row2->addWidget(presetSP);
    row2->addStretch(1);

    m_showMapLinks = new QCheckBox(QStringLiteral("在地图显示误差连线"), page);
    m_showMapLinks->setChecked(true);
    row2->addWidget(m_showMapLinks);

    QPushButton *exportBtn = new QPushButton(QStringLiteral("导出 CSV..."), page);
    connect(exportBtn, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("导出航迹误差对比数据"),
            QStringLiteral("track_compare.csv"), QStringLiteral("CSV 文件 (*.csv)"));
        if (path.isEmpty())
            return;
        QString err;
        if (!exportLastCompareCsv(path, &err))
            QMessageBox::warning(this, QStringLiteral("导出失败"), err);
    });
    row2->addWidget(exportBtn);
    lay->addLayout(row2);

    m_cmpSummary = new QLabel(page);
    m_cmpSummary->setObjectName(QStringLiteral("summaryBox"));
    m_cmpSummary->setWordWrap(true);
    lay->addWidget(m_cmpSummary);

    QScrollArea *scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    QWidget *holder = new QWidget(scroll);
    QVBoxLayout *cl = new QVBoxLayout(holder);
    cl->setContentsMargins(0, 0, 0, 0);
    ui::bindSpacing(cl, 6);
    for (int i = 0; i < 3; ++i) {
        TimeSeriesChart *ch = new TimeSeriesChart(holder);
        ch->setMinimumHeight(165);
        cl->addWidget(ch);
        m_cmpCharts.append(ch);
        connect(ch, &TimeSeriesChart::xRangeChanged, this, [this, ch](double lo, double hi) {
            syncChartsXRange(m_cmpCharts, ch, lo, hi);
        });
        connect(ch, &TimeSeriesChart::hoverTimeChanged, this, [this, ch](qint64 ms, bool valid) {
            syncChartsHover(m_cmpCharts, ch, ms, valid);
        });
        connect(ch, &TimeSeriesChart::timePicked, this, [this](qint64 ms) { emit replaySeekRequested(ms); });
    }
    cl->addStretch(1);
    scroll->setWidget(holder);
    lay->addWidget(scroll, 1);

    auto trig = [this]() {
        if (!m_populating)
            runComparison();
    };
    connect(m_cmpTrackA, QOverload<int>::of(&QComboBox::currentIndexChanged), this, trig);
    connect(m_cmpModeA, QOverload<int>::of(&QComboBox::currentIndexChanged), this, trig);
    connect(m_cmpTrackB, QOverload<int>::of(&QComboBox::currentIndexChanged), this, trig);
    connect(m_cmpModeB, QOverload<int>::of(&QComboBox::currentIndexChanged), this, trig);
    connect(m_maxGapSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, trig);
    connect(m_showMapLinks, &QCheckBox::toggled, this, &TrackAnalysisWindow::emitCompareOverlay);
    return page;
}

void TrackAnalysisWindow::refreshTracks()
{
    m_populating = true;
    const QString prevKey = m_trackCombo->currentData().toString();
    const QString prevA = m_cmpTrackA->currentData().toString();
    const QString prevB = m_cmpTrackB->currentData().toString();
    m_trackCombo->clear();
    m_cmpTrackA->clear();
    m_cmpTrackB->clear();

    if (m_store) {
        const QVector<Track> &tracks = m_store->tracks();
        for (int i = 0; i < tracks.size(); ++i) {
            const Track &t = tracks[i];
            const QString txt = trackDisplayText(t);
            m_trackCombo->addItem(txt, t.key());
            m_cmpTrackA->addItem(txt, t.key());
            m_cmpTrackB->addItem(txt, t.key());
        }
    }
    auto restore = [](QComboBox *cb, const QString &key, int fallback) {
        const int idx = key.isEmpty() ? -1 : cb->findData(key);
        if (idx >= 0)
            cb->setCurrentIndex(idx);
        else if (cb->count() > fallback)
            cb->setCurrentIndex(fallback);
    };
    restore(m_trackCombo, prevKey, 0);
    restore(m_cmpTrackA, prevA, 0);
    restore(m_cmpTrackB, prevB, m_cmpTrackB->count() > 1 ? 1 : 0);
    m_populating = false;
    rebuildSeriesCharts();
    runComparison();
}

void TrackAnalysisWindow::openForTrack(int trackIndex, int tabIndex)
{
    refreshTracks();
    if (m_store && trackIndex >= 0 && trackIndex < m_store->size()) {
        const QString key = m_store->tracks()[trackIndex].key();
        const int idx = m_trackCombo->findData(key);
        if (idx >= 0) {
            m_trackCombo->setCurrentIndex(idx);
            m_cmpTrackA->setCurrentIndex(idx);
            m_cmpTrackB->setCurrentIndex(idx);
            // 同航迹对比默认选 平滑 vs 关联（若有）
            const Track &t = m_store->tracks()[trackIndex];
            if (t.hasRawPoints()) {
                m_cmpModeA->setCurrentIndex(0);
                m_cmpModeB->setCurrentIndex(1);
            }
        }
    }
    m_tabs->setCurrentIndex(qBound(0, tabIndex, m_tabs->count() - 1));
    show();
    raise();
    activateWindow();
}

void TrackAnalysisWindow::openCompare(int trackIndexA, int trackIndexB)
{
    refreshTracks();
    if (m_store) {
        if (trackIndexA >= 0 && trackIndexA < m_store->size())
            m_cmpTrackA->setCurrentIndex(m_cmpTrackA->findData(m_store->tracks()[trackIndexA].key()));
        if (trackIndexB >= 0 && trackIndexB < m_store->size())
            m_cmpTrackB->setCurrentIndex(m_cmpTrackB->findData(m_store->tracks()[trackIndexB].key()));
    }
    m_tabs->setCurrentIndex(1);
    runComparison();
    show();
    raise();
    activateWindow();
}

void TrackAnalysisWindow::setReplayTime(qint64 ms, bool active)
{
    m_replayMs = ms;
    m_replayActive = active;
    for (TimeSeriesChart *ch : qAsConst(m_seriesCharts))
        ch->setCursorTime(ms, active);
    for (TimeSeriesChart *ch : qAsConst(m_cmpCharts))
        ch->setCursorTime(ms, active);
}

int TrackAnalysisWindow::currentTab() const
{
    return m_tabs ? m_tabs->currentIndex() : 0;
}

bool TrackAnalysisWindow::exportLastCompareCsv(const QString &path, QString *error) const
{
    return trackanalysis::exportCompareCsv(m_lastCompare, path, error);
}

void TrackAnalysisWindow::applyPresetCompare(RadarPointMode modeA, RadarPointMode modeB)
{
    m_populating = true;
    const int idx = m_trackCombo->currentIndex();
    if (idx >= 0 && idx < m_cmpTrackA->count()) {
        m_cmpTrackA->setCurrentIndex(idx);
        m_cmpTrackB->setCurrentIndex(idx);
    }
    m_cmpModeA->setCurrentIndex(int(modeA));
    m_cmpModeB->setCurrentIndex(int(modeB));
    m_populating = false;
    runComparison();
}

void TrackAnalysisWindow::syncChartsXRange(const QVector<TimeSeriesChart *> &group,
                                           TimeSeriesChart *src, double lo, double hi)
{
    if (m_syncingRange)
        return;
    m_syncingRange = true;
    for (TimeSeriesChart *ch : group)
        if (ch != src)
            ch->setXRange(lo, hi, false);
    m_syncingRange = false;
}

void TrackAnalysisWindow::syncChartsHover(const QVector<TimeSeriesChart *> &group,
                                          TimeSeriesChart *src, qint64 ms, bool valid)
{
    for (TimeSeriesChart *ch : group)
        if (ch != src)
            ch->setLinkedHoverTime(ms, valid);
}

void TrackAnalysisWindow::rebuildSeriesCharts()
{
    for (TimeSeriesChart *ch : qAsConst(m_seriesCharts)) {
        m_chartsLay->removeWidget(ch);
        delete ch;
    }
    m_seriesCharts.clear();

    if (!m_store || m_trackCombo->currentIndex() < 0) {
        m_seriesSummary->setText(QStringLiteral("暂无航迹数据。"));
        return;
    }
    const QString key = m_trackCombo->currentData().toString();
    const int ti = m_store->indexOf(key);
    if (ti < 0)
        return;
    const Track &t = m_store->tracks()[ti];
    const bool isRadar = (t.source == TrackSource::Radar || t.source == TrackSource::RadarRaw);
    m_chkSmooth->setEnabled(isRadar);
    m_chkAssc->setEnabled(isRadar && t.hasRawPoints());
    m_chkPred->setEnabled(isRadar && t.hasPredPoints());
    m_chkResiduals->setEnabled(isRadar && t.hasRawPoints() && (t.hasPredPoints() || !t.points.isEmpty()));

    auto addChart = [&](const QString &title, const QString &unit, const QVector<TimeSeriesChart::Series> &list) {
        bool any = false;
        for (const auto &s : list)
            if (!s.pts.isEmpty())
                any = true;
        if (!any)
            return;
        TimeSeriesChart *ch = new TimeSeriesChart;
        ch->setTitle(title, unit);
        ch->setSeries(list);
        ch->setCursorTime(m_replayMs, m_replayActive);
        m_chartsLay->insertWidget(m_chartsLay->count() - 1, ch);
        m_seriesCharts.append(ch);
        connect(ch, &TimeSeriesChart::xRangeChanged, this, [this, ch](double lo, double hi) {
            syncChartsXRange(m_seriesCharts, ch, lo, hi);
        });
        connect(ch, &TimeSeriesChart::hoverTimeChanged, this, [this, ch](qint64 ms, bool valid) {
            syncChartsHover(m_seriesCharts, ch, ms, valid);
        });
        connect(ch, &TimeSeriesChart::timePicked, this, [this, key](qint64 ms) {
            emit replaySeekRequested(ms);
            if (!m_store)
                return;
            const int idx = m_store->indexOf(key);
            if (idx < 0)
                return;
            const Track &tr = m_store->tracks()[idx];
            if (tr.points.isEmpty())
                return;
            // 找最近采样点并在地图定位准星
            int best = 0;
            qint64 bestD = std::llabs(tr.points[0].t - ms);
            for (int k = 1; k < tr.points.size(); ++k) {
                const qint64 d = std::llabs(tr.points[k].t - ms);
                if (d < bestD) {
                    bestD = d;
                    best = k;
                }
            }
            const TrackPoint &pt = tr.points[best];
            const QString lbl = QStringLiteral("%1  %2").arg(tr.flightNo.isEmpty() ? tr.id : tr.flightNo,
                                                             formatBeijingTime(pt.t));
            emit timeLocateRequested(key, pt.t, pt.lon, pt.lat, lbl);
        });
    };

    if (isRadar) {
        const bool showS = m_chkSmooth->isChecked();
        const bool showA = m_chkAssc->isChecked() && t.hasRawPoints();
        const bool showP = m_chkPred->isChecked() && t.hasPredPoints();

        auto makeTri = [&](std::function<double(const TrackPoint &, int)> ex) {
            QVector<TimeSeriesChart::Series> list;
            if (showS) list.append({ QStringLiteral("平滑"), kColSmooth, TimeSeriesChart::SeriesStyle::SolidLine, buildSeriesPts(t.points, ex) });
            if (showP) list.append({ QStringLiteral("预测"), kColPred, TimeSeriesChart::SeriesStyle::DashLine, buildSeriesPts(t.predPoints, ex) });
            if (showA) list.append({ QStringLiteral("关联"), kColAssc, TimeSeriesChart::SeriesStyle::Dots, buildSeriesPts(t.rawPoints, ex) });
            return list;
        };

        addChart(QStringLiteral("斜距 prange"), QStringLiteral("km/m"),
                 makeTri([](const TrackPoint &p, int) { return double(p.prange); }));
        addChart(QStringLiteral("方位 paz"), QStringLiteral("°"),
                 makeTri([](const TrackPoint &p, int) { return double(p.paz); }));
        addChart(QStringLiteral("径向速度 pvr"), QStringLiteral("m/s"),
                 makeTri([](const TrackPoint &p, int) { return double(p.pvr); }));
        addChart(QStringLiteral("信噪比 SNR"), QStringLiteral("dB"),
                 QVector<TimeSeriesChart::Series>{
                     { QStringLiteral("关联 SNR"), kColAssc, TimeSeriesChart::SeriesStyle::SolidLine,
                       buildSeriesPts(t.hasRawPoints() ? t.rawPoints : t.points,
                                      [](const TrackPoint &p, int) { return double(p.snr); }) }
                 });

        // 帧间隔 Δt（检查丢帧/断航）
        auto dtSeries = [&](const QVector<TrackPoint> &pts, const QString &name, const QColor &col,
                            TimeSeriesChart::SeriesStyle style) {
            QVector<QPointF> v;
            for (int i = 1; i < pts.size(); ++i)
                v.append(QPointF(double(pts[i].t), double(pts[i].t - pts[i - 1].t) / 1000.0));
            return TimeSeriesChart::Series{ name, col, style, v };
        };
        QVector<TimeSeriesChart::Series> dtList;
        if (showS) dtList.append(dtSeries(t.points, QStringLiteral("平滑 Δt"), kColSmooth, TimeSeriesChart::SeriesStyle::SolidLine));
        if (showA) dtList.append(dtSeries(t.rawPoints, QStringLiteral("关联 Δt"), kColAssc, TimeSeriesChart::SeriesStyle::Dots));
        addChart(QStringLiteral("采样间隔 Δt"), QStringLiteral("s"), dtList);

        // 预测新息残差
        if (m_chkResiduals->isChecked() && t.hasRawPoints()) {
            const QVector<trackanalysis::FilterResidualPoint> res = trackanalysis::computeFilterResiduals(t);
            if (!res.isEmpty()) {
                QVector<QPointF> gPred, gSmooth, drPts;
                for (const auto &r : res) {
                    if (std::isfinite(r.predGeoErrKm))
                        gPred.append(QPointF(double(r.timeMs), r.predGeoErrKm));
                    if (std::isfinite(r.smoothGeoErrKm))
                        gSmooth.append(QPointF(double(r.timeMs), r.smoothGeoErrKm));
                    if (std::isfinite(r.dRange))
                        drPts.append(QPointF(double(r.timeMs), r.dRange));
                }
                addChart(QStringLiteral("滤波水平残差 (相对关联点)"), QStringLiteral("km"),
                         QVector<TimeSeriesChart::Series>{
                             { QStringLiteral("预测新息"), kColRes, TimeSeriesChart::SeriesStyle::SolidLine, gPred },
                             { QStringLiteral("平滑残差"), kColSmooth, TimeSeriesChart::SeriesStyle::DashLine, gSmooth }
                         });
                addChart(QStringLiteral("距离新息 Δprange (关联 - 预测)"), QStringLiteral("km/m"),
                         QVector<TimeSeriesChart::Series>{
                             { QStringLiteral("Δprange"), kColPred, TimeSeriesChart::SeriesStyle::SolidLine, drPts }
                         });
            }
        }
        m_seriesSummary->setText(
            QStringLiteral("航迹 %1 | 平滑 %2 点 · 关联 %3 点 · 预测 %4 点 | 时间 %5 ~ %6（左键拖选缩放，双击还原，点击联动地图与回放）")
                .arg(t.id).arg(t.points.size()).arg(t.rawPoints.size()).arg(t.predPoints.size())
                .arg(formatBeijingTime(t.minTime()), formatBeijingTime(t.maxTime())));
    } else {
        auto mk = [&](const QString &name, const QColor &col, std::function<double(const TrackPoint &, int)> ex) {
            return QVector<TimeSeriesChart::Series>{
                { name, col, TimeSeriesChart::SeriesStyle::SolidLine, buildSeriesPts(t.points, ex) }
            };
        };
        addChart(QStringLiteral("气压高度"), QStringLiteral("ft"),
                 mk(QStringLiteral("Alt"), kColSmooth, [](const TrackPoint &p, int) { return double(p.alt) / 0.3048; }));
        addChart(QStringLiteral("地速"), QStringLiteral("kt"),
                 mk(QStringLiteral("Speed"), kColAssc, [](const TrackPoint &p, int) { return double(p.speed); }));
        addChart(QStringLiteral("航向"), QStringLiteral("°"),
                 mk(QStringLiteral("Heading"), kColPred, [](const TrackPoint &p, int) { return double(p.heading); }));
        addChart(QStringLiteral("垂直率"), QStringLiteral("ft/min"),
                 mk(QStringLiteral("VRate"), kColRes, [](const TrackPoint &p, int) { return double(p.vrate); }));
        m_seriesSummary->setText(
            QStringLiteral("ADS-B %1 (%2) | %3 点 | 时间 %4 ~ %5（左键拖选缩放，双击还原，点击联动地图与回放）")
                .arg(t.flightNo.isEmpty() ? t.id : t.flightNo, t.id).arg(t.points.size())
                .arg(formatBeijingTime(t.minTime()), formatBeijingTime(t.maxTime())));
    }
}

void TrackAnalysisWindow::runComparison()
{
    m_lastCompare = trackanalysis::TrackCompareResult();
    for (TimeSeriesChart *ch : qAsConst(m_cmpCharts))
        ch->clear();

    if (!m_store || m_cmpTrackA->currentIndex() < 0 || m_cmpTrackB->currentIndex() < 0) {
        m_cmpSummary->setText(QStringLiteral("请选择基准航迹 A 与待测航迹 B。"));
        emitCompareOverlay();
        return;
    }
    const int idxA = m_store->indexOf(m_cmpTrackA->currentData().toString());
    const int idxB = m_store->indexOf(m_cmpTrackB->currentData().toString());
    if (idxA < 0 || idxB < 0)
        return;
    const Track &tA = m_store->tracks()[idxA];
    const Track &tB = m_store->tracks()[idxB];
    const RadarPointMode mA = RadarPointMode(m_cmpModeA->currentData().toInt());
    const RadarPointMode mB = RadarPointMode(m_cmpModeB->currentData().toInt());
    const QVector<TrackPoint> &ptsA = tA.pointsForMode(mA);
    const QVector<TrackPoint> &ptsB = tB.pointsForMode(mB);
    const qint64 gapMs = qint64(m_maxGapSpin->value()) * 1000LL;

    m_lastCompare = trackanalysis::compareTracks(ptsA, ptsB, gapMs);
    if (!m_lastCompare.timeOverlapped) {
        m_cmpSummary->setText(
            QStringLiteral("⚠ 两条航迹时间窗无重叠，无法配对计算误差。\n"
                           "A (%1): %2 ~ %3\nB (%4): %5 ~ %6")
                .arg(tA.id, formatBeijingTime(tA.minTimeForMode(mA)), formatBeijingTime(tA.maxTimeForMode(mA)),
                     tB.id, formatBeijingTime(tB.minTimeForMode(mB)), formatBeijingTime(tB.maxTimeForMode(mB))));
        emitCompareOverlay();
        return;
    }
    if (m_lastCompare.samples.isEmpty()) {
        m_cmpSummary->setText(
            QStringLiteral("⚠ 时间窗重叠（%1 ~ %2），但在最大插值容差 %3 s 内无成功配对点，请适当调大容差。")
                .arg(formatBeijingTime(m_lastCompare.overlapStartMs),
                     formatBeijingTime(m_lastCompare.overlapEndMs))
                .arg(m_maxGapSpin->value()));
        emitCompareOverlay();
        return;
    }

    QStringList lines;
    lines << QStringLiteral("重叠时段: %1 ~ %2  |  匹配率: %3% (%4/%5 点)")
                 .arg(formatBeijingTime(m_lastCompare.overlapStartMs),
                      formatBeijingTime(m_lastCompare.overlapEndMs))
                 .arg(m_lastCompare.coverageB * 100.0, 0, 'f', 1)
                 .arg(m_lastCompare.samples.size()).arg(ptsB.size());
    lines << fmtMetric(QStringLiteral("水平距离误差"), m_lastCompare.geoDistKm, QStringLiteral("km"), 3);
    if (m_lastCompare.dRange.valid())
        lines << fmtMetric(QStringLiteral("斜距差 Δprange"), m_lastCompare.dRange, QStringLiteral("km/m"), 3);
    if (m_lastCompare.dAz.valid())
        lines << fmtMetric(QStringLiteral("方位差 Δpaz"), m_lastCompare.dAz, QStringLiteral("°"), 3);
    if (m_lastCompare.dVr.valid())
        lines << fmtMetric(QStringLiteral("速度差 Δpvr"), m_lastCompare.dVr, QStringLiteral("m/s"), 3);
    if (m_lastCompare.dAltM.valid())
        lines << fmtMetric(QStringLiteral("高度差 Δalt"), m_lastCompare.dAltM, QStringLiteral("m"), 1);
    m_cmpSummary->setText(lines.join(QLatin1Char('\n')));

    QVector<QPointF> geoPts, drPts, dazPts, dvrPts;
    for (const auto &s : qAsConst(m_lastCompare.samples)) {
        geoPts.append(QPointF(double(s.timeMs), s.geoDistKm));
        if (std::isfinite(s.dRange)) drPts.append(QPointF(double(s.timeMs), s.dRange));
        if (std::isfinite(s.dAz)) dazPts.append(QPointF(double(s.timeMs), s.dAz));
        if (std::isfinite(s.dVr)) dvrPts.append(QPointF(double(s.timeMs), s.dVr));
    }
    m_cmpCharts[0]->setTitle(QStringLiteral("水平位置偏差 (Vincenty)"), QStringLiteral("km"));
    m_cmpCharts[0]->setSeries({ { QStringLiteral("距离偏差"), kColRes, TimeSeriesChart::SeriesStyle::SolidLine, geoPts } });
    m_cmpCharts[0]->setCursorTime(m_replayMs, m_replayActive);

    m_cmpCharts[1]->setTitle(QStringLiteral("斜距偏差 Δprange (B - A)"), QStringLiteral("km/m"));
    m_cmpCharts[1]->setSeries({ { QStringLiteral("Δprange"), kColSmooth, TimeSeriesChart::SeriesStyle::SolidLine, drPts } });
    m_cmpCharts[1]->setCursorTime(m_replayMs, m_replayActive);

    m_cmpCharts[2]->setTitle(QStringLiteral("方位与多普勒偏差 (B - A)"), QStringLiteral("° / m/s"));
    m_cmpCharts[2]->setSeries({
        { QStringLiteral("Δpaz(°)"), kColPred, TimeSeriesChart::SeriesStyle::SolidLine, dazPts },
        { QStringLiteral("Δpvr(m/s)"), kColAssc, TimeSeriesChart::SeriesStyle::DashLine, dvrPts }
    });
    m_cmpCharts[2]->setCursorTime(m_replayMs, m_replayActive);

    emitCompareOverlay();
}

void TrackAnalysisWindow::emitCompareOverlay()
{
    const bool show = isVisible() && m_showMapLinks && m_showMapLinks->isChecked() && !m_lastCompare.samples.isEmpty();
    emit compareOverlayChanged(show ? m_lastCompare.samples : QVector<trackanalysis::TrackDiffSample>(),
                               show ? m_lastCompare.maxGeoErrorIndex : -1,
                               show);
}
