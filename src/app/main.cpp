#include "apppaths.h"
#include "drawingmanager.h"
#include "globewidget.h"
#include "mainwindow.h"
#include "point3dcanvas.h"
#include "point3dwindow.h"
#ifdef RADARVIEW_WITH_SELFTEST
#include "selftest/selftest.h"
#endif
#include "splashscreen.h"
#include "theme.h"
#include "uiwidgets.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QTimer>

namespace {

// 加载 exe 旁 fonts/ 目录里的字体。Linux 上把它设为界面默认字体，保证中文能显示；
// Windows 自带雅黑，只注册不替换
void loadBundledFonts()
{
    const QDir dir(QCoreApplication::applicationDirPath() + QStringLiteral("/fonts"));
    QStringList families;
    const QFileInfoList files = dir.entryInfoList(
        QStringList() << QStringLiteral("*.ttf") << QStringLiteral("*.ttc") << QStringLiteral("*.otf"), QDir::Files);
    for (const QFileInfo &fi : files) {
        const int id = QFontDatabase::addApplicationFont(fi.absoluteFilePath());
        if (id >= 0)
            families += QFontDatabase::applicationFontFamilies(id);
    }
#ifdef Q_OS_LINUX
    if (!families.isEmpty()) {
        QFont f = QApplication::font();
        f.setFamily(families.first());
        QApplication::setFont(f);
    }
#endif
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    loadBundledFonts();
    const QStringList args = app.arguments();
#ifdef RADARVIEW_WITH_SELFTEST
    const int pt = args.indexOf(QStringLiteral("--parsetest"));
    if (pt >= 0 && pt + 2 < args.size())
        return selftest::runParseTest(args.at(pt + 1), args.at(pt + 2));

    // 点迹可视化自检：--pointtest <目录|文件> [--track <航迹.mat>]
    const int p3t = args.indexOf(QStringLiteral("--pointtest"));
    if (p3t >= 0 && p3t + 1 < args.size()) {
        QString trackFile;
        const int tk = args.indexOf(QStringLiteral("--track"));
        if (tk >= 0 && tk + 1 < args.size())
            trackFile = args.at(tk + 1);
        app::setTestMode(true);          // 窗口设置写临时库，不碰真实配置
        ui::setAutoConfirm(true);
        return selftest::runPointTest(args.at(p3t + 1), trackFile);
    }

    // 数据库迁移自检：--dbtest（造 v2 旧库 → 迁移到 v6）
    if (args.contains(QStringLiteral("--dbtest")))
        return selftest::runDbMigrationTest();

    if (args.contains(QStringLiteral("--statecheck"))) {
        app::setTestMode(true);
        ui::setAutoConfirm(true);
        return selftest::runStateCheck();
    }
#endif

    // 只用临时数据目录跑一遍（截图/回归用，不碰真实数据库和设置）
    if (args.contains(QStringLiteral("--testmode"))) {
        app::setTestMode(true);
        ui::setAutoConfirm(true);
    }

    // 自检：数据库和设置用 exe 旁的临时文件（启动时清空），确认框自动通过
#ifdef RADARVIEW_WITH_SELFTEST
    const bool testRun = args.contains(QStringLiteral("--selftest")) || args.contains(QStringLiteral("--tracktest"));
#else
    const bool testRun = false;
#endif
    if (testRun) {
        app::setTestMode(true);
        ui::setAutoConfirm(true);
    }
    // 启动页：双击后立刻给反馈。自检 / 截图模式不显示，免得干扰断言与截图
    SplashScreen *splash = nullptr;
    if (!testRun && !args.contains(QStringLiteral("--testmode"))
            && !args.contains(QStringLiteral("--nosplash"))) {
        splash = new SplashScreen;
        splash->showNow();
    }

    UiScale::instance()->applyAppFont();   // 按保存的根字号缩放默认字体（自检时读的是临时设置）

    MainWindow w;
    // 启动自动最大化（保留任务栏）；自检/tracktest 保持普通窗口，便于断言布局
    if (testRun)
        w.show();
    else
        w.showMaximized();
    w.loadDefaultTiles();

    // 主窗口已经上屏：启动页过了最短展示时间后淡出
    if (splash)
        splash->finish();

    // 命令行直接指定在线底图 key（也方便自动化验证）：--online-key <天地图 key>
    const int ok = args.indexOf(QStringLiteral("--online-key"));
    if (ok >= 0 && ok + 1 < args.size())
        w.openOnlineMap(args.at(ok + 1), args.contains(QStringLiteral("--amap")));

    // 指定视角后再抓帧（回归验证用）：--look 经度,纬度,视高km。
    // 延到事件循环开始后再设，否则会被 loadPersisted() 里「自动飞到航迹中心」覆盖掉。
    const int lookIdx = args.indexOf(QStringLiteral("--look"));
    if (lookIdx >= 0 && lookIdx + 1 < args.size()) {
        const QStringList p = args.at(lookIdx + 1).split(QLatin1Char(','));
        if (p.size() >= 3) {
            const double lon = p[0].toDouble(), lat = p[1].toDouble(), alt = p[2].toDouble();
            QTimer::singleShot(0, &w, [&w, lon, lat, alt]() { w.globe()->lookAt(lon, lat, alt); });
        }
    }

    // 启动时加载一份绘图 GeoJSON（回归截图用）：--drawings <file>
    const int drIdx = args.indexOf(QStringLiteral("--drawings"));
    if (drIdx >= 0 && drIdx + 1 < args.size()) {
        QString drawErr;
        if (!w.drawing()->loadFromFile(args.at(drIdx + 1), &drawErr))
            qWarning("加载绘图失败：%s", qPrintable(drawErr));
    }

    // 绘图演示（回归截图用）：--draw-demo，往地图上放几个典型图形
    if (args.contains(QStringLiteral("--draw-demo"))) {
        w.setDrawingBarVisible(true);          // 截图里要能看到工具栏
        DrawingManager *d = w.drawing();
        const int rid = d->addRect(30.0, 95.0, 40.0, 115.0, QColor("#00B0F0"));
        d->setFill(rid, QColor(0x00, 0xb0, 0xf0, 70));
        d->addCircle(35.0, 105.0, 600.0, QColor("#FFC000"));
        const int demoLine = d->addLine(32.0, 100.0, 38.0, 112.0, QColor("#FF4444"));
        // 顺手演示打断：断点处会画一个白心黑边的小圆点
        if (const DrawingFeature *dl = d->feature(demoLine))
            d->breakFeature(demoLine, dl->pts.size() / 2);
        d->addText(33.0, 101.0, QStringLiteral("重点区域"), QColor("#FFFF00"), 22);
    }

    // 展开状态栏上方的数据源条（回归截图用）：--open-sources。
    // 同 --look：延到事件循环开始后再设，等主窗口首次布局完成
    if (args.contains(QStringLiteral("--open-sources")))
        QTimer::singleShot(0, &w, [&w]() { w.setSourceStripOpen(true); });

    // 抓一帧后退出（回归验证用）：--shot <png> [--shot-delay 毫秒] [--shot-window]
    //   默认只抓 GL 帧（地球+航迹）；--shot-window 抓整窗，连旗标/标签/状态栏一起，
    //   因为叠加层是 GlobeOverlay 子控件，grabFramebuffer 拍不到它
    const int shotIdx = args.indexOf(QStringLiteral("--shot"));
    if (shotIdx >= 0 && shotIdx + 1 < args.size()) {
        const QString path = args.at(shotIdx + 1);
        int delayMs = 12000;
        const int dIdx = args.indexOf(QStringLiteral("--shot-delay"));
        if (dIdx >= 0 && dIdx + 1 < args.size())
            delayMs = args.at(dIdx + 1).toInt();
        const bool wholeWindow = args.contains(QStringLiteral("--shot-window"));
        QTimer::singleShot(delayMs, &w, [&w, path, wholeWindow]() {
            if (wholeWindow)
                w.grab().save(path);
            else if (!w.globe()->grabFramebuffer().save(path))
                qWarning("抓帧保存失败：%s", qPrintable(path));
            QCoreApplication::quit();
        });
    }

    // 点迹窗口端到端冒烟：--pointdemo <目录|文件> [--point-shot <png>]
    // 走「主窗口新建窗口 → 后台导入 → 截图」的真实集成路径
    const int pdIdx = args.indexOf(QStringLiteral("--pointdemo"));
    if (pdIdx >= 0 && pdIdx + 1 < args.size()) {
        const QString input = args.at(pdIdx + 1);
        QString shot;
        const int psIdx = args.indexOf(QStringLiteral("--point-shot"));
        if (psIdx >= 0 && psIdx + 1 < args.size())
            shot = args.at(psIdx + 1);
        QTimer::singleShot(0, &w, [&w, input, shot]() {
            w.showPoint3dWindow();
            const QList<Point3dWindow *> wins = w.point3dWindows();
            if (wins.isEmpty()) {
                qWarning("pointdemo: 点迹窗口创建失败");
                QCoreApplication::exit(2);
                return;
            }
            Point3dWindow *win = wins.last();
            QStringList files;
            const QFileInfo fi(input);
            if (fi.isDir()) {
                const QDir dir(input);
                files = dir.entryList(QStringList() << QStringLiteral("detPoint_*.mat"),
                                      QDir::Files, QDir::Name);
                for (QString &f : files)
                    f = dir.filePath(f);
            } else if (fi.isFile()) {
                files << input;
            }
            win->loadDetectionFiles(files);
            QTimer *poll = new QTimer(&w);
            poll->setInterval(200);
            QObject::connect(poll, &QTimer::timeout, &w, [&w, win, poll, shot]() {
                if (win->isLoading())
                    return;
                poll->stop();
                // 演示/回归：定位到最后一帧，看到完整历史累积
                if (win->frameCount() > 0)
                    win->seekToFrame(win->frameCount() - 1);
                qWarning("pointdemo: frames=%d current=%d visible=%d points0=%d",
                         win->dataset()->frames.size(), win->currentFrame(),
                         win->canvas()->visiblePointCount(),
                         win->dataset()->frames.isEmpty()
                             ? 0 : win->dataset()->frames.first().points.size());
                win->canvas()->repaint();
                if (!shot.isEmpty() && !win->grab().save(shot))
                    qWarning("pointdemo: 截图保存失败");
                QCoreApplication::exit(win->dataset()->frames.isEmpty() ? 3 : 0);
            });
            poll->start();
        });
    }

#ifdef RADARVIEW_WITH_SELFTEST
    if (app.arguments().contains(QStringLiteral("--selftest")))
        selftest::runSelfTest(&w);
    const int tt = args.indexOf(QStringLiteral("--tracktest"));
    if (tt >= 0 && tt + 2 < args.size())
        selftest::runTrackTest(&w, args.at(tt + 1), args.at(tt + 2));
#endif

    return app.exec();
}
