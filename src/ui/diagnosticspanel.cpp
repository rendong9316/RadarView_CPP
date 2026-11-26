#include "diagnosticspanel.h"

#include <QAbstractItemView>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QToolTip>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <limits>

#include "geocalc.h"
#include "theme.h"
#include "timeserieschart.h"

namespace diagui {

// 自绘紧凑「实测/外推色带条」：绿=实测关联，灰=外推补点，悬停提示帧号与时间，点击可跳转回放
class AsscRibbonWidget : public QWidget
{
public:
    struct Segment {
        int frameId;
        qint64 timeMs;
        bool associated;

        Segment() : frameId(0), timeMs(0), associated(false) {}
        Segment(int fid, qint64 t, bool a) : frameId(fid), timeMs(t), associated(a) {}
    };

    explicit AsscRibbonWidget(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMouseTracking(true);
        setCursor(Qt::PointingHandCursor);
        ui::bindSize(this, ui::SizeKind::FixedHeight, 18);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(Theme::instance(), &Theme::changed, this, [this]() { update(); });
    }

    void setOnSeek(const std::function<void(qint64)> &fn) { m_onSeek = fn; }

    void setSegments(const QVector<Segment> &segs)
    {
        m_segs = segs;
        m_hoverIdx = -1;
        update();
    }

    void clear()
    {
        m_segs.clear();
        m_hoverIdx = -1;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);

        const QRect r = rect().adjusted(0, 0, -1, -1);
        p.fillRect(r, themeColor("bg-primary"));
        p.setPen(themeColor("border-primary"));
        p.drawRect(r);

        const QRect inner = r.adjusted(1, 1, 0, 0);
        if (m_segs.isEmpty() || inner.width() <= 0 || inner.height() <= 0) {
            QFont f = font();
            f.setPixelSize(ui::px(0.72));
            p.setFont(f);
            p.setPen(themeColor("text-tertiary"));
            p.drawText(inner, Qt::AlignCenter, QStringLiteral("无关联标志序列"));
            return;
        }

        const int n = m_segs.size();
        const double cellW = double(inner.width()) / double(n);
        const QColor colAssc(0x22, 0xc5, 0x5e);   // 绿色：实测关联
        const QColor colExtrap(0x6b, 0x72, 0x80); // 灰色：外推补点
        const QColor sepColor(0, 0, 0, 70);

        for (int i = 0; i < n; ++i) {
            const int x0 = inner.left() + int(std::floor(double(i) * cellW));
            const int x1 = (i + 1 == n)
                ? (inner.right() + 1)
                : (inner.left() + int(std::floor(double(i + 1) * cellW)));
            const int w = qMax(1, x1 - x0);
            const QRect segRect(x0, inner.top(), w, inner.height());
            p.fillRect(segRect, m_segs.at(i).associated ? colAssc : colExtrap);
            if (cellW >= 4.0 && i > 0) {
                p.setPen(sepColor);
                p.drawLine(x0, inner.top(), x0, inner.bottom());
            }
        }

        if (m_hoverIdx >= 0 && m_hoverIdx < n) {
            const int x0 = inner.left() + int(std::floor(double(m_hoverIdx) * cellW));
            const int x1 = (m_hoverIdx + 1 == n)
                ? (inner.right() + 1)
                : (inner.left() + int(std::floor(double(m_hoverIdx + 1) * cellW)));
            p.setPen(QPen(themeColor("accent-primary"), 1));
            p.setBrush(Qt::NoBrush);
            p.drawRect(QRect(x0, inner.top(), qMax(1, x1 - x0 - 1), inner.height() - 1));
        }
    }

    void mouseMoveEvent(QMouseEvent *e) override
    {
        const int idx = indexAtX(e->pos().x());
        if (idx != m_hoverIdx) {
            m_hoverIdx = idx;
            update();
        }
        if (idx >= 0 && idx < m_segs.size()) {
            const Segment &s = m_segs.at(idx);
            QString tip = QStringLiteral("帧 #%1（第 %2/%3 点）\n状态：%4")
                              .arg(s.frameId)
                              .arg(idx + 1)
                              .arg(m_segs.size())
                              .arg(s.associated ? QStringLiteral("实测关联") : QStringLiteral("外推补点"));
            if (s.timeMs > 0)
                tip += QStringLiteral("\n时间：%1（点击定位回放）").arg(formatBeijingTime(s.timeMs));
            QToolTip::showText(e->globalPos(), tip, this);
        }
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && m_onSeek) {
            const int idx = indexAtX(e->pos().x());
            if (idx >= 0 && idx < m_segs.size() && m_segs.at(idx).timeMs > 0)
                m_onSeek(m_segs.at(idx).timeMs);
        }
        QWidget::mousePressEvent(e);
    }

    void leaveEvent(QEvent *) override
    {
        if (m_hoverIdx >= 0) {
            m_hoverIdx = -1;
            update();
        }
    }

private:
    int indexAtX(int x) const
    {
        if (m_segs.isEmpty())
            return -1;
        const QRect inner = rect().adjusted(1, 1, -2, -2);
        if (inner.width() <= 0 || x < inner.left() || x > inner.right())
            return -1;
        const double frac = double(x - inner.left()) / double(inner.width());
        return qBound(0, int(std::floor(frac * double(m_segs.size()))), m_segs.size() - 1);
    }

    QVector<Segment> m_segs;
    int m_hoverIdx = -1;
    std::function<void(qint64)> m_onSeek;
};

} // namespace diagui

namespace {

// 点击标题折叠/展开面板块
class CollapsibleHeaderFilter : public QObject
{
public:
    CollapsibleHeaderFilter(QWidget *body, QLabel *chevron, QObject *parent)
        : QObject(parent), m_body(body), m_chev(chevron) {}

    bool eventFilter(QObject *, QEvent *e) override
    {
        if (e->type() == QEvent::MouseButtonRelease
                && static_cast<QMouseEvent *>(e)->button() == Qt::LeftButton) {
            const bool next = !m_body->isVisible();
            m_body->setVisible(next);
            if (m_chev)
                m_chev->setText(next ? QStringLiteral("▾") : QStringLiteral("▸"));
            return true;
        }
        return false;
    }

private:
    QWidget *m_body;
    QLabel *m_chev;
};

void configureCompactTable(QTableWidget *tbl, const QStringList &headers, int minHeightPx)
{
    tbl->setColumnCount(headers.size());
    tbl->setHorizontalHeaderLabels(headers);
    tbl->setSelectionBehavior(QAbstractItemView::SelectRows);
    tbl->setSelectionMode(QAbstractItemView::SingleSelection);
    tbl->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tbl->setAlternatingRowColors(true);
    tbl->setWordWrap(false);
    tbl->setTextElideMode(Qt::ElideRight);
    tbl->setShowGrid(false);
    tbl->setMinimumWidth(0);
    tbl->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    ui::bindSize(tbl, ui::SizeKind::MinHeight, minHeightPx);
    tbl->verticalHeader()->setVisible(false);
    tbl->verticalHeader()->setDefaultSectionSize(ui::sz(22));
    QObject::connect(UiScale::instance(), &UiScale::changed, tbl, [tbl]() {
        tbl->verticalHeader()->setDefaultSectionSize(ui::sz(22));
    });
    tbl->horizontalHeader()->setHighlightSections(false);
    tbl->horizontalHeader()->setStretchLastSection(true);
    tbl->horizontalHeader()->setMinimumSectionSize(ui::sz(28));
}

QTableWidgetItem *makeItem(const QString &text, Qt::Alignment align = Qt::AlignLeft | Qt::AlignVCenter,
                           const QString &tip = QString())
{
    QTableWidgetItem *it = new QTableWidgetItem(text);
    it->setTextAlignment(align);
    if (!tip.isEmpty())
        it->setToolTip(tip);
    return it;
}

QString fmtClockShort(qint64 ms)
{
    if (ms <= 0)
        return QStringLiteral("—");
    const QString full = formatBeijingTime(ms);
    return full.size() >= 19 ? full.mid(11, 8) : full;
}

QString fmtOpt(double v, int prec, const QString &suffix = QString())
{
    if (!std::isfinite(v))
        return QStringLiteral("—");
    return QString::number(v, 'f', prec) + suffix;
}

QString shortTrackTitle(const Track &tr)
{
    if (!tr.flightNo.isEmpty())
        return QStringLiteral("%1 (%2)").arg(tr.flightNo, tr.id);
    return tr.id;
}

// 寻找最近时刻航迹点（用于无 bundle 时的退化关联/新息计算）
int findNearestPt(const QVector<TrackPoint> &pts, qint64 t, qint64 tolMs)
{
    if (pts.isEmpty())
        return -1;
    auto it = std::lower_bound(pts.constBegin(), pts.constEnd(), t,
                               [](const TrackPoint &p, qint64 v) { return p.t < v; });
    int best = -1;
    qint64 bestDt = tolMs + 1;
    if (it != pts.constEnd()) {
        const qint64 dt = std::llabs(it->t - t);
        if (dt <= tolMs && dt < bestDt) {
            bestDt = dt;
            best = int(it - pts.constBegin());
        }
    }
    if (it != pts.constBegin()) {
        const qint64 dt = std::llabs((it - 1)->t - t);
        if (dt <= tolMs && dt < bestDt)
            best = int(it - pts.constBegin()) - 1;
    }
    return best;
}

} // namespace

// ---------------------------------------------------------------------------
//  DiagnosticsPanel 构造与生命周期
// ---------------------------------------------------------------------------

DiagnosticsPanel::DiagnosticsPanel(const TrackStore *store, const RadarSiteStore *sites, QWidget *parent)
    : QWidget(parent)
    , m_store(store)
    , m_sites(sites)
{
    setObjectName(QStringLiteral("diagnosticsPanel"));
    buildUi();
    refreshAll();
}

DiagnosticsPanel::~DiagnosticsPanel() = default;

const trackdiag::TrackDiagBundle &DiagnosticsPanel::effectiveBundle() const
{
    return m_bundle ? *m_bundle : m_emptyBundle;
}

void DiagnosticsPanel::setDiagBundle(const trackdiag::TrackDiagBundle *bundle)
{
    m_bundle = bundle;
    refreshAll();
}

void DiagnosticsPanel::setDetDataset(const Point3dDataset *detData)
{
    m_detData = detData;
    refreshAll();
}

// ---------------------------------------------------------------------------
//  UI 构建
// ---------------------------------------------------------------------------

void DiagnosticsPanel::buildUi()
{
    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    ui::bindSpacing(root, 4);

    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("diagTabs"));
    m_tabs->setDocumentMode(true);
    m_tabs->setMinimumWidth(0);
    m_tabs->addTab(buildFalseTrackTab(), QStringLiteral("虚假航迹"));
    m_tabs->addTab(buildDisappearTab(), QStringLiteral("消失/断航"));
    root->addWidget(m_tabs, 1);

    setThemedStyle(this, ui::segmentedQss() + QStringLiteral(
        "#diagnosticsPanel { background: transparent; }"
        "QLabel { background: transparent; color: var(--text-primary); font-family: %1; font-size: 11px; }"
        "#diagTabs::pane { border: none; background: transparent; margin-top: 4px; }"
        "#diagTabs QTabBar::tab { background: var(--bg-tertiary); color: var(--text-secondary);"
        " padding: 5px 10px; border: 1px solid var(--border-primary); border-bottom: none;"
        " border-top-left-radius: 4px; border-top-right-radius: 4px; margin-right: 2px;"
        " font-family: %1; font-size: 11px; }"
        "#diagTabs QTabBar::tab:selected { background: var(--accent-primary); color: var(--accent-fg);"
        " border-color: var(--accent-primary); font-weight: 600; }"
        "#diagTabs QTabBar::tab:hover:!selected { background: var(--button-hover); color: var(--text-primary); }"
        "#diagBox, #diagCard { background: var(--bg-primary); border: 1px solid var(--border-primary);"
        " border-radius: 4px; }"
        "#diagSectionTitle { color: var(--text-primary); font-family: %1; font-size: 11px; font-weight: 600; }"
        "#diagMuted { color: var(--text-secondary); font-family: %1; font-size: 10px; }"
        "#diagMono { color: var(--text-primary); font-family: %2; font-size: 10px; }"
        "#diagStats { color: var(--accent-primary); font-family: %2; font-size: 11px; font-weight: 600; }"
        "#diagCause { background: var(--bg-secondary); border: 1px solid var(--border-primary);"
        " border-radius: 4px; padding: 4px 6px; color: var(--text-primary); font-family: %1; font-size: 11px; }"
        "#diagCause[level=\"error\"] { border-color: var(--error); background: var(--error-bg); color: var(--text-primary); }"
        "#diagCause[level=\"warn\"] { border-color: var(--warning-border); background: var(--warning-bg); color: var(--text-primary); }"
        "#diagSpin { background: var(--input-bg); color: var(--input-fg); border: 1px solid var(--input-border);"
        " border-radius: 3px; padding: 2px 4px; font-family: %2; font-size: 11px; }"
        "#diagSpin:focus { border-color: var(--accent-primary); }"
        "#diagCombo { background: var(--input-bg); color: var(--input-fg); border: 1px solid var(--input-border);"
        " border-radius: 4px; padding: 3px 6px; font-family: %1; font-size: 11px; }"
        "#diagCombo:focus { border-color: var(--accent-primary); }"
        "#diagCombo QAbstractItemView { background: var(--dropdown-bg); color: var(--dropdown-fg);"
        " border: 1px solid var(--border-primary); selection-background-color: var(--dropdown-hover);"
        " selection-color: var(--text-primary); }"
        "#diagCheck { color: var(--text-primary); font-family: %1; font-size: 11px; spacing: 5px; }"
        "#diagBtn { padding: 4px 8px; background: var(--button-bg); color: var(--button-fg);"
        " border: 1px solid var(--border-primary); border-radius: 4px; font-family: %1; font-size: 11px; }"
        "#diagBtn:hover { background: var(--button-hover); color: var(--text-primary); border-color: var(--accent-primary); }"
        "#diagBtn:checked { background: var(--accent-primary); color: var(--accent-fg); border-color: var(--accent-primary); font-weight: 600; }"
        "#diagBtn:disabled { color: var(--text-muted); border-color: var(--border-secondary); }"
        "#diagPrimaryBtn { padding: 4px 8px; background: var(--accent-primary); color: var(--accent-fg);"
        " border: 1px solid var(--accent-primary); border-radius: 4px; font-family: %1; font-size: 11px; font-weight: 600; }"
        "#diagPrimaryBtn:hover { background: var(--accent-hover); border-color: var(--accent-hover); }"
        "#diagPrimaryBtn:disabled { background: var(--button-bg); color: var(--text-muted); border-color: var(--border-secondary); }"
        "QTableWidget { background: var(--bg-primary); alternate-background-color: var(--bg-secondary);"
        " color: var(--text-primary); border: 1px solid var(--border-primary); border-radius: 4px;"
        " selection-background-color: var(--accent-subtle); selection-color: var(--text-primary);"
        " font-family: %2; font-size: 10px; outline: none; }"
        "QTableWidget::item { padding: 2px 4px; }"
        "QTableWidget::item:selected { background: var(--accent-subtle-hover); color: var(--text-primary); }"
        "QHeaderView::section { background: var(--bg-tertiary); color: var(--text-secondary);"
        " border: none; border-right: 1px solid var(--border-secondary); border-bottom: 1px solid var(--border-primary);"
        " padding: 3px 4px; font-family: %1; font-size: 10px; font-weight: 600; }")
        .arg(ui::uiFamilies(), ui::monoFamilies()));
}

QWidget *DiagnosticsPanel::buildFalseTrackTab()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *lay = new QVBoxLayout(page);
    lay->setContentsMargins(0, 0, 0, 0);
    ui::bindSpacing(lay, 6);

    // ---- 1. 折叠/紧凑「门限试算沙盒」 ----
    QFrame *sandboxBox = new QFrame(page);
    sandboxBox->setObjectName(QStringLiteral("diagBox"));
    QVBoxLayout *sbLay = new QVBoxLayout(sandboxBox);
    ui::bindMargins(sbLay, 6, 5, 6, 6);
    ui::bindSpacing(sbLay, 4);

    QWidget *sbHdr = new QWidget(sandboxBox);
    sbHdr->setCursor(Qt::PointingHandCursor);
    QHBoxLayout *sbHdrLay = new QHBoxLayout(sbHdr);
    sbHdrLay->setContentsMargins(0, 0, 0, 0);
    ui::bindSpacing(sbHdrLay, 4);
    m_sandboxChevron = new QLabel(QStringLiteral("▾"), sbHdr);
    m_sandboxChevron->setObjectName(QStringLiteral("diagMuted"));
    QLabel *sbTitle = new QLabel(QStringLiteral("门限试算沙盒"), sbHdr);
    sbTitle->setObjectName(QStringLiteral("diagSectionTitle"));
    m_falseStatsLabel = new QLabel(QStringLiteral("拟滤除 0 / 共 0 条"), sbHdr);
    m_falseStatsLabel->setObjectName(QStringLiteral("diagStats"));
    m_falseStatsLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    sbHdrLay->addWidget(m_sandboxChevron);
    sbHdrLay->addWidget(sbTitle);
    sbHdrLay->addStretch(1);
    sbHdrLay->addWidget(m_falseStatsLabel);
    sbLay->addWidget(sbHdr);

    m_sandboxBody = new QWidget(sandboxBox);
    QVBoxLayout *bodyLay = new QVBoxLayout(m_sandboxBody);
    bodyLay->setContentsMargins(0, 2, 0, 0);
    ui::bindSpacing(bodyLay, 3);

    QGridLayout *grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    ui::bindSpacing(grid, 4);

    auto addRow = [this, grid](int row, const QString &label, QWidget *editor) {
        QLabel *l = new QLabel(label, m_sandboxBody);
        l->setObjectName(QStringLiteral("diagMuted"));
        editor->setObjectName(QStringLiteral("diagSpin"));
        ui::bindSize(editor, ui::SizeKind::MinWidth, 72);
        grid->addWidget(l, row, 0);
        grid->addWidget(editor, row, 1);
    };

    m_spinMinQuality = new QDoubleSpinBox(m_sandboxBody);
    m_spinMinQuality->setRange(0.0, 1.0);
    m_spinMinQuality->setSingleStep(0.05);
    m_spinMinQuality->setDecimals(2);
    m_spinMinQuality->setValue(m_falseCfg.minQuality);
    addRow(0, QStringLiteral("最低质量 (0~1)"), m_spinMinQuality);

    m_spinMinAsscPct = new QSpinBox(m_sandboxBody);
    m_spinMinAsscPct->setRange(0, 100);
    m_spinMinAsscPct->setSingleStep(5);
    m_spinMinAsscPct->setSuffix(QStringLiteral("%"));
    m_spinMinAsscPct->setValue(int(std::lround(m_falseCfg.minAsscRatio * 100.0)));
    addRow(1, QStringLiteral("最低关联率"), m_spinMinAsscPct);

    m_spinMinAsscPts = new QSpinBox(m_sandboxBody);
    m_spinMinAsscPts->setRange(1, 50);
    m_spinMinAsscPts->setSingleStep(1);
    m_spinMinAsscPts->setValue(m_falseCfg.minAsscPoints);
    addRow(2, QStringLiteral("最少实测点"), m_spinMinAsscPts);

    m_spinMaxSuccLoss = new QSpinBox(m_sandboxBody);
    m_spinMaxSuccLoss->setRange(0, 30);
    m_spinMaxSuccLoss->setSingleStep(1);
    m_spinMaxSuccLoss->setValue(m_falseCfg.maxSuccLoss);
    addRow(3, QStringLiteral("最大连丢帧"), m_spinMaxSuccLoss);

    m_spinMinSnr = new QDoubleSpinBox(m_sandboxBody);
    m_spinMinSnr->setRange(0.0, 40.0);
    m_spinMinSnr->setSingleStep(1.0);
    m_spinMinSnr->setDecimals(1);
    m_spinMinSnr->setSuffix(QStringLiteral(" dB"));
    m_spinMinSnr->setValue(m_falseCfg.minMeanSnrDb);
    addRow(4, QStringLiteral("最低均 SNR"), m_spinMinSnr);

    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 0);
    bodyLay->addLayout(grid);

    QHBoxLayout *optRow = new QHBoxLayout;
    optRow->setContentsMargins(0, 2, 0, 0);
    ui::bindSpacing(optRow, 4);
    m_chkMuteFiltered = new QCheckBox(QStringLiteral("地图灰显被滤除航迹"), m_sandboxBody);
    m_chkMuteFiltered->setObjectName(QStringLiteral("diagCheck"));
    m_chkMuteFiltered->setChecked(true);
    QPushButton *resetBtn = new QPushButton(QStringLiteral("重置"), m_sandboxBody);
    resetBtn->setObjectName(QStringLiteral("diagBtn"));
    resetBtn->setCursor(Qt::PointingHandCursor);
    resetBtn->setToolTip(QStringLiteral("恢复默认虚假航迹试算门限"));
    optRow->addWidget(m_chkMuteFiltered, 1);
    optRow->addWidget(resetBtn);
    bodyLay->addLayout(optRow);

    sbLay->addWidget(m_sandboxBody);
    sbHdr->installEventFilter(new CollapsibleHeaderFilter(m_sandboxBody, m_sandboxChevron, sbHdr));
    lay->addWidget(sandboxBox);

    auto onSandboxChanged = [this]() {
        if (m_updatingSandbox)
            return;
        syncConfigFromSandbox();
        recomputeFalseTracks();
    };
    connect(m_spinMinQuality, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [onSandboxChanged](double) { onSandboxChanged(); });
    connect(m_spinMinAsscPct, QOverload<int>::of(&QSpinBox::valueChanged), this, [onSandboxChanged](int) { onSandboxChanged(); });
    connect(m_spinMinAsscPts, QOverload<int>::of(&QSpinBox::valueChanged), this, [onSandboxChanged](int) { onSandboxChanged(); });
    connect(m_spinMaxSuccLoss, QOverload<int>::of(&QSpinBox::valueChanged), this, [onSandboxChanged](int) { onSandboxChanged(); });
    connect(m_spinMinSnr, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [onSandboxChanged](double) { onSandboxChanged(); });
    connect(m_chkMuteFiltered, &QCheckBox::toggled, this, [this](bool) { emitCurrentMutedTracks(); });
    connect(resetBtn, &QPushButton::clicked, this, [this]() {
        m_updatingSandbox = true;
        const trackdiag::FalseTrackConfig def;
        m_spinMinQuality->setValue(def.minQuality);
        m_spinMinAsscPct->setValue(int(std::lround(def.minAsscRatio * 100.0)));
        m_spinMinAsscPts->setValue(def.minAsscPoints);
        m_spinMaxSuccLoss->setValue(def.maxSuccLoss);
        m_spinMinSnr->setValue(def.minMeanSnrDb);
        m_updatingSandbox = false;
        syncConfigFromSandbox();
        recomputeFalseTracks();
    });

    // ---- 2. 疑点航迹列表 ----
    m_falseTable = new QTableWidget(page);
    m_falseTable->setObjectName(QStringLiteral("falseTrackTable"));
    configureCompactTable(m_falseTable,
                          QStringList() << QStringLiteral("批号")
                                        << QStringLiteral("疑点")
                                        << QStringLiteral("质量")
                                        << QStringLiteral("关联")
                                        << QStringLiteral("连丢")
                                        << QStringLiteral("标签"),
                          150);
    m_falseTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_falseTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_falseTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_falseTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_falseTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_falseTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    lay->addWidget(m_falseTable, 1);

    connect(m_falseTable, &QTableWidget::currentCellChanged, this, [this](int curRow, int, int, int) {
        if (curRow >= 0 && curRow < m_falseItems.size())
            updateFalseAutopsyCard(curRow);
    });
    connect(m_falseTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (row >= 0 && row < m_falseItems.size()) {
            QSet<int> set;
            set.insert(m_falseItems.at(row).trackIndex);
            emit isolateTracksRequested(set, true);
        }
    });

    // ---- 3. 底部「单批尸检卡」 ----
    QFrame *card = new QFrame(page);
    card->setObjectName(QStringLiteral("diagCard"));
    QVBoxLayout *cardLay = new QVBoxLayout(card);
    ui::bindMargins(cardLay, 6, 6, 6, 6);
    ui::bindSpacing(cardLay, 5);

    m_autopsyTitle = new QLabel(QStringLiteral("单批尸检卡 · 请选择上方航迹"), card);
    m_autopsyTitle->setObjectName(QStringLiteral("diagSectionTitle"));
    m_autopsyTitle->setWordWrap(true);
    cardLay->addWidget(m_autopsyTitle);

    QHBoxLayout *ribHdr = new QHBoxLayout;
    ribHdr->setContentsMargins(0, 0, 0, 0);
    QLabel *ribLab = new QLabel(QStringLiteral("实测/外推色带"), card);
    ribLab->setObjectName(QStringLiteral("diagMuted"));
    QLabel *ribLeg = new QLabel(QStringLiteral("<span style='color:#22c55e'>■</span>实测 &nbsp;<span style='color:#6b7280'>■</span>外推"), card);
    ribLeg->setObjectName(QStringLiteral("diagMuted"));
    ribHdr->addWidget(ribLab);
    ribHdr->addStretch(1);
    ribHdr->addWidget(ribLeg);
    cardLay->addLayout(ribHdr);

    m_asscRibbon = new diagui::AsscRibbonWidget(card);
    m_asscRibbon->setObjectName(QStringLiteral("asscRibbon"));
    m_asscRibbon->setOnSeek([this](qint64 tMs) { emit replaySeekRequested(tMs); });
    cardLay->addWidget(m_asscRibbon);

    m_autopsySummary = new QLabel(QStringLiteral("未选中航迹"), card);
    m_autopsySummary->setObjectName(QStringLiteral("diagMono"));
    m_autopsySummary->setWordWrap(true);
    cardLay->addWidget(m_autopsySummary);

    m_lifeChart = new TimeSeriesChart(card);
    m_lifeChart->setObjectName(QStringLiteral("lifeChart"));
    ui::bindSize(m_lifeChart, ui::SizeKind::FixedHeight, 138);
    m_lifeChart->setMinimumWidth(0);
    connect(m_lifeChart, &TimeSeriesChart::timePicked, this, &DiagnosticsPanel::replaySeekRequested);
    cardLay->addWidget(m_lifeChart);

    QGridLayout *btnGrid = new QGridLayout;
    btnGrid->setContentsMargins(0, 0, 0, 0);
    ui::bindSpacing(btnGrid, 4);

    m_btnFocusTrack = new QPushButton(QStringLiteral("独显聚焦"), card);
    m_btnFocusTrack->setObjectName(QStringLiteral("diagPrimaryBtn"));
    m_btnFilterOverlay = new QPushButton(QStringLiteral("开启滤波叠加"), card);
    m_btnFilterOverlay->setObjectName(QStringLiteral("diagBtn"));
    m_btnFilterOverlay->setCheckable(true);
    m_btnPoint3dBox = new QPushButton(QStringLiteral("点云局部盒"), card);
    m_btnPoint3dBox->setObjectName(QStringLiteral("diagBtn"));
    m_btnRestoreAll = new QPushButton(QStringLiteral("恢复全部"), card);
    m_btnRestoreAll->setObjectName(QStringLiteral("diagBtn"));

    for (QPushButton *b : { m_btnFocusTrack, m_btnFilterOverlay, m_btnPoint3dBox, m_btnRestoreAll }) {
        b->setCursor(Qt::PointingHandCursor);
        b->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }
    btnGrid->addWidget(m_btnFocusTrack, 0, 0);
    btnGrid->addWidget(m_btnFilterOverlay, 0, 1);
    btnGrid->addWidget(m_btnPoint3dBox, 1, 0);
    btnGrid->addWidget(m_btnRestoreAll, 1, 1);
    cardLay->addLayout(btnGrid);

    connect(m_btnFocusTrack, &QPushButton::clicked, this, [this]() {
        if (m_selectedFalseRow < 0 || m_selectedFalseRow >= m_falseItems.size())
            return;
        QSet<int> set;
        set.insert(m_falseItems.at(m_selectedFalseRow).trackIndex);
        emit isolateTracksRequested(set, true);
    });
    connect(m_btnFilterOverlay, &QPushButton::toggled, this, [this](bool on) {
        m_btnFilterOverlay->setText(on ? QStringLiteral("关闭滤波叠加") : QStringLiteral("开启滤波叠加"));
        emit filterOverlayRequested(on);
    });
    connect(m_btnPoint3dBox, &QPushButton::clicked, this, [this]() {
        if (m_selectedFalseRow < 0 || m_selectedFalseRow >= m_falseItems.size())
            return;
        emitPoint3dBoxForTrack(m_falseItems.at(m_selectedFalseRow).trackIndex);
    });
    connect(m_btnRestoreAll, &QPushButton::clicked, this, [this]() {
        if (m_btnFilterOverlay->isChecked()) {
            QSignalBlocker blk(m_btnFilterOverlay);
            m_btnFilterOverlay->setChecked(false);
            m_btnFilterOverlay->setText(QStringLiteral("开启滤波叠加"));
        }
        emit filterOverlayRequested(false);
        emit gateAutopsyOverlayChanged(trackdiag::GateAutopsyResult(), false);
        emit fragmentOverlayChanged(-1, -1, false);
        emit isolateTracksRequested(QSet<int>(), false);
    });

    lay->addWidget(card);
    return page;
}

QWidget *DiagnosticsPanel::buildDisappearTab()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *lay = new QVBoxLayout(page);
    lay->setContentsMargins(0, 0, 0, 0);
    ui::bindSpacing(lay, 6);

    // ---- 顶部工具条：DetPoint 状态 + 加载按钮 + 重新扫描 ----
    QFrame *topBox = new QFrame(page);
    topBox->setObjectName(QStringLiteral("diagBox"));
    QVBoxLayout *topLay = new QVBoxLayout(topBox);
    ui::bindMargins(topLay, 6, 5, 6, 6);
    ui::bindSpacing(topLay, 4);

    m_detStatusLabel = new QLabel(QStringLiteral("DetPoint：未加载"), topBox);
    m_detStatusLabel->setObjectName(QStringLiteral("diagMono"));
    m_detStatusLabel->setWordWrap(true);
    topLay->addWidget(m_detStatusLabel);

    QHBoxLayout *btnRow = new QHBoxLayout;
    btnRow->setContentsMargins(0, 0, 0, 0);
    ui::bindSpacing(btnRow, 4);
    QPushButton *loadDetBtn = new QPushButton(QStringLiteral("加载 DetPoint 目录…"), topBox);
    loadDetBtn->setObjectName(QStringLiteral("diagPrimaryBtn"));
    loadDetBtn->setCursor(Qt::PointingHandCursor);
    loadDetBtn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    QPushButton *rescanBtn = new QPushButton(QStringLiteral("重新扫描"), topBox);
    rescanBtn->setObjectName(QStringLiteral("diagBtn"));
    rescanBtn->setCursor(Qt::PointingHandCursor);
    btnRow->addWidget(loadDetBtn, 1);
    btnRow->addWidget(rescanBtn);
    topLay->addLayout(btnRow);
    lay->addWidget(topBox);

    connect(loadDetBtn, &QPushButton::clicked, this, &DiagnosticsPanel::loadDetPointsRequested);
    connect(rescanBtn, &QPushButton::clicked, this, &DiagnosticsPanel::refreshAll);

    // ---- 分段按钮切换 3 个子视图 ----
    QWidget *segBar = new QWidget(page);
    segBar->setProperty("segmented", true);
    segBar->setAttribute(Qt::WA_StyledBackground);
    QHBoxLayout *segLay = new QHBoxLayout(segBar);
    ui::bindMargins(segLay, 2, 2, 2, 2);
    ui::bindSpacing(segLay, 2);

    m_subViewGroup = new QButtonGroup(segBar);
    m_subViewGroup->setExclusive(true);
    static const char *const kSubTitles[3] = { "断航尸检", "断批配对", "漏批/漏斗" };
    static const char *const kSubTips[3] = {
        "异常断航检测与关联波门逐帧尸检",
        "疑似断批重起航迹对检测与桥接",
        "未关联强连续点迹链挖掘与四级漏斗探针"
    };
    for (int i = 0; i < 3; ++i) {
        QPushButton *b = new QPushButton(QString::fromUtf8(kSubTitles[i]), segBar);
        b->setObjectName(QStringLiteral("diagSubBtn_%1").arg(i));
        b->setCheckable(true);
        b->setChecked(i == 0);
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(QString::fromUtf8(kSubTips[i]));
        b->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_subViewGroup->addButton(b, i);
        segLay->addWidget(b, 1);
        m_subViewBtns[i] = b;
        connect(b, &QPushButton::clicked, this, [this, i]() { setDisappearSubView(i); });
    }
    lay->addWidget(segBar);

    // ---- 3 个子视图堆叠 ----
    m_disappearStack = new QStackedWidget(page);
    m_disappearStack->setObjectName(QStringLiteral("disappearStack"));
    m_disappearStack->addWidget(buildBrokenSubView());
    m_disappearStack->addWidget(buildFragmentSubView());
    m_disappearStack->addWidget(buildOrphanFunnelSubView());
    lay->addWidget(m_disappearStack, 1);

    return page;
}

QWidget *DiagnosticsPanel::buildBrokenSubView()
{
    QWidget *w = new QWidget(this);
    QVBoxLayout *lay = new QVBoxLayout(w);
    lay->setContentsMargins(0, 0, 0, 0);
    ui::bindSpacing(lay, 5);

    m_brokenCountLabel = new QLabel(QStringLiteral("异常断航：0 条"), w);
    m_brokenCountLabel->setObjectName(QStringLiteral("diagMuted"));
    lay->addWidget(m_brokenCountLabel);

    m_brokenTable = new QTableWidget(w);
    m_brokenTable->setObjectName(QStringLiteral("brokenTrackTable"));
    configureCompactTable(m_brokenTable,
                          QStringList() << QStringLiteral("批号")
                                        << QStringLiteral("扇区")
                                        << QStringLiteral("连丢")
                                        << QStringLiteral("末质")
                                        << QStringLiteral("推测原因"),
                          135);
    m_brokenTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_brokenTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_brokenTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_brokenTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_brokenTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    lay->addWidget(m_brokenTable);

    connect(m_brokenTable, &QTableWidget::cellClicked, this, [this](int row, int) {
        selectBrokenRow(row);
    });

    m_autopsyCauseLabel = new QLabel(QStringLiteral("主要死因：点击上方异常断航执行波门尸检"), w);
    m_autopsyCauseLabel->setObjectName(QStringLiteral("diagCause"));
    m_autopsyCauseLabel->setWordWrap(true);
    lay->addWidget(m_autopsyCauseLabel);

    m_gateTable = new QTableWidget(w);
    m_gateTable->setObjectName(QStringLiteral("gateAutopsyTable"));
    configureCompactTable(m_gateTable,
                          QStringList() << QStringLiteral("帧号")
                                        << QStringLiteral("R/Az/Vr")
                                        << QStringLiteral("SNR")
                                        << QStringLiteral("ΔR/ΔAz/ΔVr")
                                        << QStringLiteral("死因判定"),
                          145);
    m_gateTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_gateTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_gateTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_gateTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_gateTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    lay->addWidget(m_gateTable, 1);

    connect(m_gateTable, &QTableWidget::cellClicked, this, [this](int row, int) {
        QTableWidgetItem *it = m_gateTable->item(row, 0);
        if (!it)
            return;
        const qint64 tMs = it->data(Qt::UserRole).toLongLong();
        if (tMs > 0)
            emit replaySeekRequested(tMs);
    });

    QHBoxLayout *actRow = new QHBoxLayout;
    actRow->setContentsMargins(0, 0, 0, 0);
    ui::bindSpacing(actRow, 4);
    m_btnBrokenFocus = new QPushButton(QStringLiteral("独显聚焦"), w);
    m_btnBrokenFocus->setObjectName(QStringLiteral("diagPrimaryBtn"));
    m_btnBrokenBox = new QPushButton(QStringLiteral("点云波门盒"), w);
    m_btnBrokenBox->setObjectName(QStringLiteral("diagBtn"));
    m_btnClearGate = new QPushButton(QStringLiteral("清除波门"), w);
    m_btnClearGate->setObjectName(QStringLiteral("diagBtn"));
    for (QPushButton *b : { m_btnBrokenFocus, m_btnBrokenBox, m_btnClearGate }) {
        b->setCursor(Qt::PointingHandCursor);
        b->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        actRow->addWidget(b, 1);
    }
    lay->addLayout(actRow);

    connect(m_btnBrokenFocus, &QPushButton::clicked, this, [this]() {
        if (m_selectedBrokenRow < 0 || m_selectedBrokenRow >= m_brokenItems.size())
            return;
        QSet<int> set;
        set.insert(m_brokenItems.at(m_selectedBrokenRow).trackIndex);
        emit isolateTracksRequested(set, true);
    });
    connect(m_btnBrokenBox, &QPushButton::clicked, this, [this]() {
        if (!m_lastAutopsy.frames.isEmpty())
            emitPoint3dBoxForGate(m_lastAutopsy);
        else if (m_selectedBrokenRow >= 0 && m_selectedBrokenRow < m_brokenItems.size())
            emitPoint3dBoxForTrack(m_brokenItems.at(m_selectedBrokenRow).trackIndex);
    });
    connect(m_btnClearGate, &QPushButton::clicked, this, [this]() {
        emit gateAutopsyOverlayChanged(trackdiag::GateAutopsyResult(), false);
    });

    return w;
}

QWidget *DiagnosticsPanel::buildFragmentSubView()
{
    QWidget *w = new QWidget(this);
    QVBoxLayout *lay = new QVBoxLayout(w);
    lay->setContentsMargins(0, 0, 0, 0);
    ui::bindSpacing(lay, 5);

    m_fragmentCountLabel = new QLabel(QStringLiteral("疑似断批重起配对：0 对"), w);
    m_fragmentCountLabel->setObjectName(QStringLiteral("diagMuted"));
    lay->addWidget(m_fragmentCountLabel);

    m_fragmentTable = new QTableWidget(w);
    m_fragmentTable->setObjectName(QStringLiteral("fragmentPairTable"));
    configureCompactTable(m_fragmentTable,
                          QStringList() << QStringLiteral("配对 (A → B)")
                                        << QStringLiteral("间隔s")
                                        << QStringLiteral("外推km")
                                        << QStringLiteral("ΔVr")
                                        << QStringLiteral("评分"),
                          190);
    m_fragmentTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_fragmentTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_fragmentTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_fragmentTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_fragmentTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    lay->addWidget(m_fragmentTable, 1);

    connect(m_fragmentTable, &QTableWidget::cellClicked, this, [this](int row, int) {
        selectFragmentRow(row);
    });

    m_fragmentDetailLabel = new QLabel(QStringLiteral("点击上方配对行，在地图双选隔离并绘制桥接虚线"), w);
    m_fragmentDetailLabel->setObjectName(QStringLiteral("diagCause"));
    m_fragmentDetailLabel->setWordWrap(true);
    lay->addWidget(m_fragmentDetailLabel);

    QHBoxLayout *actRow = new QHBoxLayout;
    actRow->setContentsMargins(0, 0, 0, 0);
    ui::bindSpacing(actRow, 4);
    m_btnFragmentSeek = new QPushButton(QStringLiteral("跳至断点时刻"), w);
    m_btnFragmentSeek->setObjectName(QStringLiteral("diagPrimaryBtn"));
    m_btnClearFragment = new QPushButton(QStringLiteral("清除桥接并恢复"), w);
    m_btnClearFragment->setObjectName(QStringLiteral("diagBtn"));
    for (QPushButton *b : { m_btnFragmentSeek, m_btnClearFragment }) {
        b->setCursor(Qt::PointingHandCursor);
        b->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        actRow->addWidget(b, 1);
    }
    lay->addLayout(actRow);

    connect(m_btnFragmentSeek, &QPushButton::clicked, this, [this]() {
        if (!m_store || m_selectedFragmentRow < 0 || m_selectedFragmentRow >= m_fragmentPairs.size())
            return;
        const int idxA = m_fragmentPairs.at(m_selectedFragmentRow).firstIndex;
        if (idxA >= 0 && idxA < m_store->size())
            emit replaySeekRequested(m_store->tracks().at(idxA).maxTime());
    });
    connect(m_btnClearFragment, &QPushButton::clicked, this, [this]() {
        emit fragmentOverlayChanged(-1, -1, false);
        emit isolateTracksRequested(QSet<int>(), false);
    });

    return w;
}

QWidget *DiagnosticsPanel::buildOrphanFunnelSubView()
{
    QWidget *w = new QWidget(this);
    QVBoxLayout *lay = new QVBoxLayout(w);
    lay->setContentsMargins(0, 0, 0, 0);
    ui::bindSpacing(lay, 6);

    // ---- 上半部：漏批挖掘（未关联强连续点迹链） ----
    m_orphanCountLabel = new QLabel(QStringLiteral("未关联强连续点迹链：0 条"), w);
    m_orphanCountLabel->setObjectName(QStringLiteral("diagSectionTitle"));
    m_orphanCountLabel->setWordWrap(true);
    lay->addWidget(m_orphanCountLabel);

    m_orphanTable = new QTableWidget(w);
    m_orphanTable->setObjectName(QStringLiteral("orphanClusterTable"));
    configureCompactTable(m_orphanTable,
                          QStringList() << QStringLiteral("簇")
                                        << QStringLiteral("点数")
                                        << QStringLiteral("帧范围")
                                        << QStringLiteral("均R/Az/Vr")
                                        << QStringLiteral("SNR"),
                          135);
    m_orphanTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_orphanTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_orphanTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_orphanTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_orphanTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    lay->addWidget(m_orphanTable);

    connect(m_orphanTable, &QTableWidget::cellClicked, this, [this](int row, int) {
        selectOrphanRow(row);
    });

    // ---- 下半部：四级漏斗探针 ----
    QFrame *funnelCard = new QFrame(w);
    funnelCard->setObjectName(QStringLiteral("diagCard"));
    QVBoxLayout *fLay = new QVBoxLayout(funnelCard);
    ui::bindMargins(fLay, 6, 6, 6, 6);
    ui::bindSpacing(fLay, 4);

    QLabel *fTitle = new QLabel(QStringLiteral("四级漏斗探针（L1检测 → L2临时 → L3关联 → L4平滑）"), funnelCard);
    fTitle->setObjectName(QStringLiteral("diagSectionTitle"));
    fTitle->setWordWrap(true);
    fLay->addWidget(fTitle);

    QHBoxLayout *selRow = new QHBoxLayout;
    selRow->setContentsMargins(0, 0, 0, 0);
    ui::bindSpacing(selRow, 4);
    m_funnelTrackCombo = new QComboBox(funnelCard);
    m_funnelTrackCombo->setObjectName(QStringLiteral("diagCombo"));
    m_funnelTrackCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_funnelTrackCombo->setMinimumContentsLength(8);
    m_funnelTrackCombo->setMinimumWidth(0);
    QPushButton *probeBtn = new QPushButton(QStringLiteral("探测"), funnelCard);
    probeBtn->setObjectName(QStringLiteral("diagBtn"));
    probeBtn->setCursor(Qt::PointingHandCursor);
    selRow->addWidget(m_funnelTrackCombo, 1);
    selRow->addWidget(probeBtn);
    fLay->addLayout(selRow);

    m_funnelSummaryLabel = new QLabel(QStringLiteral("请选择参考航迹以运行四级漏斗分析"), funnelCard);
    m_funnelSummaryLabel->setObjectName(QStringLiteral("diagCause"));
    m_funnelSummaryLabel->setWordWrap(true);
    fLay->addWidget(m_funnelSummaryLabel);

    m_funnelTable = new QTableWidget(funnelCard);
    m_funnelTable->setObjectName(QStringLiteral("funnelTable"));
    configureCompactTable(m_funnelTable,
                          QStringList() << QStringLiteral("帧")
                                        << QStringLiteral("L1")
                                        << QStringLiteral("L2")
                                        << QStringLiteral("L3")
                                        << QStringLiteral("L4")
                                        << QStringLiteral("SNR/距"),
                          135);
    m_funnelTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_funnelTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_funnelTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_funnelTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_funnelTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_funnelTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    fLay->addWidget(m_funnelTable, 1);

    connect(m_funnelTrackCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (!m_populatingFunnelCombo)
            runFunnelForSelectedTrack();
    });
    connect(probeBtn, &QPushButton::clicked, this, &DiagnosticsPanel::runFunnelForSelectedTrack);
    connect(m_funnelTable, &QTableWidget::cellClicked, this, [this](int row, int) {
        if (row >= 0 && row < m_funnelStats.size() && m_funnelStats.at(row).timeMs > 0)
            emit replaySeekRequested(m_funnelStats.at(row).timeMs);
    });

    lay->addWidget(funnelCard, 1);
    return w;
}

// ---------------------------------------------------------------------------
//  数据刷新与 Tab 1「虚假航迹」逻辑
// ---------------------------------------------------------------------------

void DiagnosticsPanel::refreshAll()
{
    syncConfigFromSandbox();
    recomputeFalseTracks();
    updateDetStatusUi();
    recomputeDisappearViews();
}

void DiagnosticsPanel::syncConfigFromSandbox()
{
    if (!m_spinMinQuality)
        return;
    m_falseCfg.minQuality = m_spinMinQuality->value();
    m_falseCfg.minAsscRatio = double(m_spinMinAsscPct->value()) / 100.0;
    m_falseCfg.minAsscPoints = m_spinMinAsscPts->value();
    m_falseCfg.maxSuccLoss = m_spinMaxSuccLoss->value();
    m_falseCfg.minMeanSnrDb = m_spinMinSnr->value();
}

void DiagnosticsPanel::recomputeFalseTracks()
{
    QString prevKey;
    if (m_selectedFalseRow >= 0 && m_selectedFalseRow < m_falseItems.size())
        prevKey = m_falseItems.at(m_selectedFalseRow).trackKey;

    m_falseItems.clear();
    if (m_store && m_store->size() > 0)
        m_falseItems = trackdiag::evaluateFalseTracks(*m_store, effectiveBundle(), m_falseCfg);

    int filteredCnt = 0;
    for (const trackdiag::FalseTrackItem &it : qAsConst(m_falseItems)) {
        if (it.filteredOut)
            ++filteredCnt;
    }
    if (m_falseStatsLabel) {
        m_falseStatsLabel->setText(QStringLiteral("拟滤除 %1 / 共 %2 条")
                                       .arg(filteredCnt).arg(m_falseItems.size()));
    }

    QSignalBlocker blk(m_falseTable);
    m_falseTable->setRowCount(m_falseItems.size());
    int restoredRow = -1;
    const QColor colErr = themeColor("error");
    const QColor colWarn = themeColor("warning");
    const QColor colMuted = themeColor("text-tertiary");

    for (int i = 0; i < m_falseItems.size(); ++i) {
        const trackdiag::FalseTrackItem &it = m_falseItems.at(i);
        if (!prevKey.isEmpty() && it.trackKey == prevKey)
            restoredRow = i;

        const QString reasonText = it.reasons.isEmpty() ? QStringLiteral("正常") : it.reasons.join(QStringLiteral("、"));
        const QString tip = QStringLiteral("%1 | 疑点分 %2 | 质量 %3 | 关联 %4/%5 (%6%) | 连丢 %7 | 均SNR %8 | %9")
                                .arg(it.displayId)
                                .arg(it.suspicionScore, 0, 'f', 1)
                                .arg(it.quality, 0, 'f', 2)
                                .arg(it.asscCount)
                                .arg(it.totalCount)
                                .arg(it.asscRatio * 100.0, 0, 'f', 0)
                                .arg(it.succLoss)
                                .arg(fmtOpt(it.meanSnr, 1, QStringLiteral("dB")))
                                .arg(reasonText);

        QTableWidgetItem *cId = makeItem(it.displayId, Qt::AlignLeft | Qt::AlignVCenter, tip);
        QTableWidgetItem *cScore = makeItem(QString::number(int(std::lround(it.suspicionScore))),
                                            Qt::AlignRight | Qt::AlignVCenter, tip);
        QTableWidgetItem *cQ = makeItem(QString::number(it.quality, 'f', 2),
                                        Qt::AlignRight | Qt::AlignVCenter, tip);
        QTableWidgetItem *cAssc = makeItem(QStringLiteral("%1%").arg(int(std::lround(it.asscRatio * 100.0))),
                                           Qt::AlignRight | Qt::AlignVCenter, tip);
        QTableWidgetItem *cLoss = makeItem(QString::number(it.succLoss),
                                           Qt::AlignRight | Qt::AlignVCenter, tip);
        QTableWidgetItem *cTag = makeItem(reasonText, Qt::AlignLeft | Qt::AlignVCenter, tip);

        if (it.suspicionScore >= 60.0)
            cScore->setForeground(colErr);
        else if (it.suspicionScore >= 35.0)
            cScore->setForeground(colWarn);

        if (it.filteredOut) {
            cId->setForeground(colErr);
            cTag->setForeground(colWarn);
        } else if (it.reasons.isEmpty()) {
            cTag->setForeground(colMuted);
        }

        m_falseTable->setItem(i, 0, cId);
        m_falseTable->setItem(i, 1, cScore);
        m_falseTable->setItem(i, 2, cQ);
        m_falseTable->setItem(i, 3, cAssc);
        m_falseTable->setItem(i, 4, cLoss);
        m_falseTable->setItem(i, 5, cTag);
    }

    if (m_falseItems.isEmpty()) {
        m_selectedFalseRow = -1;
        updateFalseAutopsyCard(-1);
    } else {
        const int target = restoredRow >= 0 ? restoredRow : 0;
        m_falseTable->selectRow(target);
        updateFalseAutopsyCard(target);
    }

    emitCurrentMutedTracks();
}

void DiagnosticsPanel::emitCurrentMutedTracks()
{
    QSet<int> muted;
    if (m_chkMuteFiltered && m_chkMuteFiltered->isChecked()) {
        for (const trackdiag::FalseTrackItem &it : qAsConst(m_falseItems)) {
            if (it.filteredOut && it.trackIndex >= 0)
                muted.insert(it.trackIndex);
        }
    }
    emit mutedTracksChanged(muted);
}

void DiagnosticsPanel::updateFalseAutopsyCard(int row)
{
    m_selectedFalseRow = row;
    const bool valid = (m_store != nullptr && row >= 0 && row < m_falseItems.size());
    m_btnFocusTrack->setEnabled(valid);
    m_btnPoint3dBox->setEnabled(valid);

    if (!valid) {
        m_autopsyTitle->setText(QStringLiteral("单批尸检卡 · 请选择上方航迹"));
        m_asscRibbon->clear();
        m_autopsySummary->setText(QStringLiteral("暂无数据"));
        m_lifeChart->clear();
        m_lifeChart->setTitle(QStringLiteral("质量 / SNR 演变"), QString());
        return;
    }

    const trackdiag::FalseTrackItem &it = m_falseItems.at(row);
    if (it.trackIndex < 0 || it.trackIndex >= m_store->size())
        return;
    const Track &tr = m_store->tracks().at(it.trackIndex);
    const trackdiag::TrackDiagMeta *meta = effectiveBundle().findMeta(tr);

    const QString typeStr = (meta && meta->valid)
        ? trackdiag::trackTypeName(meta->trackType)
        : trackSourceName(tr.source);
    m_autopsyTitle->setText(QStringLiteral("%1 · %2 · 疑点 %3%4")
                                .arg(it.displayId, typeStr)
                                .arg(it.suspicionScore, 0, 'f', 0)
                                .arg(it.filteredOut ? QStringLiteral(" [拟滤除]") : QString()));

    // ---- 1. 实测/外推色带条 ----
    QVector<diagui::AsscRibbonWidget::Segment> segs;
    if (meta && meta->valid && !meta->asscFlags.isEmpty()) {
        const int n = meta->asscFlags.size();
        segs.reserve(n);
        for (int k = 0; k < n; ++k) {
            const int fid = (k < meta->smoothFrameIds.size() && meta->smoothFrameIds.at(k) > 0)
                ? meta->smoothFrameIds.at(k) : (k + 1);
            const qint64 tMs = (k < tr.points.size()) ? tr.points.at(k).t : 0;
            segs.append(diagui::AsscRibbonWidget::Segment(fid, tMs, meta->asscFlags.at(k) != 0));
        }
    } else if (!tr.points.isEmpty()) {
        // 无 MAT 元数据时，按 rawPoints 时间戳退化重建色带
        segs.reserve(tr.points.size());
        for (int k = 0; k < tr.points.size(); ++k) {
            const qint64 tMs = tr.points.at(k).t;
            const bool assc = tr.hasRawPoints()
                ? (findNearestPt(tr.rawPoints, tMs, 2000) >= 0)
                : (tr.source == TrackSource::RadarRaw);
            segs.append(diagui::AsscRibbonWidget::Segment(k + 1, tMs, assc));
        }
    }
    m_asscRibbon->setSegments(segs);

    // ---- 2. 起批信息与关键指标摘要 ----
    QStringList lines;
    if (meta && meta->valid) {
        lines << QStringLiteral("起批: 帧 %1→%2 (起批关联 %3, 初始Q=%4) | 类型: %5")
                     .arg(meta->startFrame)
                     .arg(meta->confirmFrame)
                     .arg(meta->startAsscCnt)
                     .arg(meta->startQuality, 0, 'f', 2)
                     .arg(trackdiag::trackTypeName(meta->trackType));
    } else {
        lines << QStringLiteral("起批: 无 MAT 扩展元数据（基于轨迹点退化评估） | 来源: %1")
                     .arg(trackSourceName(tr.source));
    }
    lines << QStringLiteral("航程: %1 km | 均SNR: %2 | 均新息: %3")
                 .arg(it.travelLenKm, 0, 'f', 1)
                 .arg(fmtOpt(it.meanSnr, 1, QStringLiteral(" dB")))
                 .arg(fmtOpt(it.meanInnovKm, 2, QStringLiteral(" km")));
    lines << QStringLiteral("实测/总点: %1/%2 (%3%) | 最大连丢: %4 | 质量: %5")
                 .arg(it.asscCount)
                 .arg(it.totalCount)
                 .arg(it.asscRatio * 100.0, 0, 'f', 0)
                 .arg(it.succLoss)
                 .arg(it.quality, 0, 'f', 2);
    if (meta && meta->valid && meta->gate.valid) {
        lines << QStringLiteral("波门: R=%1(+%2) A=%3°(+%4) V=%5(+%6)")
                     .arg(meta->gate.fixedR, 0, 'f', 1)
                     .arg(meta->gate.floatR, 0, 'f', 1)
                     .arg(meta->gate.fixedA, 0, 'f', 2)
                     .arg(meta->gate.floatA, 0, 'f', 2)
                     .arg(meta->gate.fixedV, 0, 'f', 1)
                     .arg(meta->gate.floatV, 0, 'f', 1);
    }
    if (it.multipathPeerIndex >= 0 && it.multipathPeerIndex < m_store->size()) {
        lines << QStringLiteral("多径镜像对: %1").arg(m_store->tracks().at(it.multipathPeerIndex).id);
    }
    if (!it.reasons.isEmpty())
        lines << QStringLiteral("触发标签: %1").arg(it.reasons.join(QStringLiteral("、")));
    m_autopsySummary->setText(lines.join(QLatin1Char('\n')));

    // ---- 3. 质量演变曲线（无 lifeList 时回退 SNR 或 新息曲线） ----
    QVector<TimeSeriesChart::Series> chartSeries;
    if (meta && meta->valid && !meta->life.isEmpty()) {
        QHash<int, qint64> frameTime;
        const int ns = qMin(meta->smoothFrameIds.size(), tr.points.size());
        for (int k = 0; k < ns; ++k) {
            if (meta->smoothFrameIds.at(k) > 0)
                frameTime.insert(meta->smoothFrameIds.at(k), tr.points.at(k).t);
        }
        const int na = qMin(meta->asscFrameIds.size(), tr.rawPoints.size());
        for (int k = 0; k < na; ++k) {
            if (meta->asscFrameIds.at(k) > 0 && !frameTime.contains(meta->asscFrameIds.at(k)))
                frameTime.insert(meta->asscFrameIds.at(k), tr.rawPoints.at(k).t);
        }

        const qint64 t0 = tr.minTime();
        const qint64 t1 = tr.maxTime();
        const int nLife = meta->life.size();
        const int fMin = meta->life.first().frameId;
        const int fMax = meta->life.last().frameId;

        QVector<QPointF> curPts;
        QVector<QPointF> prevPts;
        curPts.reserve(nLife);
        prevPts.reserve(nLife);
        bool hasPrevDiff = false;
        for (int k = 0; k < nLife; ++k) {
            const trackdiag::TrackLifeRow &lr = meta->life.at(k);
            qint64 tm = frameTime.value(lr.frameId, 0);
            if (tm <= 0) {
                if (t1 > t0) {
                    const double frac = (fMax > fMin)
                        ? qBound(0.0, double(lr.frameId - fMin) / double(fMax - fMin), 1.0)
                        : (nLife > 1 ? double(k) / double(nLife - 1) : 0.0);
                    tm = t0 + qint64(std::llround(frac * double(t1 - t0)));
                } else {
                    tm = t0 + qint64(k) * 10000;
                }
            }
            curPts.append(QPointF(double(tm), lr.curQuality));
            prevPts.append(QPointF(double(tm), lr.prevQuality));
            if (std::fabs(lr.curQuality - lr.prevQuality) > 1e-4)
                hasPrevDiff = true;
        }
        m_lifeChart->setTitle(QStringLiteral("生命周期质量演变"), QStringLiteral("Q"));
        chartSeries.append(TimeSeriesChart::Series(
            QStringLiteral("当前Q"), QColor(0x38, 0xbd, 0xf8), TimeSeriesChart::SeriesStyle::SolidLine, curPts));
        if (hasPrevDiff) {
            chartSeries.append(TimeSeriesChart::Series(
                QStringLiteral("前帧Q"), QColor(0xf5, 0x9e, 0x0b), TimeSeriesChart::SeriesStyle::DashLine, prevPts));
        }
    } else {
        // 退化分支 1：SNR 曲线
        const QVector<TrackPoint> &snrSrc = tr.hasRawPoints() ? tr.rawPoints : tr.points;
        QVector<QPointF> snrPts;
        snrPts.reserve(snrSrc.size());
        for (const TrackPoint &pt : snrSrc) {
            if (std::isfinite(pt.snr))
                snrPts.append(QPointF(double(pt.t), double(pt.snr)));
        }
        if (!snrPts.isEmpty()) {
            m_lifeChart->setTitle(QStringLiteral("实测信噪比演变"), QStringLiteral("dB"));
            chartSeries.append(TimeSeriesChart::Series(
                QStringLiteral("SNR"), QColor(0x22, 0xc5, 0x5e), TimeSeriesChart::SeriesStyle::SolidLine, snrPts));
        } else if (!tr.predPoints.isEmpty() && !tr.points.isEmpty()) {
            // 退化分支 2：新息残差曲线
            QVector<QPointF> innovPts;
            for (const TrackPoint &sp : tr.points) {
                const int pi = findNearestPt(tr.predPoints, sp.t, 2000);
                if (pi >= 0) {
                    const double dKm = geocalc::vincentyKm(sp.lat, sp.lon,
                                                           tr.predPoints.at(pi).lat, tr.predPoints.at(pi).lon);
                    if (std::isfinite(dKm))
                        innovPts.append(QPointF(double(sp.t), dKm));
                }
            }
            m_lifeChart->setTitle(QStringLiteral("预测新息误差"), QStringLiteral("km"));
            if (!innovPts.isEmpty()) {
                chartSeries.append(TimeSeriesChart::Series(
                    QStringLiteral("新息"), QColor(0xf4, 0x3f, 0x5e), TimeSeriesChart::SeriesStyle::SolidLine, innovPts));
            }
        } else {
            m_lifeChart->setTitle(QStringLiteral("质量 / SNR 曲线"), QString());
        }
    }

    if (chartSeries.isEmpty())
        m_lifeChart->clear();
    else
        m_lifeChart->setSeries(chartSeries);
}

void DiagnosticsPanel::emitPoint3dBoxForTrack(int trackIndex)
{
    if (!m_store || trackIndex < 0 || trackIndex >= m_store->size())
        return;
    const Track &tr = m_store->tracks().at(trackIndex);

    double rMin = std::numeric_limits<double>::infinity();
    double rMax = -std::numeric_limits<double>::infinity();
    double vMin = std::numeric_limits<double>::infinity();
    double vMax = -std::numeric_limits<double>::infinity();
    double azMin = std::numeric_limits<double>::infinity();
    double azMax = -std::numeric_limits<double>::infinity();
    bool anyPolar = false;

    auto feed = [&](const QVector<TrackPoint> &pts) {
        for (const TrackPoint &pt : pts) {
            if (std::isfinite(pt.prange) && std::isfinite(pt.pvr) && std::isfinite(pt.paz)) {
                rMin = qMin(rMin, double(pt.prange));
                rMax = qMax(rMax, double(pt.prange));
                vMin = qMin(vMin, double(pt.pvr));
                vMax = qMax(vMax, double(pt.pvr));
                azMin = qMin(azMin, double(pt.paz));
                azMax = qMax(azMax, double(pt.paz));
                anyPolar = true;
            }
        }
    };
    feed(tr.points);
    feed(tr.rawPoints);
    feed(tr.predPoints);
    if (!anyPolar)
        return;

    const double rPad = qMax(15.0, (rMax - rMin) * 0.15);
    const double vPad = qMax(10.0, (vMax - vMin) * 0.15);
    const double azPad = qMax(2.0, (azMax - azMin) * 0.15);

    int fStart = 0;
    int fEnd = 0;
    if (m_detData && !m_detData->frames.isEmpty()) {
        const qint64 t0 = tr.minTime();
        const qint64 t1 = tr.maxTime();
        bool found = false;
        for (const DetPointFrame &df : m_detData->frames) {
            if (df.timeMs >= t0 - 15000 && df.timeMs <= t1 + 15000) {
                const int zv = int(std::lround(df.zValue));
                if (!found) {
                    fStart = fEnd = zv;
                    found = true;
                } else {
                    fStart = qMin(fStart, zv);
                    fEnd = qMax(fEnd, zv);
                }
            }
        }
        if (!found) {
            fStart = int(std::lround(m_detData->frames.first().zValue));
            fEnd = int(std::lround(m_detData->frames.last().zValue));
        }
    } else {
        const trackdiag::TrackDiagMeta *meta = effectiveBundle().findMeta(tr);
        if (meta && !meta->smoothFrameIds.isEmpty()) {
            fStart = meta->smoothFrameIds.first();
            fEnd = meta->smoothFrameIds.last();
        } else {
            fStart = 1;
            fEnd = qMax(2, tr.points.size());
        }
    }
    if (fEnd <= fStart) {
        fStart -= 2;
        fEnd += 2;
    }

    emit focusPoint3dRegionRequested(rMin - rPad, rMax + rPad,
                                     vMin - vPad, vMax + vPad,
                                     azMin - azPad, azMax + azPad,
                                     fStart, fEnd);
}

// ---------------------------------------------------------------------------
//  Tab 2「消失/断航」刷新与三大子视图
// ---------------------------------------------------------------------------

void DiagnosticsPanel::updateDetStatusUi()
{
    if (!m_detStatusLabel)
        return;
    if (!m_detData || m_detData->frames.isEmpty()) {
        m_detStatusLabel->setText(QStringLiteral("DetPoint：未加载（点击下方按钮加载以启用完整波门尸检与漏批挖掘）"));
        return;
    }
    qint64 totalPts = 0;
    for (const DetPointFrame &f : m_detData->frames)
        totalPts += f.points.size();
    const qint64 t0 = m_detData->frames.first().timeMs;
    const qint64 t1 = m_detData->frames.last().timeMs;
    m_detStatusLabel->setText(QStringLiteral("DetPoint：已关联 %1 帧（%2 点）· %3~%4")
                                  .arg(m_detData->frames.size())
                                  .arg(totalPts)
                                  .arg(fmtClockShort(t0), fmtClockShort(t1)));
}

void DiagnosticsPanel::recomputeDisappearViews()
{
    const RadarSite *activeSite = m_sites ? m_sites->activeSite() : nullptr;

    m_brokenItems.clear();
    m_fragmentPairs.clear();
    m_orphanClusters.clear();
    m_lastAutopsy = trackdiag::GateAutopsyResult();
    m_selectedBrokenRow = -1;
    m_selectedFragmentRow = -1;
    m_selectedOrphanRow = -1;

    if (m_store && m_store->size() > 0) {
        m_brokenItems = trackdiag::findBrokenTracks(*m_store, effectiveBundle(), activeSite);
        m_fragmentPairs = trackdiag::findFragmentPairs(*m_store);
        if (m_detData && !m_detData->frames.isEmpty())
            m_orphanClusters = trackdiag::findOrphanDetClusters(*m_detData, *m_store, effectiveBundle());
    }

    rebuildBrokenTable();
    rebuildFragmentTable();
    rebuildOrphanTable();
    rebuildFunnelTrackCombo();
}

void DiagnosticsPanel::rebuildBrokenTable()
{
    int insideCnt = 0;
    for (const trackdiag::BrokenTrackItem &it : qAsConst(m_brokenItems)) {
        if (it.insideSector)
            ++insideCnt;
    }
    m_brokenCountLabel->setText(QStringLiteral("异常断航：%1 条（扇区内 %2 条）")
                                    .arg(m_brokenItems.size()).arg(insideCnt));

    QSignalBlocker blk(m_brokenTable);
    m_brokenTable->setRowCount(m_brokenItems.size());
    const QColor colErr = themeColor("error");
    const QColor colWarn = themeColor("warning");

    for (int i = 0; i < m_brokenItems.size(); ++i) {
        const trackdiag::BrokenTrackItem &it = m_brokenItems.at(i);
        const QString tip = QStringLiteral("%1 | 终止时间 %2 | 扇区内:%3 | 连丢:%4 | 末质:%5 | %6")
                                .arg(it.displayId,
                                     formatBeijingTime(it.endTimeMs),
                                     it.insideSector ? QStringLiteral("是") : QStringLiteral("否"))
                                .arg(it.succLoss)
                                .arg(it.endQuality, 0, 'f', 2)
                                .arg(it.likelyCause);

        QTableWidgetItem *cId = makeItem(it.displayId, Qt::AlignLeft | Qt::AlignVCenter, tip);
        QTableWidgetItem *cSec = makeItem(it.insideSector ? QStringLiteral("是") : QStringLiteral("否"),
                                          Qt::AlignCenter, tip);
        QTableWidgetItem *cLoss = makeItem(QString::number(it.succLoss),
                                           Qt::AlignRight | Qt::AlignVCenter, tip);
        QTableWidgetItem *cQ = makeItem(QString::number(it.endQuality, 'f', 2),
                                        Qt::AlignRight | Qt::AlignVCenter, tip);
        QTableWidgetItem *cCause = makeItem(it.likelyCause, Qt::AlignLeft | Qt::AlignVCenter, tip);
        if (it.insideSector && it.succLoss >= 3)
            cSec->setForeground(colErr);
        else if (it.insideSector)
            cSec->setForeground(colWarn);

        m_brokenTable->setItem(i, 0, cId);
        m_brokenTable->setItem(i, 1, cSec);
        m_brokenTable->setItem(i, 2, cLoss);
        m_brokenTable->setItem(i, 3, cQ);
        m_brokenTable->setItem(i, 4, cCause);
    }

    m_gateTable->setRowCount(0);
    m_autopsyCauseLabel->setProperty("level", QString());
    m_autopsyCauseLabel->setText(m_brokenItems.isEmpty()
                                     ? QStringLiteral("主要死因：当前未检出异常断航")
                                     : QStringLiteral("主要死因：点击上方异常断航执行波门尸检"));
    m_autopsyCauseLabel->style()->unpolish(m_autopsyCauseLabel);
    m_autopsyCauseLabel->style()->polish(m_autopsyCauseLabel);
    m_btnBrokenFocus->setEnabled(false);
    m_btnBrokenBox->setEnabled(false);
}

void DiagnosticsPanel::populateGateTable(const trackdiag::GateAutopsyResult &res)
{
    m_gateTable->setRowCount(0);
    const QColor colOk(0x22, 0xc5, 0x5e);
    const QColor colErr = themeColor("error");
    const QColor colWarn = themeColor("warning");
    const QColor colInfo = themeColor("info");
    const QColor colMuted = themeColor("text-tertiary");

    for (const trackdiag::GateAutopsyFrame &af : res.frames) {
        if (af.candidates.isEmpty()) {
            const int r = m_gateTable->rowCount();
            m_gateTable->insertRow(r);
            const QString predTxt = (std::isfinite(af.predR) && std::isfinite(af.predAz) && std::isfinite(af.predVr))
                ? QStringLiteral("预 %1/%2°/%3").arg(af.predR, 0, 'f', 1).arg(af.predAz, 0, 'f', 1).arg(af.predVr, 0, 'f', 1)
                : QStringLiteral("—");
            const QString gateTxt = QStringLiteral("±%1/%2°/%3")
                .arg(af.gateR, 0, 'f', 1).arg(af.gateAz, 0, 'f', 1).arg(af.gateVr, 0, 'f', 1);
            const QString tip = QStringLiteral("帧 #%1 | 时间 %2\n预测: %3 | 1x波门: %4\n结论: %5")
                                    .arg(af.frameId)
                                    .arg(formatBeijingTime(af.timeMs), predTxt, gateTxt, af.summary);

            QTableWidgetItem *cF = makeItem(QStringLiteral("#%1").arg(af.frameId), Qt::AlignRight | Qt::AlignVCenter, tip);
            cF->setData(Qt::UserRole, af.timeMs);
            QTableWidgetItem *cPos = makeItem(predTxt, Qt::AlignLeft | Qt::AlignVCenter, tip);
            QTableWidgetItem *cSnr = makeItem(QStringLiteral("—"), Qt::AlignRight | Qt::AlignVCenter, tip);
            QTableWidgetItem *cDel = makeItem(gateTxt, Qt::AlignLeft | Qt::AlignVCenter, tip);
            QTableWidgetItem *cVer = makeItem(af.summary, Qt::AlignLeft | Qt::AlignVCenter, tip);
            cPos->setForeground(colMuted);
            cDel->setForeground(colMuted);
            cVer->setForeground(af.summary.contains(QStringLiteral("漏检")) ? colErr : colMuted);

            m_gateTable->setItem(r, 0, cF);
            m_gateTable->setItem(r, 1, cPos);
            m_gateTable->setItem(r, 2, cSnr);
            m_gateTable->setItem(r, 3, cDel);
            m_gateTable->setItem(r, 4, cVer);
            continue;
        }

        for (const trackdiag::GateCandidate &c : af.candidates) {
            const int r = m_gateTable->rowCount();
            m_gateTable->insertRow(r);
            const QString posTxt = QStringLiteral("%1/%2°/%3")
                .arg(c.range, 0, 'f', 1).arg(c.az, 0, 'f', 1).arg(c.vr, 0, 'f', 1);
            const QString delTxt = QStringLiteral("%1/%2°/%3")
                .arg(c.dR, 0, 'f', 1).arg(c.dAz, 0, 'f', 1).arg(c.dVr, 0, 'f', 1);
            const qint64 tMs = c.timeMs > 0 ? c.timeMs : af.timeMs;
            const QString tip = QStringLiteral(
                "帧 #%1 · 点ID=%2 · 时间 %3\n"
                "实测: R=%4, Az=%5°, Vr=%6, SNR=%7 dB\n"
                "预测: R=%8, Az=%9°, Vr=%10 | 1x波门: ±%11 / ±%12° / ±%13\n"
                "残差: ΔR=%14, ΔAz=%15°, ΔVr=%16\n"
                "判定: %17")
                .arg(c.frameId > 0 ? c.frameId : af.frameId)
                .arg(c.pointId)
                .arg(formatBeijingTime(tMs))
                .arg(c.range, 0, 'f', 2).arg(c.az, 0, 'f', 2).arg(c.vr, 0, 'f', 2).arg(c.snr, 0, 'f', 1)
                .arg(af.predR, 0, 'f', 2).arg(af.predAz, 0, 'f', 2).arg(af.predVr, 0, 'f', 2)
                .arg(af.gateR, 0, 'f', 1).arg(af.gateAz, 0, 'f', 2).arg(af.gateVr, 0, 'f', 1)
                .arg(c.dR, 0, 'f', 2).arg(c.dAz, 0, 'f', 2).arg(c.dVr, 0, 'f', 2)
                .arg(c.verdict);

            QTableWidgetItem *cF = makeItem(QStringLiteral("#%1").arg(c.frameId > 0 ? c.frameId : af.frameId),
                                            Qt::AlignRight | Qt::AlignVCenter, tip);
            cF->setData(Qt::UserRole, tMs);
            QTableWidgetItem *cPos = makeItem(posTxt, Qt::AlignLeft | Qt::AlignVCenter, tip);
            QTableWidgetItem *cSnr = makeItem(QString::number(double(c.snr), 'f', 1),
                                              Qt::AlignRight | Qt::AlignVCenter, tip);
            QTableWidgetItem *cDel = makeItem(delTxt, Qt::AlignLeft | Qt::AlignVCenter, tip);
            QTableWidgetItem *cVer = makeItem(c.verdict, Qt::AlignLeft | Qt::AlignVCenter, tip);

            if (c.verdict.contains(QStringLiteral("已关联")))
                cVer->setForeground(colOk);
            else if (c.verdict.contains(QStringLiteral("抢占")))
                cVer->setForeground(colErr);
            else if (c.verdict.contains(QStringLiteral("未关联")))
                cVer->setForeground(colInfo);
            else
                cVer->setForeground(colWarn);

            m_gateTable->setItem(r, 0, cF);
            m_gateTable->setItem(r, 1, cPos);
            m_gateTable->setItem(r, 2, cSnr);
            m_gateTable->setItem(r, 3, cDel);
            m_gateTable->setItem(r, 4, cVer);
        }
    }
}

void DiagnosticsPanel::emitPoint3dBoxForGate(const trackdiag::GateAutopsyResult &res)
{
    double rMin = std::numeric_limits<double>::infinity();
    double rMax = -std::numeric_limits<double>::infinity();
    double vMin = std::numeric_limits<double>::infinity();
    double vMax = -std::numeric_limits<double>::infinity();
    double azMin = std::numeric_limits<double>::infinity();
    double azMax = -std::numeric_limits<double>::infinity();
    int fMin = std::numeric_limits<int>::max();
    int fMax = std::numeric_limits<int>::min();
    bool any = false;

    for (const trackdiag::GateAutopsyFrame &af : res.frames) {
        if (std::isfinite(af.predR) && std::isfinite(af.predAz) && std::isfinite(af.predVr)) {
            const double gR = qMax(10.0, double(af.gateR) * 2.0);
            const double gA = qMax(1.5, double(af.gateAz) * 2.0);
            const double gV = qMax(8.0, double(af.gateVr) * 2.0);
            rMin = qMin(rMin, double(af.predR) - gR);
            rMax = qMax(rMax, double(af.predR) + gR);
            azMin = qMin(azMin, double(af.predAz) - gA);
            azMax = qMax(azMax, double(af.predAz) + gA);
            vMin = qMin(vMin, double(af.predVr) - gV);
            vMax = qMax(vMax, double(af.predVr) + gV);
            any = true;
        }
        int zv = af.frameId;
        if (m_detData && af.frameIndex >= 0 && af.frameIndex < m_detData->frames.size())
            zv = int(std::lround(m_detData->frames.at(af.frameIndex).zValue));
        fMin = qMin(fMin, zv);
        fMax = qMax(fMax, zv);
    }
    if (!any)
        return;
    if (fMin > fMax) {
        fMin = 1;
        fMax = 10;
    } else if (fMax - fMin < 4) {
        fMin -= 2;
        fMax += 2;
    }
    emit focusPoint3dRegionRequested(rMin, rMax, vMin, vMax, azMin, azMax, fMin, fMax);
}

void DiagnosticsPanel::rebuildFragmentTable()
{
    m_fragmentCountLabel->setText(QStringLiteral("疑似断批重起配对：%1 对").arg(m_fragmentPairs.size()));

    QSignalBlocker blk(m_fragmentTable);
    m_fragmentTable->setRowCount(m_fragmentPairs.size());
    const QColor colOk(0x22, 0xc5, 0x5e);
    const QColor colWarn = themeColor("warning");

    for (int i = 0; i < m_fragmentPairs.size(); ++i) {
        const trackdiag::TrackFragmentPair &fp = m_fragmentPairs.at(i);
        const double gapSec = double(fp.gapMs) / 1000.0;
        const QString pairTxt = QStringLiteral("%1 → %2").arg(fp.firstId, fp.secondId);
        const QString tip = QStringLiteral("%1 | 间隔 %2s | 外推距离 %3 km | ΔVr %4 | ΔAz %5 | 置信分 %6")
                                .arg(pairTxt)
                                .arg(gapSec, 0, 'f', 1)
                                .arg(fp.extrapDistKm, 0, 'f', 1)
                                .arg(fmtOpt(fp.dVr, 1, QStringLiteral(" m/s")))
                                .arg(fmtOpt(fp.dAz, 2, QStringLiteral("°")))
                                .arg(fp.score, 0, 'f', 0);

        QTableWidgetItem *cPair = makeItem(pairTxt, Qt::AlignLeft | Qt::AlignVCenter, tip);
        QTableWidgetItem *cGap = makeItem(QString::number(gapSec, 'f', 1), Qt::AlignRight | Qt::AlignVCenter, tip);
        QTableWidgetItem *cDist = makeItem(QString::number(fp.extrapDistKm, 'f', 1), Qt::AlignRight | Qt::AlignVCenter, tip);
        QTableWidgetItem *cVr = makeItem(fmtOpt(fp.dVr, 1), Qt::AlignRight | Qt::AlignVCenter, tip);
        QTableWidgetItem *cScore = makeItem(QString::number(int(std::lround(fp.score))),
                                            Qt::AlignRight | Qt::AlignVCenter, tip);
        if (fp.score >= 75.0)
            cScore->setForeground(colOk);
        else if (fp.score >= 55.0)
            cScore->setForeground(colWarn);

        m_fragmentTable->setItem(i, 0, cPair);
        m_fragmentTable->setItem(i, 1, cGap);
        m_fragmentTable->setItem(i, 2, cDist);
        m_fragmentTable->setItem(i, 3, cVr);
        m_fragmentTable->setItem(i, 4, cScore);
    }

    m_fragmentDetailLabel->setText(m_fragmentPairs.isEmpty()
                                       ? QStringLiteral("当前未检出疑似断批重起航迹对")
                                       : QStringLiteral("点击上方配对行，在地图双选隔离并绘制桥接虚线"));
    m_btnFragmentSeek->setEnabled(false);
}

void DiagnosticsPanel::rebuildOrphanTable()
{
    if (!m_detData || m_detData->frames.isEmpty()) {
        m_orphanCountLabel->setText(QStringLiteral("未关联强连续点迹链：未加载 DetPoint"));
    } else {
        m_orphanCountLabel->setText(QStringLiteral("未关联强连续点迹链：%1 条（点击定位地图与点云盒）")
                                        .arg(m_orphanClusters.size()));
    }

    QSignalBlocker blk(m_orphanTable);
    m_orphanTable->setRowCount(m_orphanClusters.size());

    for (int i = 0; i < m_orphanClusters.size(); ++i) {
        const trackdiag::OrphanDetCluster &cl = m_orphanClusters.at(i);
        const QString frameSpan = QStringLiteral("%1-%2").arg(cl.frameStart).arg(cl.frameEnd);
        const QString polarTxt = QStringLiteral("%1/%2°/%3")
            .arg(cl.meanRange, 0, 'f', 1).arg(cl.meanAz, 0, 'f', 1).arg(cl.meanVr, 0, 'f', 1);
        const QString tip = QStringLiteral(
            "簇 #%1 | %2 点 | 帧 %3 (%4 ~ %5)\n"
            "均R=%6 | 均Az=%7° | 均Vr=%8 | 均SNR=%9 dB | 均连续性=%10")
            .arg(cl.clusterId)
            .arg(cl.pointCount)
            .arg(frameSpan, fmtClockShort(cl.t0), fmtClockShort(cl.t1))
            .arg(cl.meanRange, 0, 'f', 1)
            .arg(cl.meanAz, 0, 'f', 2)
            .arg(cl.meanVr, 0, 'f', 1)
            .arg(cl.meanSnr, 0, 'f', 1)
            .arg(cl.meanContinuity, 0, 'f', 2);

        m_orphanTable->setItem(i, 0, makeItem(QStringLiteral("#%1").arg(cl.clusterId), Qt::AlignRight | Qt::AlignVCenter, tip));
        m_orphanTable->setItem(i, 1, makeItem(QString::number(cl.pointCount), Qt::AlignRight | Qt::AlignVCenter, tip));
        m_orphanTable->setItem(i, 2, makeItem(frameSpan, Qt::AlignCenter, tip));
        m_orphanTable->setItem(i, 3, makeItem(polarTxt, Qt::AlignLeft | Qt::AlignVCenter, tip));
        m_orphanTable->setItem(i, 4, makeItem(QString::number(double(cl.meanSnr), 'f', 1), Qt::AlignRight | Qt::AlignVCenter, tip));
    }
}

void DiagnosticsPanel::emitPoint3dBoxForOrphan(const trackdiag::OrphanDetCluster &cl)
{
    double rMin = std::numeric_limits<double>::infinity();
    double rMax = -std::numeric_limits<double>::infinity();
    double vMin = std::numeric_limits<double>::infinity();
    double vMax = -std::numeric_limits<double>::infinity();
    double azMin = std::numeric_limits<double>::infinity();
    double azMax = -std::numeric_limits<double>::infinity();
    int fMin = std::numeric_limits<int>::max();
    int fMax = std::numeric_limits<int>::min();
    bool any = false;

    if (m_detData && !cl.framePointIndices.isEmpty()) {
        for (const QPair<int, int> &fp : cl.framePointIndices) {
            if (fp.first < 0 || fp.first >= m_detData->frames.size())
                continue;
            const DetPointFrame &df = m_detData->frames.at(fp.first);
            if (fp.second < 0 || fp.second >= df.points.size())
                continue;
            const DetPointItem &dp = df.points.at(fp.second);
            rMin = qMin(rMin, double(dp.range));
            rMax = qMax(rMax, double(dp.range));
            vMin = qMin(vMin, double(dp.vr));
            vMax = qMax(vMax, double(dp.vr));
            azMin = qMin(azMin, double(dp.az));
            azMax = qMax(azMax, double(dp.az));
            const int zv = int(std::lround(df.zValue));
            fMin = qMin(fMin, zv);
            fMax = qMax(fMax, zv);
            any = true;
        }
    }

    if (!any) {
        rMin = rMax = double(cl.meanRange);
        vMin = vMax = double(cl.meanVr);
        azMin = azMax = double(cl.meanAz);
        fMin = cl.frameStart;
        fMax = cl.frameEnd;
    }

    const double rPad = qMax(20.0, (rMax - rMin) * 0.25);
    const double vPad = qMax(10.0, (vMax - vMin) * 0.25);
    const double azPad = qMax(2.5, (azMax - azMin) * 0.25);
    if (fMax - fMin < 4) {
        fMin -= 2;
        fMax += 2;
    }
    emit focusPoint3dRegionRequested(rMin - rPad, rMax + rPad,
                                     vMin - vPad, vMax + vPad,
                                     azMin - azPad, azMax + azPad,
                                     fMin, fMax);
}

void DiagnosticsPanel::rebuildFunnelTrackCombo()
{
    m_populatingFunnelCombo = true;
    const int prevTrackIdx = m_funnelTrackCombo->currentData().isValid()
        ? m_funnelTrackCombo->currentData().toInt() : -1;
    m_funnelTrackCombo->clear();

    if (m_store && m_store->size() > 0) {
        const QVector<Track> &tracks = m_store->tracks();
        // 优先列出 ADS-B 参考航迹，其次雷达航迹
        QVector<int> order;
        order.reserve(tracks.size());
        for (int i = 0; i < tracks.size(); ++i) {
            if (tracks.at(i).source == TrackSource::Adsb)
                order.append(i);
        }
        for (int i = 0; i < tracks.size(); ++i) {
            if (tracks.at(i).source != TrackSource::Adsb)
                order.append(i);
        }
        int restoreComboIdx = -1;
        for (int idx : qAsConst(order)) {
            const Track &tr = tracks.at(idx);
            const QString label = QStringLiteral("%1 [%2 · %3点]")
                                      .arg(shortTrackTitle(tr), trackSourceName(tr.source))
                                      .arg(tr.points.size());
            m_funnelTrackCombo->addItem(label, idx);
            if (idx == prevTrackIdx)
                restoreComboIdx = m_funnelTrackCombo->count() - 1;
        }
        if (restoreComboIdx >= 0)
            m_funnelTrackCombo->setCurrentIndex(restoreComboIdx);
        else if (m_funnelTrackCombo->count() > 0)
            m_funnelTrackCombo->setCurrentIndex(0);
    }
    m_populatingFunnelCombo = false;
    runFunnelForSelectedTrack();
}

void DiagnosticsPanel::runFunnelForSelectedTrack()
{
    m_funnelStats.clear();
    m_funnelTable->setRowCount(0);

    if (!m_store || m_funnelTrackCombo->currentIndex() < 0) {
        m_funnelSummaryLabel->setText(QStringLiteral("无可用航迹进行四级漏斗分析"));
        return;
    }
    const int trackIndex = m_funnelTrackCombo->currentData().toInt();
    if (trackIndex < 0 || trackIndex >= m_store->size()) {
        m_funnelSummaryLabel->setText(QStringLiteral("请选择有效航迹"));
        return;
    }

    const Track &refTrack = m_store->tracks().at(trackIndex);
    m_funnelStats = trackdiag::traceTrackFunnel(refTrack, *m_store, effectiveBundle(), m_detData, 40.0);

    int h1 = 0, h2 = 0, h3 = 0, h4 = 0;
    int drop12 = 0, drop23 = 0, drop34 = 0;
    for (const trackdiag::FunnelFrameStat &st : qAsConst(m_funnelStats)) {
        if (st.l1DetCount > 0) ++h1;
        if (st.l2TempCount > 0) ++h2;
        if (st.l3AsscCount > 0) ++h3;
        if (st.l4SmoothCount > 0) ++h4;
        if (st.l1DetCount > 0 && st.l2TempCount == 0 && st.l3AsscCount == 0) ++drop12;
        if (st.l2TempCount > 0 && st.l3AsscCount == 0) ++drop23;
        if (st.l3AsscCount > 0 && st.l4SmoothCount == 0) ++drop34;
    }

    QString bottleneck = QStringLiteral("全链路命中均衡");
    if (drop12 >= drop23 && drop12 >= drop34 && drop12 > 0)
        bottleneck = QStringLiteral("主要流失：L1检测→起批/关联（%1 帧）").arg(drop12);
    else if (drop23 >= drop34 && drop23 > 0)
        bottleneck = QStringLiteral("主要流失：L2临时池→L3关联（%1 帧）").arg(drop23);
    else if (drop34 > 0)
        bottleneck = QStringLiteral("主要流失：L3关联→L4平滑输出（%1 帧）").arg(drop34);
    else if (!m_funnelStats.isEmpty() && h1 == 0 && m_detData && !m_detData->frames.isEmpty())
        bottleneck = QStringLiteral("L1 原始检测 0 命中（可能前端未检出或无重叠）");

    m_funnelSummaryLabel->setText(
        QStringLiteral("共 %1 帧 | L1:%2 → L2:%3 → L3:%4 → L4:%5\n%6")
            .arg(m_funnelStats.size()).arg(h1).arg(h2).arg(h3).arg(h4).arg(bottleneck));

    m_funnelTable->setRowCount(m_funnelStats.size());
    const QColor colOk(0x22, 0xc5, 0x5e);
    const QColor colWarn = themeColor("warning");
    const QColor colErr = themeColor("error");
    const QColor colMuted = themeColor("text-tertiary");

    for (int i = 0; i < m_funnelStats.size(); ++i) {
        const trackdiag::FunnelFrameStat &st = m_funnelStats.at(i);
        const QString snrDist = (std::isfinite(st.bestDetSnr) || std::isfinite(st.bestDetDistKm))
            ? QStringLiteral("%1 / %2").arg(fmtOpt(st.bestDetSnr, 1, QStringLiteral("dB")),
                                            fmtOpt(st.bestDetDistKm, 1, QStringLiteral("km")))
            : QStringLiteral("—");
        const QString tip = QStringLiteral("帧 #%1 | 时间 %2\nL1检测:%3 | L2临时:%4 | L3关联:%5 | L4平滑:%6\n最佳检测:%7")
                                .arg(st.frameId)
                                .arg(formatBeijingTime(st.timeMs))
                                .arg(st.l1DetCount)
                                .arg(st.l2TempCount)
                                .arg(st.l3AsscCount)
                                .arg(st.l4SmoothCount)
                                .arg(snrDist);

        QTableWidgetItem *cF = makeItem(QStringLiteral("#%1").arg(st.frameId), Qt::AlignRight | Qt::AlignVCenter, tip);
        QTableWidgetItem *c1 = makeItem(QString::number(st.l1DetCount), Qt::AlignRight | Qt::AlignVCenter, tip);
        QTableWidgetItem *c2 = makeItem(QString::number(st.l2TempCount), Qt::AlignRight | Qt::AlignVCenter, tip);
        QTableWidgetItem *c3 = makeItem(QString::number(st.l3AsscCount), Qt::AlignRight | Qt::AlignVCenter, tip);
        QTableWidgetItem *c4 = makeItem(QString::number(st.l4SmoothCount), Qt::AlignRight | Qt::AlignVCenter, tip);
        QTableWidgetItem *cS = makeItem(snrDist, Qt::AlignLeft | Qt::AlignVCenter, tip);

        c1->setForeground(st.l1DetCount > 0 ? colOk : colMuted);
        c2->setForeground(st.l2TempCount > 0 ? colOk : (st.l1DetCount > 0 ? colWarn : colMuted));
        c3->setForeground(st.l3AsscCount > 0 ? colOk : (st.l2TempCount > 0 ? colErr : colMuted));
        c4->setForeground(st.l4SmoothCount > 0 ? colOk : (st.l3AsscCount > 0 ? colErr : colMuted));

        m_funnelTable->setItem(i, 0, cF);
        m_funnelTable->setItem(i, 1, c1);
        m_funnelTable->setItem(i, 2, c2);
        m_funnelTable->setItem(i, 3, c3);
        m_funnelTable->setItem(i, 4, c4);
        m_funnelTable->setItem(i, 5, cS);
    }
}

// ---------------------------------------------------------------------------
//  自检与外部控制方法
// ---------------------------------------------------------------------------

int DiagnosticsPanel::falseTrackCount() const
{
    return m_falseItems.size();
}

int DiagnosticsPanel::filteredFalseCount() const
{
    int cnt = 0;
    for (const trackdiag::FalseTrackItem &it : m_falseItems) {
        if (it.filteredOut)
            ++cnt;
    }
    return cnt;
}

int DiagnosticsPanel::brokenTrackCount() const
{
    return m_brokenItems.size();
}

int DiagnosticsPanel::fragmentPairCount() const
{
    return m_fragmentPairs.size();
}

int DiagnosticsPanel::orphanClusterCount() const
{
    return m_orphanClusters.size();
}

int DiagnosticsPanel::funnelFrameCount() const
{
    return m_funnelStats.size();
}

void DiagnosticsPanel::selectFalseRow(int row)
{
    if (row < 0 || row >= m_falseItems.size())
        return;
    {
        QSignalBlocker blk(m_falseTable);
        m_falseTable->selectRow(row);
    }
    updateFalseAutopsyCard(row);
}

void DiagnosticsPanel::selectBrokenRow(int row)
{
    if (!m_store || row < 0 || row >= m_brokenItems.size())
        return;
    m_selectedBrokenRow = row;
    {
        QSignalBlocker blk(m_brokenTable);
        m_brokenTable->selectRow(row);
    }

    const trackdiag::BrokenTrackItem &item = m_brokenItems.at(row);
    m_lastAutopsy = trackdiag::runGateAutopsy(item.trackIndex, *m_store, effectiveBundle(), m_detData);
    populateGateTable(m_lastAutopsy);

    QString level;
    if (m_lastAutopsy.primaryCause.contains(QStringLiteral("漏检"))
            || m_lastAutopsy.primaryCause.contains(QStringLiteral("抢占"))) {
        level = QStringLiteral("error");
    } else if (m_lastAutopsy.primaryCause.contains(QStringLiteral("超限"))
               || m_lastAutopsy.primaryCause.contains(QStringLiteral("未关联"))
               || m_lastAutopsy.primaryCause.contains(QStringLiteral("未加载"))) {
        level = QStringLiteral("warn");
    }
    m_autopsyCauseLabel->setProperty("level", level);
    m_autopsyCauseLabel->setText(QStringLiteral("主要死因：%1").arg(m_lastAutopsy.primaryCause));
    m_autopsyCauseLabel->style()->unpolish(m_autopsyCauseLabel);
    m_autopsyCauseLabel->style()->polish(m_autopsyCauseLabel);

    m_btnBrokenFocus->setEnabled(true);
    m_btnBrokenBox->setEnabled(true);

    emit gateAutopsyOverlayChanged(m_lastAutopsy, !m_lastAutopsy.frames.isEmpty());
}

void DiagnosticsPanel::selectFragmentRow(int row)
{
    if (row < 0 || row >= m_fragmentPairs.size())
        return;
    m_selectedFragmentRow = row;
    {
        QSignalBlocker blk(m_fragmentTable);
        m_fragmentTable->selectRow(row);
    }

    const trackdiag::TrackFragmentPair &fp = m_fragmentPairs.at(row);
    m_fragmentDetailLabel->setText(
        QStringLiteral("%1 → %2 | 间隔 %3 s | 外推距离 %4 km | ΔVr %5 | ΔAz %6 | 评分 %7")
            .arg(fp.firstId, fp.secondId)
            .arg(double(fp.gapMs) / 1000.0, 0, 'f', 1)
            .arg(fp.extrapDistKm, 0, 'f', 1)
            .arg(fmtOpt(fp.dVr, 1, QStringLiteral(" m/s")))
            .arg(fmtOpt(fp.dAz, 2, QStringLiteral("°")))
            .arg(fp.score, 0, 'f', 0));
    m_btnFragmentSeek->setEnabled(true);

    QSet<int> pairSet;
    if (fp.firstIndex >= 0)
        pairSet.insert(fp.firstIndex);
    if (fp.secondIndex >= 0)
        pairSet.insert(fp.secondIndex);
    emit isolateTracksRequested(pairSet, true);
    emit fragmentOverlayChanged(fp.firstIndex, fp.secondIndex, true);
}

void DiagnosticsPanel::selectOrphanRow(int row)
{
    if (row < 0 || row >= m_orphanClusters.size())
        return;
    m_selectedOrphanRow = row;
    {
        QSignalBlocker blk(m_orphanTable);
        m_orphanTable->selectRow(row);
    }
    const trackdiag::OrphanDetCluster &cl = m_orphanClusters.at(row);
    emit orphanClusterSelected(cl);
    emitPoint3dBoxForOrphan(cl);
}

void DiagnosticsPanel::selectFunnelTrack(int trackIndex)
{
    if (!m_funnelTrackCombo)
        return;
    for (int i = 0; i < m_funnelTrackCombo->count(); ++i) {
        if (m_funnelTrackCombo->itemData(i).toInt() == trackIndex) {
            m_funnelTrackCombo->setCurrentIndex(i);
            return;
        }
    }
}

void DiagnosticsPanel::setSandboxMinQuality(double q)
{
    if (!m_spinMinQuality)
        return;
    const double clamped = qBound(m_spinMinQuality->minimum(), q, m_spinMinQuality->maximum());
    if (std::fabs(m_spinMinQuality->value() - clamped) < 1e-6) {
        syncConfigFromSandbox();
        recomputeFalseTracks();
        return;
    }
    m_spinMinQuality->setValue(clamped);
}

void DiagnosticsPanel::setCurrentTab(int index)
{
    if (m_tabs && index >= 0 && index < m_tabs->count())
        m_tabs->setCurrentIndex(index);
}

int DiagnosticsPanel::currentTab() const
{
    return m_tabs ? m_tabs->currentIndex() : 0;
}

void DiagnosticsPanel::setDisappearSubView(int index)
{
    if (!m_disappearStack || index < 0 || index >= m_disappearStack->count())
        return;
    m_disappearStack->setCurrentIndex(index);
    for (int i = 0; i < 3; ++i) {
        if (m_subViewBtns[i]) {
            QSignalBlocker blk(m_subViewBtns[i]);
            m_subViewBtns[i]->setChecked(i == index);
        }
    }
}

int DiagnosticsPanel::disappearSubView() const
{
    return m_disappearStack ? m_disappearStack->currentIndex() : 0;
}
