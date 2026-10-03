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
#include <QStyle>
#include <QStatusBar>
#include <QApplication>
#include <QFileDialog>
#include <QMessageBox>
#include <QDir>
#include <QFileInfo>
#include <QProgressBar>
#include <QKeySequence>
#include <QEvent>
#include <QDialog>
#include <QtMath>
#include <cmath>

#include "globewidget.h"
#include "trackimporter.h"
#include "replaycontroller.h"
#include "replaybar.h"
#include "tracklayer.h"
#include "trackpointdialog.h"

// ---------------------------------------------------------------
//  左侧活动栏：竖排 5 个图标
// ---------------------------------------------------------------
ActivityBar::ActivityBar(QWidget *parent)
    : QWidget(parent)
{
    setObjectName("activityBar");
    setFixedWidth(64);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);

    QVBoxLayout *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 8, 0, 8);
    lay->setSpacing(12);

    QStyle *st = style();
    // 用 QStyle 内置标准图标，零外部资源依赖，Win7 上同样可用
    const QStyle::StandardPixmap pmaps[5] = {
        QStyle::SP_DirIcon,             // 资源管理器
        QStyle::SP_FileDialogContentsView, // 搜索
        QStyle::SP_FileIcon,            // 源代码管理
        QStyle::SP_ArrowDown,           // 运行 / 调试
        QStyle::SP_DialogSaveButton     // 扩展 / 账户
    };

    for (int i = 0; i < 5; ++i) {
        QPushButton *b = new QPushButton(this);
        b->setIcon(st->standardIcon(pmaps[i], nullptr));
        b->setIconSize(QSize(32, 32));
        b->setFixedSize(56, 56);
        b->setFlat(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setToolTip(QStringLiteral("活动栏 %1").arg(i + 1));
        b->setStyleSheet(
            "QPushButton { border: 1px solid #3a3d41; border-radius: 4px; }"
            "QPushButton:hover { background-color: #3a3d41; }"
            "QPushButton[active=\"true\"] { background-color: #0078d4; border-color: #0078d4; }");
        m_buttons[i] = b;

        lay->addWidget(b, 0, Qt::AlignHCenter);

        const int idx = i;
        connect(b, &QPushButton::clicked,
                this, [this, idx]() { emit iconClicked(idx); });
    }

    lay->addStretch(1);
}

void ActivityBar::highlight(int index)
{
    m_activeIndex = index;
    for (int i = 0; i < 5; ++i)
        m_buttons[i]->setProperty("active", i == index);
}

int ActivityBar::activeIndex() const
{
    return m_activeIndex;
}

// ---------------------------------------------------------------
//  侧栏：可折叠，内容为空占位
// ---------------------------------------------------------------
SidePanel::SidePanel(QWidget *parent)
    : QWidget(parent)
{
    setObjectName("sidePanel");
    setMinimumWidth(160);
    setMaximumWidth(600);

    QVBoxLayout *lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 6, 8, 6);
    lay->setSpacing(4);

    m_title = new QLabel(this);
    m_title->setStyleSheet("font-weight: bold; font-size: 15px; padding: 6px;");
    lay->addWidget(m_title);

    // 要求：侧栏里可以“啥也没有”，给个居中占位
    QLabel *empty = new QLabel(tr("（侧栏为空）"), this);
    empty->setAlignment(Qt::AlignCenter);
    lay->addWidget(empty, 1);
}

void SidePanel::setHeaderText(const QString &text)
{
    m_title->setText(text);
}

// ---------------------------------------------------------------
//  主窗口
// ---------------------------------------------------------------
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("Hello Qt — 仿 VSCode 框架"));

    buildMenuBar();

    QWidget *central = new QWidget(this);
    QHBoxLayout *h = new QHBoxLayout(central);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);

    m_activityBar = new ActivityBar(central);

    m_splitter = new QSplitter(Qt::Horizontal, central);
    m_sidePanel = new SidePanel();
    m_editor = buildEditorArea();

    m_splitter->addWidget(m_sidePanel);
    m_splitter->addWidget(m_editor);
    m_splitter->setCollapsible(0, true);  // 侧栏可折叠
    m_splitter->setCollapsible(1, false); // 编辑器不可折叠
    m_splitter->setChildrenCollapsible(false);
    // 分隔条：加宽 + 灰色可见 + 悬停高亮，便于识别和拖拽
    m_splitter->setHandleWidth(5);
    m_splitter->setStyleSheet(
        "QSplitter::separator { background-color: #3a3d41; width: 5px; height: 5px; }"
        "QSplitter::separator:hover { background-color: #0078d4; }");

    // 两侧不同底色，区分侧栏与编辑器区
    m_sidePanel->setStyleSheet(
        "#sidePanel { background-color: #252526; color: #cccccc; }"
        "#sidePanel QLabel { color: #cccccc; }");
    if (m_editor)
        m_editor->setStyleSheet(
            "#editorArea { background-color: #1e1e1e; }"
            "#editorArea QLabel { color: #999999; font-size: 14px; }");

    h->addWidget(m_activityBar);
    h->addWidget(m_splitter, 1);

    setCentralWidget(central);

    // 初始：侧栏展开，活动栏不高亮
    m_splitter->setSizes(QList<int>() << 220 << 800);
    m_activityBar->highlight(-1);

    connect(m_activityBar, &ActivityBar::iconClicked,
            this, &MainWindow::onIconClicked);

    // 状态栏常驻：瓦片名、层级、视点高度、鼠标经纬度
    m_globeStatus = new QLabel(this);
    statusBar()->addPermanentWidget(m_globeStatus, 1);
    connect(m_globe, &GlobeWidget::statusChanged, m_globeStatus, &QLabel::setText);

    // 导入进度 + 航迹数
    m_importProgress = new QProgressBar(this);
    m_importProgress->setRange(0, 100);
    m_importProgress->setFixedWidth(160);
    m_importProgress->setTextVisible(true);
    m_importProgress->hide();
    statusBar()->addPermanentWidget(m_importProgress);
    m_trackCount = new QLabel(tr("航迹: 0"), this);
    statusBar()->addPermanentWidget(m_trackCount);

    m_globe->setTrackStore(&m_store);

    // 回放：控件放状态栏左侧（与 RadarView 一致）
    m_replay = new ReplayController(this);
    m_replayBar = new ReplayBar(m_replay, this);
    statusBar()->addWidget(m_replayBar);
    connect(m_replay, &ReplayController::timeChanged, this, &MainWindow::syncReplayToLayer);
    connect(m_replay, &ReplayController::stateChanged, this, &MainWindow::syncReplayToLayer);
    QAction *aPlay = new QAction(tr("播放/暂停"), this);
    aPlay->setShortcut(Qt::Key_Space);
    aPlay->setShortcutContext(Qt::WindowShortcut);
    connect(aPlay, &QAction::triggered, m_replay, &ReplayController::togglePlay);
    addAction(aPlay);

    // 地图交互：单击航迹单独显示，单击空白处返回全部；右键菜单
    connect(m_globe, &GlobeWidget::trackClicked, this, [this](int index) {
        if (index >= 0)
            isolateTrack(index);
        else if (isolatedTrack() >= 0)
            isolateTrack(-1);
    });
    connect(m_globe, &GlobeWidget::trackContextMenuRequested, this, &MainWindow::onTrackContextMenu);
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
    QAction *aBackAll = new QAction(tr("返回全部"), this);
    aBackAll->setShortcut(Qt::Key_Escape);
    aBackAll->setShortcutContext(Qt::WindowShortcut);
    connect(aBackAll, &QAction::triggered, this, [this]() {
        if (isolatedTrack() >= 0)
            isolateTrack(-1);
    });
    addAction(aBackAll);

    statusBar()->showMessage(tr("就绪"), 3000);

    resize(1280, 800);
}

bool MainWindow::openTiles(const QString &path)
{
    QString err;
    if (!m_globe->openTiles(path, &err)) {
        QMessageBox::warning(this, tr("打开瓦片失败"), err);
        return false;
    }
    statusBar()->showMessage(tr("已加载瓦片：%1").arg(QDir::toNativeSeparators(path)), 4000);
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
    if (best.isEmpty())
        statusBar()->showMessage(tr("未找到 .mbtiles，可通过“文件 → 打开瓦片”加载"), 6000);
    else
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
    if (m_importer) {
        statusBar()->showMessage(tr("正在导入，请稍候"), 3000);
        return;
    }
    QString name = displayName;
    if (name.isEmpty() && kind != TrackSource::Adsb)
        name = m_store.uniqueFileName(kind);
    m_importer = new TrackImporter(kind, path, name, this);
    connect(m_importer, &TrackImporter::progressChanged, m_importProgress, &QProgressBar::setValue);
    connect(m_importer, &TrackImporter::importDone, this, &MainWindow::onImportDone);
    for (QAction *a : qAsConst(m_importActions))
        a->setEnabled(false);
    m_importProgress->setValue(0);
    m_importProgress->setFormat(tr("导入 %p%"));
    m_importProgress->show();
    statusBar()->showMessage(tr("正在解析 %1 ...").arg(QFileInfo(path).fileName()));
    m_importer->start();
}

void MainWindow::onImportDone()
{
    TrackImporter *imp = m_importer;
    m_importer = nullptr;
    imp->wait();
    m_importProgress->hide();
    for (QAction *a : qAsConst(m_importActions))
        a->setEnabled(true);

    QString msg;
    const bool ok = imp->ok();
    if (ok) {
        const bool firstData = m_store.size() == 0;
        const int before = m_store.size();
        const int added = m_store.addTracks(std::move(imp->tracks()));
        msg = tr("%1：新增 %2 条航迹（共 %3 条，%4 个点），解析 %5 ms")
                  .arg(QFileInfo(imp->path()).fileName()).arg(added)
                  .arg(m_store.size()).arg(m_store.pointCount()).arg(imp->elapsedMs());
        m_trackCount->setText(tr("航迹: %1").arg(m_store.size()));
        resetReplayRange();
        statusBar()->showMessage(msg, 8000);
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
        statusBar()->showMessage(tr("导入失败：%1").arg(msg), 8000);
        if (!property("noDialogs").toBool())
            QMessageBox::warning(this, tr("导入失败"), msg);
    }
    imp->deleteLater();
    emit importFinished(ok, msg);
}

int MainWindow::isolatedTrack() const
{
    return m_globe->trackLayer()->isolatedTrack();
}

// 回放范围：单独显示时为该航迹的时间段，否则为全部数据（与 RadarView 一致）
void MainWindow::resetReplayRange()
{
    const int iso = isolatedTrack();
    if (iso >= 0 && iso < m_store.size())
        m_replay->setRange(m_store.tracks()[iso].minTime(), m_store.tracks()[iso].maxTime());
    else
        m_replay->setRange(m_store.minTime(), m_store.maxTime());
}

void MainWindow::isolateTrack(int index)
{
    if (index >= m_store.size())
        index = -1;
    if (index == isolatedTrack())
        return;
    m_globe->trackLayer()->setIsolatedTrack(index);
    m_backAllBtn->setVisible(index >= 0);
    if (index >= 0) {
        const Track &t = m_store.tracks()[index];
        m_backAllBtn->setText(tr("← 返回全部（单独显示：%1）").arg(t.flightNo.isEmpty() ? t.id : t.flightNo));
        m_backAllBtn->adjustSize();
        placeBackAllButton();
        m_backAllBtn->raise();
        statusBar()->showMessage(tr("单独显示 %1，单击空白处或按 Esc 返回全部").arg(t.flightNo.isEmpty() ? t.id : t.flightNo), 5000);
    }
    resetReplayRange();
    m_globe->update();
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

void MainWindow::onTrackContextMenu(int index, const QPoint &globalPos)
{
    QMenu menu(this);
    if (index >= 0) {
        const Track &t = m_store.tracks()[index];
        QAction *title = menu.addAction(t.flightNo.isEmpty() ? t.id : t.flightNo);
        title->setEnabled(false);
        menu.addSeparator();
        QAction *aPoints = menu.addAction(tr("查看点迹数据"));
        connect(aPoints, &QAction::triggered, this, [this, index]() { showTrackPoints(index); });
        if (isolatedTrack() != index) {
            QAction *aIso = menu.addAction(tr("单独显示该航迹"));
            connect(aIso, &QAction::triggered, this, [this, index]() { isolateTrack(index); });
        }
    }
    if (isolatedTrack() >= 0) {
        QAction *aBack = menu.addAction(tr("← 返回全部"));
        connect(aBack, &QAction::triggered, this, [this]() { isolateTrack(-1); });
    }
    if (!menu.isEmpty())
        menu.exec(globalPos);
}

void MainWindow::syncReplayToLayer()
{
    m_globe->trackLayer()->setReplay(m_replay->isActive(), m_replay->current());
    m_globe->update();
}

void MainWindow::buildMenuBar()
{
    QMenuBar *mb = menuBar();
    // 调大菜单字号，便于阅读
    mb->setStyleSheet(
        "QMenuBar { font-size: 20px; padding: 6px; }"
        "QMenuBar::item { padding: 6px 14px; }"
        "QMenu { font-size: 18px; }"
        "QMenu::item { padding: 8px 28px; }");

    // 文件
    QMenu *mFile = mb->addMenu(tr("文件"));
    QAction *aImportAdsb = mFile->addAction(tr("导入 ADS-B 数据 (.csv)..."));
    aImportAdsb->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_O));
    QAction *aImportRadar = mFile->addAction(tr("导入雷达数据 (.mat)..."));
    aImportRadar->setShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_O));
    QAction *aImportRaw = mFile->addAction(tr("导入雷达原始量测数据 (.mat)..."));
    m_importActions = { aImportAdsb, aImportRadar, aImportRaw };
    auto pick = [this](TrackSource kind) {
        const bool csv = kind == TrackSource::Adsb;
        const QString path = QFileDialog::getOpenFileName(
            this, csv ? tr("导入 ADS-B 数据") : tr("导入雷达数据"), QString(),
            csv ? tr("ADS-B 数据 (*.csv);;所有文件 (*)") : tr("MATLAB 数据 (*.mat);;所有文件 (*)"));
        if (!path.isEmpty())
            importFile(kind, path);
    };
    connect(aImportAdsb, &QAction::triggered, this, [pick]() { pick(TrackSource::Adsb); });
    connect(aImportRadar, &QAction::triggered, this, [pick]() { pick(TrackSource::Radar); });
    connect(aImportRaw, &QAction::triggered, this, [pick]() { pick(TrackSource::RadarRaw); });
    QAction *aClearTracks = mFile->addAction(tr("清空航迹"));
    connect(aClearTracks, &QAction::triggered, this, [this]() {
        if (m_importer)
            return;
        isolateTrack(-1);
        m_store.clear();
        m_globe->trackLayer()->setHoveredTrack(-1);
        m_replay->setRange(0, 0);
        m_trackCount->setText(tr("航迹: 0"));
        m_globe->update();
    });
    mFile->addSeparator();
    QAction *aOpenTiles = mFile->addAction(tr("打开瓦片 (.mbtiles)..."));
    connect(aOpenTiles, &QAction::triggered, this, &MainWindow::openTileFile);
    mFile->addSeparator();
    QAction *aExit = mFile->addAction(tr("退出"));

    // 编辑
    QMenu *mEdit = mb->addMenu(tr("编辑"));
    mEdit->addAction(tr("撤销"));
    mEdit->addAction(tr("重做"));
    mEdit->addSeparator();
    mEdit->addAction(tr("剪切"));
    mEdit->addAction(tr("复制"));
    mEdit->addAction(tr("粘贴"));
    mEdit->addSeparator();
    mEdit->addAction(tr("全选"));

    // 视图
    QMenu *mView = mb->addMenu(tr("视图"));
    QAction *aTogglePanel = mView->addAction(tr("显示/隐藏侧栏"));
    QAction *aZoomIn = mView->addAction(tr("放大"));
    QAction *aZoomOut = mView->addAction(tr("缩小"));
    QAction *aReset = mView->addAction(tr("重置视角"));
    aZoomIn->setShortcut(QKeySequence::ZoomIn);
    aZoomOut->setShortcut(QKeySequence::ZoomOut);
    aReset->setShortcut(Qt::Key_Home);
    connect(aZoomIn, &QAction::triggered, this, [this]() { m_globe->zoomBy(1.0); });
    connect(aZoomOut, &QAction::triggered, this, [this]() { m_globe->zoomBy(-1.0); });
    connect(aReset, &QAction::triggered, this, [this]() { m_globe->resetView(); });

    // 运行
    QMenu *mRun = mb->addMenu(tr("运行"));
    mRun->addAction(tr("运行项目"));
    mRun->addAction(tr("调试"));

    // 帮助
    QMenu *mHelp = mb->addMenu(tr("帮助"));
    QAction *aAbout = mHelp->addAction(tr("关于"));
    QAction *aAboutQt = mHelp->addAction(tr("关于 Qt"));

    connect(aExit, &QAction::triggered, qApp, &QApplication::quit);
    connect(aTogglePanel, &QAction::triggered, this, &MainWindow::toggleSidePanel);
    connect(aAbout, &QAction::triggered, this, &MainWindow::onMenuActionTriggered);
    connect(aAboutQt, &QAction::triggered, qApp, &QApplication::aboutQt);

}

QStackedWidget *MainWindow::buildEditorArea()
{
    QStackedWidget *stack = new QStackedWidget;
    stack->setObjectName("editorArea");
    m_globe = new GlobeWidget(stack);
    stack->addWidget(m_globe);
    return stack;
}

void MainWindow::onMenuActionTriggered()
{
    statusBar()->showMessage(tr("你点击了“关于”"), 2000);
}

void MainWindow::onIconClicked(int index)
{
    m_activityBar->highlight(index);

    QString titles[5] = {
        tr("资源管理器"), tr("搜索"), tr("源代码管理"),
        tr("运行与调试"), tr("扩展 / 账户")
    };
    m_sidePanel->setHeaderText(titles[index]);

    // 若侧栏当前折叠（宽度 < 100）则展开
    QList<int> sizes = m_splitter->sizes();
    if (sizes.value(0) < 100)
        m_splitter->setSizes(QList<int>() << 220 << 800);

    statusBar()->showMessage(tr("已切换到：") + titles[index], 2000);
}

void MainWindow::toggleSidePanel()
{
    QList<int> sizes = m_splitter->sizes();
    if (sizes.value(0) > 0) {
        m_splitter->setSizes(QList<int>() << 0 << 1000);
        m_activityBar->highlight(-1);
    } else {
        m_splitter->setSizes(QList<int>() << 220 << 800);
        m_activityBar->highlight(0);
    }
}
