#include "mainwindow.h"

#include <QSplitter>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QStackedWidget>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QApplication>
#include <QFileDialog>
#include <QMessageBox>
#include <QDir>
#include <QFileInfo>
#include <QKeySequence>
#include <QEvent>
#include <QDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QSettings>
#include <QTimer>
#include <QtMath>
#include <cmath>

#include "apppaths.h"
#include "appstatusbar.h"
#include "globewidget.h"
#include "managepanel.h"
#include "maptools.h"
#include "replaycontroller.h"
#include "theme.h"
#include "trackdb.h"
#include "trackimporter.h"
#include "tracklayer.h"
#include "trackpointdialog.h"
#include "uiwidgets.h"

namespace {

const int kSidebarDefault = 280, kSidebarMin = 200, kSidebarMax = 900;

struct PanelDef { LucideIcon icon; const char *tooltip; const char *title; };
const PanelDef kPanels[] = {
    { LucideIcon::List, "轨迹面板 (Ctrl+Shift+T)", "轨迹面板" },
    { LucideIcon::ChartColumn, "航迹管理系统 (Ctrl+Shift+M)", "航迹管理系统" },
    { LucideIcon::Layers, "图层控制 (Ctrl+Shift+L)", "图层控制" },
    { LucideIcon::Flag, "旗标面板 (Ctrl+Shift+F)", "旗标面板" },
    { LucideIcon::Funnel, "时间筛选 (Ctrl+Shift+E)", "筛选" },
    { LucideIcon::Settings, "设置", "设置" },
};

// 活动栏按钮：48×48，24px 图标；悬停 / 选中为 --activitybar-active，选中时左侧 2px 竖条
class ActivityButton : public QAbstractButton
{
public:
    ActivityButton(LucideIcon icon, QWidget *parent) : QAbstractButton(parent), m_icon(icon)
    {
        setFixedSize(48, 48);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setFocusPolicy(Qt::NoFocus);
    }
    bool active = false;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        if (active)
            p.fillRect(QRect(0, 0, 2, height()), themeColor("activitybar-active-border"));
        const QColor c = themeColor(active || underMouse() ? "activitybar-active" : "activitybar-fg");
        drawLucide(p, m_icon, QRectF(13, 12, 24, 24), c);
    }
    void enterEvent(QEvent *) override { update(); }
    void leaveEvent(QEvent *) override { update(); }

private:
    LucideIcon m_icon;
};

QString menuQss()
{
    return QStringLiteral(
        "QMenuBar { background: var(--titlebar-bg); color: var(--titlebar-fg); font-family: %1; font-size: 13px;"
        " border-bottom: 1px solid var(--titlebar-border); }"
        "QMenuBar::item { padding: 5px 8px; background: transparent; }"
        "QMenuBar::item:selected, QMenuBar::item:pressed { background: var(--menu-hover); }"
        "QMenu { min-width: 220px; background: var(--menu-bg); color: var(--menu-fg); border: 1px solid var(--border-primary);"
        " border-radius: 3px; padding: 4px 0px; font-family: %1; font-size: 13px; }"
        "QMenu::item { padding: 4px 20px; min-height: 16px; }"
        "QMenu::item:selected { background: var(--menu-hover); }"
        "QMenu::item:disabled { color: var(--menu-shortcut); }"
        "QMenu::separator { height: 1px; margin: 4px 12px; background: var(--menu-separator); }")
        .arg(ui::uiFamilies());
}

} // namespace

// ---------------------------------------------------------------
//  活动栏
// ---------------------------------------------------------------
ActivityBar::ActivityBar(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("activityBar"));
    setFixedWidth(48);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    QVBoxLayout *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    for (int i = 0; i < int(PanelId::Count); ++i) {
        ActivityButton *b = new ActivityButton(kPanels[i].icon, this);
        b->setToolTip(QString::fromUtf8(kPanels[i].tooltip));
        b->setAccessibleName(QString::fromUtf8(kPanels[i].tooltip));
        if (i == int(PanelId::Settings))
            lay->addStretch(1);           // 设置固定在底部
        lay->addWidget(b);
        m_buttons << b;
        connect(b, &QAbstractButton::clicked, this, [this, i]() { emit iconClicked(i); });
    }
    connect(Theme::instance(), &Theme::changed, this, [this]() {
        update();
        for (QAbstractButton *b : qAsConst(m_buttons))
            b->update();
    });
}

void ActivityBar::highlight(int index)
{
    m_active = index;
    for (int i = 0; i < m_buttons.size(); ++i) {
        static_cast<ActivityButton *>(m_buttons[i])->active = i == index;
        m_buttons[i]->update();
    }
}

void ActivityBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), themeColor("activitybar-bg"));
    p.fillRect(QRect(width() - 1, 0, 1, height()), themeColor("border-secondary"));
}

// ---------------------------------------------------------------
//  侧栏
// ---------------------------------------------------------------
SidePanel::SidePanel(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("sidePanel"));
    setAttribute(Qt::WA_StyledBackground);
    setMinimumWidth(kSidebarMin);
    setMaximumWidth(kSidebarMax);
    QVBoxLayout *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    QWidget *header = new QWidget(this);
    header->setObjectName(QStringLiteral("sidebarHeader"));
    header->setAttribute(Qt::WA_StyledBackground);
    header->setFixedHeight(32);
    QHBoxLayout *hh = new QHBoxLayout(header);
    hh->setContentsMargins(12, 0, 12, 0);
    m_title = new QLabel(header);
    m_title->setObjectName(QStringLiteral("sidebarTitle"));
    hh->addWidget(m_title);
    hh->addStretch(1);
    ui::IconButton *close = new ui::IconButton(LucideIcon::X, 14, "text-tertiary", "text-primary", header);
    close->setObjectName(QStringLiteral("sidebarClose"));
    close->setFixedSize(20, 20);
    close->setToolTip(QStringLiteral("关闭侧边栏"));
    hh->addWidget(close);
    lay->addWidget(header);
    m_stack = new QStackedWidget(this);
    m_stack->setObjectName(QStringLiteral("sidebarBody"));
    m_stack->setContentsMargins(8, 8, 8, 8);
    lay->addWidget(m_stack, 1);
    connect(close, &QToolButton::clicked, this, &SidePanel::closeRequested);
    setThemedStyle(this, QStringLiteral(
        "#sidePanel { background: var(--sidebar-bg); border-right: 1px solid var(--sidebar-border); }"
        "#sidebarHeader { background: var(--sidebar-bg); border-bottom: 1px solid var(--sidebar-border); }"
        "#sidebarTitle { color: var(--sidebar-header); font-family: %1; font-size: 11px; font-weight: 600; background: transparent; }"
        "#sidebarClose { border: none; border-radius: 3px; background: transparent; }"
        "#sidebarClose:hover { background: var(--button-hover); }"
        "#sidebarBody { background: var(--sidebar-bg); }"
        "#sidebarBody QToolTip { font-size: 11px; }").arg(ui::uiFamilies()));
}

void SidePanel::addPanel(QWidget *w)
{
    m_stack->addWidget(w);
}

void SidePanel::showPanel(int index, const QString &title)
{
    m_stack->setCurrentIndex(index);
    m_title->setText(title);
}

int SidePanel::currentPanel() const
{
    return m_stack->currentIndex();
}

QString SidePanel::title() const
{
    return m_title->text();
}

// ---------------------------------------------------------------
//  主窗口
// ---------------------------------------------------------------
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("RadarView"));

    QWidget *central = new QWidget(this);
    central->setObjectName(QStringLiteral("central"));
    QVBoxLayout *v = new QVBoxLayout(central);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    QWidget *row = new QWidget(central);
    QHBoxLayout *h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);
    v->addWidget(row, 1);

    m_activityBar = new ActivityBar(row);
    m_splitter = new QSplitter(Qt::Horizontal, row);
    m_sidePanel = new SidePanel();
    m_editor = buildEditorArea();
    m_splitter->addWidget(m_sidePanel);
    m_splitter->addWidget(m_editor);
    m_splitter->setCollapsible(0, false);
    m_splitter->setCollapsible(1, false);
    m_splitter->setHandleWidth(4);
    setThemedStyle(m_splitter, QStringLiteral("QSplitter::handle { background: transparent; }"
                                              "QSplitter::handle:hover { background: var(--accent-primary); }"));
    h->addWidget(m_activityBar);
    h->addWidget(m_splitter, 1);
    setCentralWidget(central);
    setThemedStyle(central, QStringLiteral("#central { background: var(--editor-bg); }"));

    m_globe->setTrackStore(&m_store);
    m_flags = new FlagStore(this);
    m_ruler = new RulerState(this);
    m_globe->setMapTools(m_flags, m_ruler);
    m_manage = new ManageState(&m_store, this);

    // 侧栏面板（顺序同 PanelId）；轨迹 / 图层 / 设置面板暂为空
    m_managePanel = new ManagePanel(m_manage);
    m_managePanel->rowColor = [this](const ManageRow &r) { return m_store.fileColor(r.source, r.fileName); };
    m_filterPanel = new FilterPanel(&m_filter);
    m_flagPanel = new FlagPanel(m_flags, m_ruler);
    for (int i = 0; i < int(PanelId::Count); ++i) {
        QWidget *w = i == int(PanelId::Manage) ? static_cast<QWidget *>(m_managePanel)
                   : i == int(PanelId::Flags) ? static_cast<QWidget *>(m_flagPanel)
                   : i == int(PanelId::TimeFilter) ? static_cast<QWidget *>(m_filterPanel) : new QWidget;
        m_sidePanel->addPanel(w);
    }
    m_sidePanel->hide();
    connect(m_sidePanel, &SidePanel::closeRequested, this, &MainWindow::closeSidebar);
    connect(m_activityBar, &ActivityBar::iconClicked, this, &MainWindow::onIconClicked);
    connect(m_splitter, &QSplitter::splitterMoved, this, [this]() {
        if (m_panel >= 0)
            m_panelWidth[m_panel] = m_splitter->sizes().value(0);
    });

    // 状态栏：常驻的自绘控件（不用 QStatusBar，它的 showMessage() 会把左侧控件整体隐藏）
    m_replay = new ReplayController(this);
    m_statusBar = new AppStatusBar(m_replay, central);
    v->addWidget(m_statusBar);
    Theme::instance()->setId(m_statusBar->theme().id);
    connect(m_statusBar, &AppStatusBar::themeChanged, Theme::instance(), &Theme::setId);
    connect(m_globe, &GlobeWidget::viewStatusChanged, m_statusBar, &AppStatusBar::setViewStatus);
    connect(m_statusBar, &AppStatusBar::sourceToggled, this, &MainWindow::toggleSource);
    connect(m_replay, &ReplayController::timeChanged, this, &MainWindow::syncReplayToLayer);
    connect(m_replay, &ReplayController::stateChanged, this, &MainWindow::syncReplayToLayer);

    buildMenuBar();
    setThemedStyle(menuBar(), menuQss());

    // 地图交互：单击航迹单独显示，单击空白处返回全部；右键菜单；双击放旗标（GlobeWidget 内处理）
    connect(m_globe, &GlobeWidget::trackClicked, this, [this](int index) {
        if (index >= 0)
            isolateTrack(index);
        else if (isolatedTrack() >= 0)
            isolateTrack(-1);
    });
    connect(m_globe, &GlobeWidget::trackContextMenuRequested, this, &MainWindow::onTrackContextMenu);
    connect(m_globe, &GlobeWidget::flagContextMenuRequested, this, &MainWindow::onFlagContextMenu);
    m_backAllBtn = new QPushButton(tr("← 返回全部"), m_globe);
    m_backAllBtn->setToolTip(tr("返回查看全部航迹（Esc）"));
    m_backAllBtn->setCursor(Qt::PointingHandCursor);
    m_backAllBtn->setStyleSheet(
        "QPushButton { background-color: #0078d4; color: #ffffff; border: none; border-radius: 2px;"
        " padding: 4px 12px; font-weight: bold; }"
        "QPushButton:hover { background-color: #1a8ae0; }");
    m_backAllBtn->hide();
    connect(m_backAllBtn, &QPushButton::clicked, this, [this]() { isolateTrack(-1); });
    m_globe->installEventFilter(this);

    // 管理面板 / 筛选 → 地图显示集合
    connect(m_manage, &ManageState::aboutToChangeStore, m_globe, &GlobeWidget::resetInteraction);
    connect(m_manage, &ManageState::storeChanged, this, [this]() {
        refreshSources();
        m_filterPanel->setDataRange(m_store.minTime(), m_store.maxTime());
        applyDisplay();
    });
    connect(m_manage, &ManageState::displayChanged, this, &MainWindow::applyDisplay);
    connect(m_managePanel, &ManagePanel::viewPointsRequested, this, [this](const QString &key) {
        showTrackPoints(m_store.indexOf(key));
    });
    connect(m_filterPanel, &FilterPanel::changed, this, &MainWindow::applyDisplay);
    m_undoToast = new ui::UndoToast(central);
    connect(m_undoToast, &ui::UndoToast::undoClicked, m_manage, &ManageState::undoDelete);
    connect(m_manage, &ManageState::undoChanged, this, &MainWindow::updateUndoToast);

    // 快捷键（App.vue keydown）
    auto shortcut = [this](const QKeySequence &seq, std::function<void()> fn) {
        QAction *a = new QAction(this);
        a->setShortcut(seq);
        a->setShortcutContext(Qt::WindowShortcut);
        connect(a, &QAction::triggered, this, fn);
        addAction(a);
    };
    shortcut(Qt::Key_Space, [this]() { m_replay->togglePlay(); });
    shortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_R), [this]() { m_ruler->toggle(); });

    loadPersisted();
    resize(1280, 800);
}

MainWindow::~MainWindow()
{
    // 等后台入库结束，避免进程退出时写到一半
    if (m_saveJob)
        m_saveJob->wait();
    qDeleteAll(m_saveQueue);
    m_saveQueue.clear();
    trackdb::close();
}

// 启动：打开数据库并加载已保存的航迹（RadarView load_persisted_tracks），恢复各项设置
void MainWindow::loadPersisted()
{
    QSettings &s = app::settings();
    // 文件颜色先恢复，加载后同一文件颜色不变，新文件继续轮换
    QHash<QString, QColor> colors;
    const QJsonObject co = QJsonDocument::fromJson(s.value(QStringLiteral("display.file_line_color")).toString().toUtf8()).object();
    for (auto it = co.begin(); it != co.end(); ++it)
        colors.insert(it.key(), QColor(it.value().toString()));
    m_store.setFileColors(colors);

    QString err;
    if (trackdb::open(&err)) {
        const QVector<Track> saved = trackdb::loadAll(&err);
        if (!saved.isEmpty()) {
            m_store.addTracks(saved);
            saveFileColors();
        }
    } else {
        m_statusBar->setError(QStringLiteral("数据库打开失败: %1").arg(err));
    }
    m_manage->loadSettings();
    m_filter.load();
    m_flags->load();
    m_globe->setShowLabels(s.value(QStringLiteral("display.show_labels"), false).toBool());
    if (m_labelAction)
        m_labelAction->setChecked(m_globe->showLabels());
    // 筛选面板控件按已恢复的筛选状态重建
    FilterPanel *fresh = new FilterPanel(&m_filter);
    connect(fresh, &FilterPanel::changed, this, &MainWindow::applyDisplay);
    QStackedWidget *stack = m_sidePanel->findChild<QStackedWidget *>(QStringLiteral("sidebarBody"));
    stack->insertWidget(int(PanelId::TimeFilter), fresh);
    stack->removeWidget(m_filterPanel);
    m_filterPanel->deleteLater();
    m_filterPanel = fresh;
    m_filterPanel->setDataRange(m_store.minTime(), m_store.maxTime());
    m_managePanel->refresh();

    applyAdsbVisibility();
    refreshSources();
    applyDisplay();
    if (m_store.size() > 0) {
        double sx = 0, sy = 0, sz = 0;
        for (const Track &t : m_store.tracks()) {
            const TrackPoint &p = t.points.first();
            const double la = qDegreesToRadians(p.lat), lo = qDegreesToRadians(p.lon);
            sx += std::cos(la) * std::cos(lo);
            sy += std::cos(la) * std::sin(lo);
            sz += std::sin(la);
        }
        m_globe->lookAt(qRadiansToDegrees(std::atan2(sy, sx)),
                        qRadiansToDegrees(std::atan2(sz, std::sqrt(sx * sx + sy * sy))), 6000.0);
    }
}

void MainWindow::saveFileColors()
{
    QJsonObject o;
    const QHash<QString, QColor> colors = m_store.fileColors();
    for (auto it = colors.constBegin(); it != colors.constEnd(); ++it)
        o.insert(it.key(), it.value().name());
    app::settings().setValue(QStringLiteral("display.file_line_color"),
                             QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
}

bool MainWindow::openTiles(const QString &path)
{
    QString err;
    if (!m_globe->openTiles(path, &err)) {
        QMessageBox::warning(this, tr("打开瓦片失败"), err);
        return false;
    }
    return true;
}

// 启动时自动加载：优先 exe 旁 tiles/ 目录，其次 exe 同目录，取层级最高的那个
void MainWindow::loadDefaultTiles()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    QFileInfoList files;
    for (const QString &dir : { appDir + QStringLiteral("/tiles"), appDir })
        files += QDir(dir).entryInfoList(QStringList() << QStringLiteral("*.mbtiles"), QDir::Files);

    QString best;
    int bestZoom = -1;
    for (const QFileInfo &fi : qAsConst(files)) {
        TileSource probe;
        if (probe.open(fi.absoluteFilePath()) && probe.maxZoom() > bestZoom) {
            bestZoom = probe.maxZoom();
            best = fi.absoluteFilePath();
        }
    }
    if (!best.isEmpty())
        openTiles(best);
}

void MainWindow::openTileFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("打开瓦片"), QCoreApplication::applicationDirPath(),
        tr("MBTiles 瓦片 (*.mbtiles);;所有文件 (*)"));
    if (!path.isEmpty())
        openTiles(path);
}

void MainWindow::importFile(TrackSource kind, const QString &path, const QString &displayName)
{
    if (m_importer)
        return;
    QString name = displayName;
    if (name.isEmpty() && kind != TrackSource::Adsb) {
        QSet<QString> used;   // 数据库里已有的同类批次名也要避开
        for (const BatchInfo &b : trackdb::batches())
            if (b.source == kind)
                used.insert(b.fileName);
        name = m_store.uniqueFileName(kind, used);
    }
    m_importer = new TrackImporter(kind, path, name, this);
    connect(m_importer, &TrackImporter::progressChanged, this,
            [this](int percent) { m_statusBar->setLoading(true, percent); });
    connect(m_importer, &TrackImporter::importDone, this, &MainWindow::onImportDone);
    for (QAction *a : qAsConst(m_importActions))
        a->setEnabled(false);
    m_statusBar->setError(QString());       // 与 RadarView 一致：开始导入时清除上次的错误
    m_statusBar->setLoading(true, 0);
    m_importer->start();
}

void MainWindow::onImportDone()
{
    TrackImporter *imp = m_importer;
    m_importer = nullptr;
    imp->wait();
    m_statusBar->setLoading(false, 0);
    for (QAction *a : qAsConst(m_importActions))
        a->setEnabled(true);

    QString msg;
    const bool ok = imp->ok();
    if (ok) {
        const bool firstData = m_store.size() == 0;
        const int before = m_store.size();
        QVector<Track> &tracks = imp->tracks();
        // 后台入库（RadarView：batch_exists 时 append_missing_tracks，否则 save_batch），状态栏显示「保存中」
        if (!tracks.isEmpty() && trackdb::isOpen()) {
            m_saveQueue << new DbSaveJob(tracks.first().fileName, imp->kind(), tracks, this);
            startNextSave();
        }
        // 重新导入：清掉这些航迹的软删除，撤销栈里对应的条目作废
        m_globe->resetInteraction();
        const int added = m_store.addTracks(tracks);
        saveFileColors();
        msg = tr("%1：新增 %2 条航迹（共 %3 条，%4 个点），解析 %5 ms")
                  .arg(QFileInfo(imp->path()).fileName()).arg(added)
                  .arg(m_store.size()).arg(m_store.pointCount()).arg(imp->elapsedMs());
        // 管理面板有可见集合时，新导入的航迹也加入（RadarView revealImportedTracks）
        if (isolatedTrack() >= 0)
            isolateTrack(-1);
        if (!m_manage->visibleKeys().isEmpty()) {
            QStringList keys;
            for (int i = before; i < m_store.size(); ++i)
                keys << m_store.tracks()[i].key();
            m_manage->showAll(keys);
        }
        applyAdsbVisibility();
        refreshSources();
        m_filterPanel->setDataRange(m_store.minTime(), m_store.maxTime());
        applyDisplay();
        // 第一次导入时把视角移到数据中心
        if (firstData && m_store.size() > before) {
            double sx = 0, sy = 0, sz = 0;
            for (const Track &t : m_store.tracks()) {
                const TrackPoint &p = t.points.first();
                const double la = qDegreesToRadians(p.lat), lo = qDegreesToRadians(p.lon);
                sx += std::cos(la) * std::cos(lo);
                sy += std::cos(la) * std::sin(lo);
                sz += std::sin(la);
            }
            m_globe->lookAt(qRadiansToDegrees(std::atan2(sy, sx)),
                            qRadiansToDegrees(std::atan2(sz, std::sqrt(sx * sx + sy * sy))), 6000.0);
        }
        m_globe->update();
    } else {
        msg = imp->errorString();
        m_statusBar->setError(msg);
        if (!property("noDialogs").toBool())
            QMessageBox::warning(this, tr("导入失败"), msg);
    }
    imp->deleteLater();
    emit importFinished(ok, msg);
}

void MainWindow::startNextSave()
{
    if (m_saveJob || m_saveQueue.isEmpty()) {
        m_statusBar->setPersisting(isPersisting());
        return;
    }
    m_saveJob = m_saveQueue.takeFirst();
    m_statusBar->setPersisting(true);
    connect(m_saveJob, &QThread::finished, this, [this]() {
        DbSaveJob *job = m_saveJob;
        m_saveJob = nullptr;
        const bool ok = job->ok();
        QString msg;
        if (ok) {
            msg = QStringLiteral("%1：入库 %2 条").arg(job->fileName()).arg(job->savedCount());
        } else {
            msg = QStringLiteral("数据保存失败: %1").arg(job->errorString());
            m_statusBar->setError(msg);
            QTimer::singleShot(8000, this, [this, msg]() {
                if (m_statusBar->errorText() == msg)
                    m_statusBar->setError(QString());
            });
        }
        job->deleteLater();
        m_manage->markStale();
        startNextSave();
        emit persistFinished(ok, msg);
    });
    m_saveJob->start();
}

int MainWindow::isolatedTrack() const
{
    return m_isoKey.isEmpty() ? -1 : m_store.indexOf(m_isoKey);
}

// 回放范围跟随地图显示集合（RadarView useReplay(displayTracks)），时间筛选时取窗口内的部分
void MainWindow::resetReplayRange()
{
    qint64 lo = 0, hi = 0;
    bool any = false;
    TrackLayer *layer = m_globe->trackLayer();
    const bool timeClip = m_manage->visibleKeys().isEmpty() && isolatedTrack() < 0 && m_filter.timeActive;
    for (int i = 0; i < m_store.size(); ++i) {
        if (!layer->isTrackShownIgnoringGroups(i))
            continue;
        qint64 a = m_store.tracks()[i].minTime(), b = m_store.tracks()[i].maxTime();
        if (timeClip) {
            a = qMax(a, m_filter.timeMin);
            b = qMin(b, m_filter.timeMax);
        }
        if (!any || a < lo)
            lo = a;
        if (!any || b > hi)
            hi = b;
        any = true;
    }
    m_replay->setRange(any ? lo : 0, any ? hi : 0);
}

// 地图显示集合（App.vue displayTracks）：管理面板可见集合 > 单独显示 > 筛选结果；软删除的已从内存移除
void MainWindow::applyDisplay()
{
    TrackLayer *layer = m_globe->trackLayer();
    const QSet<QString> &vis = m_manage->visibleKeys();
    const int iso = isolatedTrack();
    if (iso < 0)
        m_isoKey.clear();
    QVector<bool> mask;
    if (!vis.isEmpty()) {
        mask.resize(m_store.size());
        for (int i = 0; i < m_store.size(); ++i)
            mask[i] = vis.contains(m_store.tracks()[i].key());
        layer->setIsolatedTrack(-1);
        layer->setTimeFilter(false, 0, 0);
    } else if (iso >= 0) {
        layer->setIsolatedTrack(iso);
        layer->setTimeFilter(false, 0, 0);
    } else {
        layer->setIsolatedTrack(-1);
        if (m_filter.timeActive || m_filter.hasPointCountFilter()) {
            mask.resize(m_store.size());
            for (int i = 0; i < m_store.size(); ++i)
                mask[i] = m_filter.accepts(m_store.tracks()[i]);
        }
        layer->setTimeFilter(m_filter.timeActive, m_filter.timeMin, m_filter.timeMax);
    }
    layer->setTrackMask(mask);
    // 单独显示提示按钮只在单独显示真正生效时出现
    const bool isoShown = vis.isEmpty() && iso >= 0;
    m_backAllBtn->setVisible(isoShown);
    if (isoShown) {
        const Track &t = m_store.tracks()[iso];
        m_backAllBtn->setText(tr("← 返回全部（单独显示：%1）").arg(t.flightNo.isEmpty() ? t.id : t.flightNo));
        m_backAllBtn->adjustSize();
        placeBackAllButton();
        m_backAllBtn->raise();
    }
    // 点迹：删掉已不存在的航迹
    QVector<int> dots;
    for (auto it = m_dotKeys.begin(); it != m_dotKeys.end();) {
        const int idx = m_store.indexOf(*it);
        if (idx < 0) {
            it = m_dotKeys.erase(it);
        } else {
            dots << idx;
            ++it;
        }
    }
    std::sort(dots.begin(), dots.end());
    layer->setPointDotTracks(dots);
    resetReplayRange();
    m_globe->update();
}

int MainWindow::displayedTrackCount() const
{
    int n = 0;
    for (int i = 0; i < m_store.size(); ++i)
        if (m_globe->trackLayer()->isTrackShownIgnoringGroups(i))
            ++n;
    return n;
}

void MainWindow::isolateTrack(int index)
{
    if (index >= m_store.size())
        index = -1;
    if (index == isolatedTrack())
        return;
    m_isoKey = index >= 0 ? m_store.tracks()[index].key() : QString();
    applyDisplay();
}

// 与 RadarView statusSources 相同：ADS-B 一项（整体），雷达 / 原始量测按导入文件各一项（按首次出现顺序）
void MainWindow::refreshSources()
{
    QVector<AppStatusBar::SourceItem> items;
    AppStatusBar::SourceItem adsb;
    adsb.key = QStringLiteral("adsb");
    adsb.label = QStringLiteral("ADS-B");
    adsb.color = trackSourceColor(TrackSource::Adsb);
    adsb.visible = m_adsbVisible;
    QVector<AppStatusBar::SourceItem> radar, raw;
    TrackLayer *layer = m_globe->trackLayer();
    for (const Track &t : m_store.tracks()) {
        if (t.source == TrackSource::Adsb) {
            ++adsb.count;
            continue;
        }
        QVector<AppStatusBar::SourceItem> &list = t.source == TrackSource::Radar ? radar : raw;
        const QString key = trackSourceName(t.source) + QStringLiteral("::") + t.fileName;
        int i = 0;
        while (i < list.size() && list[i].key != key)
            ++i;
        if (i == list.size()) {
            AppStatusBar::SourceItem it;
            it.key = key;
            it.label = t.fileName;
            it.color = m_store.fileColor(t.source, t.fileName);
            it.visible = layer->isGroupVisible(key);
            list.append(it);
        }
        ++list[i].count;
    }
    items << adsb << radar << raw;
    m_statusBar->setSources(items);
    m_statusBar->setTrackCount(m_store.size());
}

void MainWindow::toggleSource(const QString &key)
{
    if (key == QLatin1String("adsb")) {
        m_adsbVisible = !m_adsbVisible;
        applyAdsbVisibility();
    } else {
        TrackLayer *layer = m_globe->trackLayer();
        layer->setGroupVisible(key, !layer->isGroupVisible(key));
    }
    refreshSources();
    m_globe->update();
}

// ADS-B 是整体开关：每个 ADS-B 文件组都跟随它（新导入的文件也一样）
void MainWindow::applyAdsbVisibility()
{
    TrackLayer *layer = m_globe->trackLayer();
    for (const Track &t : m_store.tracks())
        if (t.source == TrackSource::Adsb)
            layer->setGroupVisible(trackSourceName(t.source) + QStringLiteral("::") + t.fileName, m_adsbVisible);
}

void MainWindow::placeBackAllButton()
{
    m_backAllBtn->move((m_globe->width() - m_backAllBtn->width()) / 2, 8);
}

bool MainWindow::eventFilter(QObject *obj, QEvent *e)
{
    if (obj == m_globe && e->type() == QEvent::Resize)
        placeBackAllButton();
    return QMainWindow::eventFilter(obj, e);
}

QDialog *MainWindow::showTrackPoints(int index)
{
    if (index < 0 || index >= m_store.size())
        return nullptr;
    TrackPointDialog *dlg = new TrackPointDialog(m_store.tracks()[index], this);
    dlg->show();
    return dlg;
}

bool MainWindow::pointDotsShown(int index) const
{
    return index >= 0 && index < m_store.size() && m_dotKeys.contains(m_store.tracks()[index].key());
}

void MainWindow::togglePointDots(int index)
{
    if (index < 0 || index >= m_store.size())
        return;
    const QString key = m_store.tracks()[index].key();
    if (!m_dotKeys.remove(key))
        m_dotKeys.insert(key);
    applyDisplay();
}

// 「详细信息」：高亮该航迹并打开管理面板，按来源 + ICAO 搜索（App.vue onShowTrackDetail）
void MainWindow::showTrackDetail(int index)
{
    if (index < 0 || index >= m_store.size())
        return;
    const Track &t = m_store.tracks()[index];
    m_manage->addHighlight(t.id);
    m_manage->filter.source = trackSourceKey(t.source);
    m_manage->filter.searchText = t.id;
    m_manage->page = 1;
    m_manage->saveSettings();
    if (activePanel() != int(PanelId::Manage))
        activatePanel(PanelId::Manage);
    m_managePanel->refresh();
}

void MainWindow::deleteTrackFromMap(int index)
{
    if (index < 0 || index >= m_store.size())
        return;
    const Track t = m_store.tracks()[index];
    const QString src = trackSourceName(t.source);
    if (!ui::confirm(this, QStringLiteral("确定隐藏航迹 %1 (%2)？\n数据保留在数据库中，可通过撤销恢复。").arg(t.id, src),
                     QStringLiteral("隐藏航迹"), true))
        return;
    m_manage->softDelete(QStringList() << t.key(), QStringLiteral("航迹 %1 (%2)").arg(t.id, src));
}

void MainWindow::updateUndoToast()
{
    const QVector<ManageState::UndoEntry> &st = m_manage->undoStack();
    m_undoToast->setState(st.isEmpty() ? QString() : st.last().label, st.size());
}

// 地图右键菜单（CesiumMap.vue）：点迹开关、详细信息、查看点迹数据、删除
void MainWindow::onTrackContextMenu(int index, const QPoint &globalPos)
{
    if (index < 0 || index >= m_store.size())
        return;
    ui::ContextMenu menu(this);
    if (!pointDotsShown(index))
        menu.addItem(LucideIcon::Dot, QStringLiteral("显示所有对应点迹"), [this, index]() { togglePointDots(index); });
    else
        menu.addItem(LucideIcon::Circle, QStringLiteral("隐藏所有对应点迹"), [this, index]() { togglePointDots(index); });
    menu.addItem(LucideIcon::FileText, QStringLiteral("详细信息"), [this, index]() { showTrackDetail(index); });
    menu.addItem(LucideIcon::ClipboardList, QStringLiteral("查看点迹数据"), [this, index]() { showTrackPoints(index); });
    menu.addItem(LucideIcon::Trash2, QStringLiteral("删除该航迹"), [this, index]() { deleteTrackFromMap(index); }, true);
    menu.exec(globalPos);
}

void MainWindow::onFlagContextMenu(const QString &flagId, const QPoint &globalPos)
{
    const int i = m_flags->indexOf(flagId);
    if (i < 0)
        return;
    ui::ContextMenu menu(this);
    menu.addItem(LucideIcon::Pencil, QStringLiteral("重命名"), [this, flagId]() {
        const int k = m_flags->indexOf(flagId);
        QString name;
        if (k >= 0 && ui::prompt(this, QStringLiteral("请输入新名称："), m_flags->flags()[k].label, &name)
                && !name.trimmed().isEmpty())
            m_flags->renameFlag(flagId, name);
    });
    menu.addItem(LucideIcon::Trash2, QStringLiteral("删除"), [this, flagId]() {
        const int k = m_flags->indexOf(flagId);
        if (k >= 0 && ui::confirm(this, QStringLiteral("确定要删除旗标「%1」吗？").arg(m_flags->flags()[k].label),
                                  QStringLiteral("删除旗标")))
            m_flags->removeFlag(flagId);
    }, true);
    menu.exec(globalPos);
}

void MainWindow::syncReplayToLayer()
{
    m_globe->trackLayer()->setReplay(m_replay->isActive(), m_replay->current());
    m_globe->update();
}

void MainWindow::toggleLabels()
{
    m_globe->setShowLabels(!m_globe->showLabels());
    app::settings().setValue(QStringLiteral("display.show_labels"), m_globe->showLabels());
    if (m_labelAction)
        m_labelAction->setChecked(m_globe->showLabels());
}

// Esc：标尺开着时关标尺，否则取消单独显示（App.vue keydown）
void MainWindow::onEscape()
{
    if (m_ruler->isActive())
        m_ruler->deactivate();
    else if (isolatedTrack() >= 0)
        isolateTrack(-1);
}

// 菜单（MenuBar.vue）
void MainWindow::buildMenuBar()
{
    QMenuBar *mb = menuBar();
    auto item = [this](QMenu *m, const QString &text, const QKeySequence &seq, std::function<void()> fn) {
        QAction *a = m->addAction(text);
        if (!seq.isEmpty()) {
            a->setShortcut(seq);
            a->setShortcutContext(Qt::WindowShortcut);
        }
        connect(a, &QAction::triggered, this, fn);
        return a;
    };

    QMenu *mFile = mb->addMenu(tr("文件"));
    auto pick = [this](TrackSource kind) {
        const bool csv = kind == TrackSource::Adsb;
        const QString path = QFileDialog::getOpenFileName(
            this, csv ? tr("导入 ADS-B 数据") : tr("导入雷达数据"), QString(),
            csv ? tr("ADS-B 数据 (*.csv);;所有文件 (*)") : tr("MATLAB 数据 (*.mat);;所有文件 (*)"));
        if (!path.isEmpty())
            importFile(kind, path);
    };
    QAction *aImportAdsb = item(mFile, tr("导入 ADS-B 数据..."), QKeySequence(Qt::CTRL + Qt::Key_O),
                                [pick]() { pick(TrackSource::Adsb); });
    QAction *aImportRadar = item(mFile, tr("导入雷达数据..."), QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_O),
                                 [pick]() { pick(TrackSource::Radar); });
    QAction *aImportRaw = item(mFile, tr("导入雷达原始测量数据..."), QKeySequence(), [pick]() { pick(TrackSource::RadarRaw); });
    m_importActions = { aImportAdsb, aImportRadar, aImportRaw };
    item(mFile, tr("清空航迹"), QKeySequence(), [this]() {
        if (m_importer)
            return;
        m_globe->resetInteraction();
        m_isoKey.clear();
        m_dotKeys.clear();
        m_replay->stop();
        m_store.clear();
        m_manage->clearVisible();
        refreshSources();
        applyDisplay();
    });
    mFile->addSeparator();
    item(mFile, tr("打开瓦片 (.mbtiles)..."), QKeySequence(), [this]() { openTileFile(); });
    mFile->addSeparator();
    QAction *aExit = item(mFile, tr("退出"), QKeySequence(Qt::ALT + Qt::Key_F4), []() { qApp->quit(); });
    aExit->setShortcutContext(Qt::WidgetShortcut);   // Alt+F4 由系统处理，这里只显示

    QMenu *mEdit = mb->addMenu(tr("编辑"));
    item(mEdit, tr("清除选中"), QKeySequence(Qt::Key_Escape), [this]() { onEscape(); });
    mEdit->addSeparator();
    item(mEdit, tr("首选项设置"), QKeySequence(Qt::CTRL + Qt::Key_Comma), [this]() { activatePanel(PanelId::Settings); });

    QMenu *mView = mb->addMenu(tr("视图"));
    item(mView, tr("轨迹面板"), QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_T), [this]() { activatePanel(PanelId::Tracks); });
    item(mView, tr("航迹管理系统"), QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_M), [this]() { activatePanel(PanelId::Manage); });
    item(mView, tr("图层控制"), QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_L), [this]() { activatePanel(PanelId::Layers); });
    item(mView, tr("旗标面板"), QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_F), [this]() { activatePanel(PanelId::Flags); });
    item(mView, tr("时间过滤"), QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_E), [this]() { activatePanel(PanelId::TimeFilter); });
    mView->addSeparator();
    item(mView, tr("重置地图视角"), QKeySequence(Qt::CTRL + Qt::Key_R), [this]() { m_globe->resetView(); });
    m_labelAction = item(mView, tr("切换标签显示"), QKeySequence(Qt::CTRL + Qt::Key_T), [this]() { toggleLabels(); });
    m_labelAction->setCheckable(true);
    item(mView, tr("放大"), QKeySequence::ZoomIn, [this]() { m_globe->zoomBy(1.0); });
    item(mView, tr("缩小"), QKeySequence::ZoomOut, [this]() { m_globe->zoomBy(-1.0); });

    QMenu *mTools = mb->addMenu(tr("工具"));
    item(mTools, tr("旗标管理"), QKeySequence(), [this]() { activatePanel(PanelId::Flags); });
    item(mTools, tr("清除所有旗标"), QKeySequence(), [this]() {
        if (ui::confirm(this, QStringLiteral("确定要清除地图上所有旗标吗？此操作不可撤销。"), QStringLiteral("清除旗标")))
            m_flags->clearAll();
    });

    QMenu *mHelp = mb->addMenu(tr("帮助"));
    item(mHelp, tr("关于 RadarView"), QKeySequence(), [this]() {
        QMessageBox::about(this, tr("关于 RadarView"), tr("RadarView（Qt 版）\n雷达 / ADS-B 航迹导入、显示与回放"));
    });
    item(mHelp, tr("关于 Qt"), QKeySequence(), []() { QApplication::aboutQt(); });
}

QStackedWidget *MainWindow::buildEditorArea()
{
    QStackedWidget *stack = new QStackedWidget;
    stack->setObjectName("editorArea");
    m_globe = new GlobeWidget(stack);
    stack->addWidget(m_globe);
    return stack;
}

void MainWindow::onIconClicked(int index)
{
    activatePanel(PanelId(index));
}

int MainWindow::activePanel() const
{
    return m_sidePanel->isVisible() || (!isVisible() && !m_sidePanel->isHidden()) ? m_panel : -1;
}

// useActivityBar.activate：点当前面板关闭侧栏，否则切换并恢复该面板记住的宽度
void MainWindow::activatePanel(PanelId id)
{
    const int i = int(id);
    if (m_panel == i && !m_sidePanel->isHidden()) {
        closeSidebar();
        return;
    }
    if (m_panel >= 0 && !m_sidePanel->isHidden())
        m_panelWidth[m_panel] = m_splitter->sizes().value(0);
    m_panel = i;
    m_sidePanel->showPanel(i, QString::fromUtf8(kPanels[i].title));
    m_sidePanel->show();
    const int w = qBound(kSidebarMin, m_panelWidth.value(i, kSidebarDefault), kSidebarMax);
    const int total = qMax(m_splitter->width(), w + 200);
    m_splitter->setSizes(QList<int>() << w << total - w);
    m_activityBar->highlight(i);
    if (id == PanelId::Manage)
        m_managePanel->refresh();
}

void MainWindow::closeSidebar()
{
    if (m_panel >= 0 && !m_sidePanel->isHidden())
        m_panelWidth[m_panel] = m_splitter->sizes().value(0);
    m_sidePanel->hide();
    m_panel = -1;
    m_activityBar->highlight(-1);
}
