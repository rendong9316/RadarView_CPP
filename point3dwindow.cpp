#include "point3dwindow.h"

#include "apppaths.h"
#include "point3dcanvas.h"
#include "point3dimportjob.h"
#include "theme.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QShortcut>
#include <QSpinBox>
#include <QVBoxLayout>

#include <functional>

namespace {

QVariant setting(const char *key, const QVariant &def)
{
    return app::settings().value(QString::fromLatin1(key), def);
}

QString toolbarQss()
{
    return QStringLiteral(
        "QWidget#point3dToolbar { background: var(--bg-secondary);"
        " border-bottom: 1px solid var(--border-primary); }"
        "QWidget#point3dStatus { background: var(--bg-secondary);"
        " border-top: 1px solid var(--border-primary); }"
        "QLabel { color: var(--text-secondary); font-family: %1; font-size: 12px; }"
        "QPushButton { color: var(--text-secondary); background: var(--button-bg);"
        " border: 1px solid var(--border-primary); border-radius: 3px; padding: 4px 10px;"
        " font-family: %1; font-size: 12px; }"
        "QPushButton:hover { background: var(--button-hover); color: var(--text-primary); }"
        "QPushButton:checked { background: var(--accent-primary); color: #ffffff;"
        " border-color: var(--accent-primary); }"
        "QComboBox, QSpinBox { color: var(--text-primary); background: var(--input-bg);"
        " border: 1px solid var(--input-border); border-radius: 3px; padding: 2px 6px;"
        " font-family: %1; font-size: 12px; }"
        "QComboBox QAbstractItemView { background: var(--dropdown-bg); color: var(--dropdown-fg);"
        " selection-background-color: var(--dropdown-hover); }")
        .arg(ui::uiFamilies());
}

} // namespace

Point3dWindow::Point3dWindow(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlag(Qt::Window, true);
    setWindowTitle(QStringLiteral("点迹三维可视化"));
    buildUi();

    const QByteArray geo = setting("point3d.window_geometry", QByteArray()).toByteArray();
    if (geo.isEmpty() || !restoreGeometry(geo))
        resize(1200, 800);
}

Point3dWindow::~Point3dWindow()
{
    if (m_job) {
        m_job->requestCancel();
        m_job->wait();
    }
}

void Point3dWindow::setNumber(int n)
{
    m_number = qMax(1, n);
    setWindowTitle(m_number <= 1 ? QStringLiteral("点迹三维可视化")
                                 : QStringLiteral("点迹三维可视化 %1").arg(m_number));
}

void Point3dWindow::buildUi()
{
    m_restoring = true;

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- 工具栏 ----
    m_toolbar = new QWidget(this);
    m_toolbar->setObjectName(QStringLiteral("point3dToolbar"));
    QHBoxLayout *bar = new QHBoxLayout(m_toolbar);
    bar->setContentsMargins(ui::sz(8), ui::sz(6), ui::sz(8), ui::sz(6));
    bar->setSpacing(ui::sz(8));

    m_importBtn = new QPushButton(QStringLiteral("导入点迹…"), m_toolbar);
    m_importBtn->setToolTip(QStringLiteral("选择一个或多个 detPoint_*.mat 文件（可多选，单个也可以）"));
    bar->addWidget(m_importBtn);

    QLabel *viewLabel = new QLabel(QStringLiteral("视图"), m_toolbar);
    bar->addWidget(viewLabel);
    m_viewCombo = new QComboBox(m_toolbar);
    m_viewCombo->addItem(QStringLiteral("距离–速度–帧号"), int(Point3dViewMode::RVF));
    m_viewCombo->addItem(QStringLiteral("距离–方位–帧号"), int(Point3dViewMode::RAF));
    m_viewCombo->addItem(QStringLiteral("速度–方位–帧号"), int(Point3dViewMode::VAF));
    m_viewCombo->setToolTip(QStringLiteral("三维视图的 X/Y 轴组合，Z 轴恒为帧号"));
    bar->addWidget(m_viewCombo);

    QLabel *windowLabel = new QLabel(QStringLiteral("窗口"), m_toolbar);
    bar->addWidget(windowLabel);
    m_windowCombo = new QComboBox(m_toolbar);
    const int presets[] = { 20, 40, 70, 150 };
    for (int v : presets)
        m_windowCombo->addItem(QStringLiteral("%1 帧").arg(v), v);
    m_windowCombo->addItem(QStringLiteral("全部"), 0);
    m_windowCombo->addItem(QStringLiteral("自定义…"), -1);
    m_windowCombo->setToolTip(QStringLiteral("同时显示最近多少帧的点迹（「全部」= 整个数据集）"));
    bar->addWidget(m_windowCombo);
    m_windowSpin = new QSpinBox(m_toolbar);
    m_windowSpin->setRange(1, 100000);
    m_windowSpin->setValue(70);
    m_windowSpin->setVisible(false);
    m_windowSpin->setToolTip(QStringLiteral("自定义滑动窗口帧数"));
    bar->addWidget(m_windowSpin);

    bar->addSpacing(ui::sz(6));
    m_pointBtn = new QPushButton(QStringLiteral("点迹 [P]"), m_toolbar);
    m_pointBtn->setCheckable(true);
    m_pointBtn->setChecked(true);
    m_pointBtn->setToolTip(QStringLiteral("显示 / 隐藏点迹（P）"));
    bar->addWidget(m_pointBtn);
    m_trackBtn = new QPushButton(QStringLiteral("航迹 [T]"), m_toolbar);
    m_trackBtn->setCheckable(true);
    m_trackBtn->setChecked(true);
    m_trackBtn->setToolTip(QStringLiteral("显示 / 隐藏航迹（T）"));
    bar->addWidget(m_trackBtn);
    m_labelBtn = new QPushButton(QStringLiteral("标签 [L]"), m_toolbar);
    m_labelBtn->setCheckable(true);
    m_labelBtn->setChecked(true);
    m_labelBtn->setToolTip(QStringLiteral("显示 / 隐藏航迹编号（L）"));
    bar->addWidget(m_labelBtn);

    bar->addStretch(1);
    m_progress = new QProgressBar(m_toolbar);
    m_progress->setRange(0, 100);
    m_progress->setTextVisible(false);
    m_progress->setFixedWidth(ui::sz(140));
    m_progress->setVisible(false);
    bar->addWidget(m_progress);
    root->addWidget(m_toolbar);

    // ---- 画布 ----
    m_canvas = new Point3dCanvas(this);
    root->addWidget(m_canvas, 1);

    // ---- 底部状态 ----
    m_statusBar = new QWidget(this);
    m_statusBar->setObjectName(QStringLiteral("point3dStatus"));
    QHBoxLayout *statusLay = new QHBoxLayout(m_statusBar);
    statusLay->setContentsMargins(ui::sz(10), ui::sz(3), ui::sz(10), ui::sz(3));
    m_status = new QLabel(m_statusBar);
    m_status->setText(QStringLiteral("未导入点迹数据"));
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    statusLay->addWidget(m_status, 1);
    root->addWidget(m_statusBar);

    setThemedStyle(this, toolbarQss());

    // ---- 连接 ----
    connect(m_importBtn, &QPushButton::clicked, this, &Point3dWindow::importDetections);
    connect(m_viewCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &Point3dWindow::onViewModeChanged);
    connect(m_windowCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &Point3dWindow::onWindowLengthChanged);
    connect(m_windowSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int v) {
                if (!m_restoring)
                    applyWindowLength(v);
            });
    connect(m_pointBtn, &QPushButton::toggled, this, [this](bool on) {
        m_canvas->setShowDetections(on);
        if (!m_restoring) {
            app::setValue(QStringLiteral("point3d.show_detections"), on);
            updateStatus();
        }
    });
    connect(m_trackBtn, &QPushButton::toggled, this, [this](bool on) {
        m_canvas->setShowTracks(on);
        if (!m_restoring) {
            app::setValue(QStringLiteral("point3d.show_tracks"), on);
            updateStatus();
        }
    });
    connect(m_labelBtn, &QPushButton::toggled, this, [this](bool on) {
        m_canvas->setShowLabels(on);
        if (!m_restoring)
            app::setValue(QStringLiteral("point3d.show_labels"), on);
    });

    connect(m_canvas, &Point3dCanvas::hoverChanged, this, &Point3dWindow::updateStatus);
    connect(m_canvas, &Point3dCanvas::cameraChanged, this, &Point3dWindow::saveSettings);
    connect(m_canvas, &Point3dCanvas::trackFocusChanged,
            this, &Point3dWindow::trackFocusChanged);
    connect(m_canvas, &Point3dCanvas::trackActivated,
            this, &Point3dWindow::trackActivated);

    // 快捷键：P / T / L（窗口内生效）
    auto bindKey = [this](Qt::Key key, const std::function<void()> &fn) {
        QShortcut *sc = new QShortcut(QKeySequence(key), this);
        sc->setContext(Qt::WidgetWithChildrenShortcut);
        connect(sc, &QShortcut::activated, this, fn);
    };
    bindKey(Qt::Key_P, [this]() { m_pointBtn->toggle(); });
    bindKey(Qt::Key_T, [this]() { m_trackBtn->toggle(); });
    bindKey(Qt::Key_L, [this]() { m_labelBtn->toggle(); });

    // ---- 恢复全局默认参数 ----
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
    const QString src = setting("point3d.track_source", QStringLiteral("associated")).toString();
    Point3dTrackSource trackSource = Point3dTrackSource::Associated;
    if (src == QLatin1String("auto"))
        trackSource = Point3dTrackSource::Auto;
    else if (src == QLatin1String("smooth"))
        trackSource = Point3dTrackSource::Smooth;
    else if (src == QLatin1String("predicted"))
        trackSource = Point3dTrackSource::Predicted;
    m_canvas->setTrackSource(trackSource);
    m_canvas->setTrackLineWidth(setting("point3d.track_line_width", 2.0).toDouble());

    const double camYaw = setting("point3d.camera_yaw", 42.0).toDouble();
    const double camPitch = setting("point3d.camera_pitch", 23.0).toDouble();
    const double camZoom = setting("point3d.camera_zoom", 1.0).toDouble();
    m_canvas->setCamera(camYaw, camPitch, camZoom);

    // 滑动窗口
    const int len = setting("point3d.window_length", 0).toInt();
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

    m_restoring = false;
    updateStatus();
}

void Point3dWindow::importDetections()
{
    QString dir = setting("point3d.last_dir", QDir::homePath()).toString();
    const QString pattern = setting("point3d.file_pattern", QStringLiteral("detPoint_*.mat")).toString();
    const QString filter = QStringLiteral("MATLAB 数据 (%1);;所有文件 (*)").arg(pattern);
    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("导入点迹数据"), dir, filter);
    if (files.isEmpty())
        return;
    app::setValue(QStringLiteral("point3d.last_dir"), QFileInfo(files.first()).absolutePath());
    loadDetectionFiles(files);
}

void Point3dWindow::loadDetectionFiles(const QStringList &paths)
{
    if (paths.isEmpty())
        return;
    if (m_job) {
        m_job->requestCancel();
        m_job->wait();
        m_job->deleteLater();
        m_job = nullptr;
    }

    QStringList files;
    QSet<QString> seen;
    for (const QString &p : paths) {
        const QString c = QFileInfo(p).canonicalFilePath();
        const QString key = c.isEmpty() ? p : c;
        if (!seen.contains(key)) {
            seen.insert(key);
            files << key;
        }
    }
    if (files.isEmpty())
        return;

    Point3dImportOptions opt;
    opt.detVariable = setting("point3d.det_variable", QStringLiteral("DetPoint")).toString();
    opt.sysVariable = setting("point3d.syspara_variable", QStringLiteral("sysPara")).toString();
    opt.azimuthField = setting("point3d.azimuth_field", QStringLiteral("az")).toString();
    opt.maxFrames = setting("point3d.max_frames", 0).toInt();
    opt.continuity.frameGap = setting("point3d.continuity_frame_gap", 3).toInt();
    opt.continuity.rangeScale = setting("point3d.continuity_range_scale", 80.0).toDouble();
    opt.continuity.dopplerScale = setting("point3d.continuity_doppler_scale", 10.0).toDouble();
    opt.continuity.azimuthScale = setting("point3d.continuity_azimuth_scale", 6.0).toDouble();

    m_pendingFiles = files;
    m_job = new Point3dImportJob(files, opt, this);
    connect(m_job, &Point3dImportJob::progressChanged, this,
            [this](int pct) { setLoadingUi(true, pct); });
    connect(m_job, &Point3dImportJob::importDone, this, &Point3dWindow::onImportDone);
    setLoadingUi(true, 0);
    m_job->start();
}

void Point3dWindow::onImportDone()
{
    Point3dImportJob *job = m_job;
    m_job = nullptr;
    if (!job)
        return;
    job->wait();
    setLoadingUi(false, 100);

    if (job->cancelled()) {
        m_status->setText(QStringLiteral("导入已取消"));
        job->deleteLater();
        return;
    }
    if (job->dataset().frames.isEmpty()) {
        const QString summary = job->errors().isEmpty()
            ? QStringLiteral("没有解析到有效点迹")
            : job->errors().first();
        m_status->setText(QStringLiteral("导入失败：%1").arg(summary));
        if (!app::testMode())
            QMessageBox::warning(this, QStringLiteral("导入失败"),
                                 job->errors().join(QLatin1Char('\n')));
        job->deleteLater();
        return;
    }

    m_data = job->dataset();
    m_canvas->setDataset(&m_data);
    m_canvas->setHighlightedBatch(-1);
    m_files = m_pendingFiles;

    // 持久化文件清单
    QJsonArray arr;
    for (const QString &f : qAsConst(m_files))
        arr.append(f);
    app::setValue(QStringLiteral("point3d.files"),
                  QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));

    QString msg = QStringLiteral("已导入 %1 帧，%2 个点，耗时 %3 ms")
                      .arg(m_data.frames.size())
                      .arg([this]() {
                          int n = 0;
                          for (const DetPointFrame &f : qAsConst(m_data.frames))
                              n += f.points.size();
                          return n;
                      }())
                      .arg(job->elapsedMs());
    if (!job->errors().isEmpty())
        msg += QStringLiteral("；%1 个文件失败").arg(job->errors().size());
    m_status->setText(msg);
    job->deleteLater();
    emit dataChanged();
}

void Point3dWindow::onViewModeChanged()
{
    const int mode = m_viewCombo->currentData().toInt();
    m_canvas->setViewMode(Point3dViewMode(mode));
    if (m_restoring)
        return;
    const char *key = mode == int(Point3dViewMode::RAF) ? "raf"
                    : mode == int(Point3dViewMode::VAF) ? "vaf" : "rvf";
    app::setValue(QStringLiteral("point3d.view_mode"), QString::fromLatin1(key));
    updateStatus();
}

void Point3dWindow::onWindowLengthChanged()
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

void Point3dWindow::applyWindowLength(int len)
{
    m_canvas->setWindowLength(len);
    app::setValue(QStringLiteral("point3d.window_length"), len);
    updateStatus();
}

void Point3dWindow::setLoadingUi(bool loading, int percent)
{
    m_progress->setVisible(loading);
    m_progress->setValue(qBound(0, percent, 100));
    m_importBtn->setEnabled(!loading);
    if (loading)
        m_status->setText(QStringLiteral("正在解析点迹数据… %1%").arg(qBound(0, percent, 100)));
}

void Point3dWindow::updateStatus()
{
    if (m_job)
        return;
    if (m_data.frames.isEmpty())
        return;                       // 保留导入完成/失败时的提示文字
    m_status->setText(m_canvas->statusText());
}

void Point3dWindow::setHighlightedBatch(int batchNo)
{
    m_canvas->setHighlightedBatch(batchNo);
    updateStatus();
}

int Point3dWindow::highlightedBatch() const
{
    return m_canvas->highlightedBatch();
}

int Point3dWindow::currentFrame() const
{
    return m_canvas->currentFrame();
}

void Point3dWindow::setCurrentFrame(int index)
{
    m_canvas->setCurrentFrame(index);
    updateStatus();
}

void Point3dWindow::cancelImport()
{
    if (m_job)
        m_job->requestCancel();
}

void Point3dWindow::saveSettings()
{
    if (m_restoring)
        return;
    double yaw = 0, pitch = 0, zoom = 0;
    m_canvas->camera(&yaw, &pitch, &zoom);
    app::setValue(QStringLiteral("point3d.camera_yaw"), yaw);
    app::setValue(QStringLiteral("point3d.camera_pitch"), pitch);
    app::setValue(QStringLiteral("point3d.camera_zoom"), zoom);
    app::setValue(QStringLiteral("point3d.window_geometry"), saveGeometry());
}

void Point3dWindow::closeEvent(QCloseEvent *e)
{
    if (m_job) {
        m_job->requestCancel();
        m_job->wait();
        m_job->deleteLater();
        m_job = nullptr;
    }
    saveSettings();
    app::flushSettings();
    emit closed();
    QWidget::closeEvent(e);
}
