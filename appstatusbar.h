#ifndef APPSTATUSBAR_H
#define APPSTATUSBAR_H

#include <QColor>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QAbstractButton;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QWheelEvent;
class ReplayController;

namespace statusbar_detail {
class IconButton;
class SeekBar;
class SpeedSelect;
class Spinner;
class ErrorTag;
class SourceButton;
}

// 状态栏用到的主题色，取自 RadarView src/themes/{dark,light,highContrast}.ts
struct StatusTheme {
    QString id;      // dark / light / hc
    QColor bg;       // --statusbar-bg
    QColor fg;       // --statusbar-fg
    QColor border;   // --statusbar-border
    QColor track;    // --bg-tertiary（进度条底色）
    QColor accent;   // --accent-primary（导入进度）
    QColor error;    // --error
};

// 回放区间文字：同一天省略日期（"09:30:00 / 10:30:00"），跨天带月日（"04-27 09:30:00 / 04-28 10:30:00"）
QString formatReplayRange(qint64 currentMs, qint64 endMs);

// 底部状态栏，逐项对应 RadarView src/components/layout/StatusBar.vue：
//   左：播放/暂停 | 进度条 | 当前时间 / 结束时间 | 倍速（有数据时才显示时间和倍速）
//   右：导入进度 | 错误提示 | 高 | 经纬 | FPS | 数据源（展开上方数据源条）| 航迹数 | 主题切换
// 自己绘制背景、作为普通控件放在主窗口底部，不用 QStatusBar（它的 showMessage 会把左侧控件盖住）
class AppStatusBar : public QWidget
{
    Q_OBJECT
public:
    struct SourceItem {
        QString key;       // 点击时回传，由调用方解释
        QString label;
        QColor color;
        int count = 0;
        bool visible = true;
    };

    explicit AppStatusBar(ReplayController *replay, QWidget *parent = nullptr);

    void setViewStatus(double heightKm, double lonDeg, double latDeg, int fps, bool hasCoord = false);
    void setSources(const QVector<SourceItem> &items);
    void setTrackCount(int count);
    void setLoading(bool loading, int percent);
    void setPersisting(bool persisting);        // 后台写数据库中：显示「保存中」
    QString loadingText() const;
    void setError(const QString &message);     // 空串表示清除

    void setTheme(const QString &id);           // 会写入设置 theme
    void cycleTheme();                          // dark -> light -> hc -> dark
    const StatusTheme &theme() const { return m_theme; }

    // 数据源条（横向一排，紧贴状态栏上方）的展开状态：按钮高亮 + 文案提示
    void setSourceStripOpen(bool open);
    bool sourceStripOpen() const { return m_stripOpen; }

    // ---- 自检用 ----
    QString timeText() const;
    QString speedText() const;
    QString heightText() const;
    QString lonLatText() const;
    QString fpsText() const;
    QString trackCountText() const;
    QString errorText() const;
    QStringList sourceTexts() const;            // "ADS-B:2900"，隐藏的加前缀 "off "
    bool isLoadingShown() const;
    bool areReplayControlsShown() const;        // 播放按钮和进度条（任何时候都应显示）
    bool isTimeShown() const;                   // 时间和倍速（有数据时显示）
    bool isCustomSpeedShown() const;
    void chooseCustomSpeed();                   // 模拟在下拉框里选「自定义...」
    void typeCustomSpeed(const QString &text);  // 模拟在输入框里键入并回车
    double seekProgress() const;                // 进度条当前显示的进度 0..1
    void clickSource(int index);
    void clickTheme();
    bool checkLayout(QString *report) const;    // 各项从左到右排列、互不重叠、都在状态栏内

signals:
    void sourceToggled(const QString &key);
    void themeChanged(const QString &id);
    void sourceStripToggleRequested();         // 点「数据源」：展开/收起上方数据源条

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    void syncState();
    void syncTime();
    void syncLoading();
    bool m_isLoading = false, m_persisting = false;
    int m_loadPercent = 0;
    void applyTheme();
    void updateSeekWidth();
    void applyCustomSpeed(const QString &text);
    void setLabelColor(QLabel *label, double opacity);

    ReplayController *m_ctrl;
    StatusTheme m_theme;

    QWidget *m_left = nullptr;
    QWidget *m_rightClip = nullptr;
    QWidget *m_right = nullptr;
    QWidget *m_speedBox = nullptr;
    QWidget *m_loadingBox = nullptr;
    statusbar_detail::IconButton *m_play = nullptr;
    statusbar_detail::SeekBar *m_seek = nullptr;
    QLabel *m_time = nullptr;
    statusbar_detail::SpeedSelect *m_speed = nullptr;
    QLineEdit *m_customSpeed = nullptr;
    bool m_showCustom = false;
    double m_lastSpeed = -1.0;                  // 上次同步时的倍速，用来判断倍速是否变化

    statusbar_detail::Spinner *m_spinner = nullptr;
    QLabel *m_loading = nullptr;
    statusbar_detail::ErrorTag *m_error = nullptr;
    QLabel *m_height = nullptr;
    QLabel *m_lonLat = nullptr;
    QLabel *m_fps = nullptr;
    QPushButton *m_sourceBtn = nullptr;         // 「数据源」入口，替代原来一排按钮
    QVector<SourceItem> m_items;                // 最近一次 setSources 的内容
    bool m_stripOpen = false;
    void updateSourceBtnStyle();
    QLabel *m_count = nullptr;
    statusbar_detail::IconButton *m_themeBtn = nullptr;
};

// 数据源条：横向一排 chip，紧贴状态栏上方；点 chip 切换该数据源/文件的显隐。
// 由主窗口放进底部竖排布局（状态栏正上方），展开时压缩地图高度而不是盖住地图。
class SourceStrip : public QWidget
{
    Q_OBJECT
public:
    explicit SourceStrip(QWidget *parent = nullptr);

    void setItems(const QVector<AppStatusBar::SourceItem> &items);
    void applyTheme(const StatusTheme &theme);

signals:
    void sourceToggled(const QString &key);

protected:
    void paintEvent(QPaintEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;

private:
    QScrollArea *m_scroll = nullptr;
    QWidget *m_row = nullptr;
    QHBoxLayout *m_rowLayout = nullptr;
    QVector<QAbstractButton *> m_chips;
    QVector<AppStatusBar::SourceItem> m_items;   // 最近一次 setItems，字号变化时按它重建
    StatusTheme m_theme;
};

#endif // APPSTATUSBAR_H
