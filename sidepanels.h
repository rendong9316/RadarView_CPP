#ifndef SIDEPANELS_H
#define SIDEPANELS_H

#include <QWidget>

#include "track.h"

class QCheckBox;
class QDateTimeEdit;
class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;
class FlagStore;
class RulerState;

// 筛选条件，对应 RadarView useTrackFilter.ts：时间范围 + 各数据源点数范围（空间套索不做）
struct TrackFilterState {
    bool timeActive = false;
    qint64 timeMin = 0, timeMax = 0;
    struct PointCount {
        bool enabled = false;
        int min = -1, max = -1;   // -1 不限
    };
    PointCount pc[3];             // 按 TrackSource 下标

    bool hasPointCountFilter() const;
    // 是否通过筛选（点数用原始总点数；时间按航迹时间段与窗口有交集，且窗口内至少有一个点）
    bool accepts(const Track &t) const;
    void save() const;
    void load();
};

// 「筛选」面板（TimeFilterPanel.vue，去掉空间套索）
class FilterPanel : public QWidget
{
    Q_OBJECT
public:
    FilterPanel(TrackFilterState *state, QWidget *parent = nullptr);
    void setDataRange(qint64 minMs, qint64 maxMs);   // 0, 0 表示无数据

    // ---- 自检用 ----
    void setInputs(qint64 startMs, qint64 endMs);
    void clickApply();
    void clickClear();
    QString errorText() const;
    QString rangeText() const;
    void setPointCount(TrackSource s, bool enabled, int min, int max);

signals:
    void changed();

private:
    void syncUi();
    TrackFilterState *m_state;
    QLabel *m_timeIndicator = nullptr, *m_pcIndicator = nullptr, *m_range = nullptr, *m_error = nullptr;
    QDateTimeEdit *m_start = nullptr, *m_end = nullptr;
    bool m_startSet = false, m_endSet = false;
    QPushButton *m_apply = nullptr, *m_clear = nullptr;
    QCheckBox *m_pcCheck[3];
    QLineEdit *m_pcMin[3], *m_pcMax[3];
    qint64 m_dataMin = 0, m_dataMax = 0;
};

// 「旗标面板」（FlagPanel.vue）：手动放置旗标、旗标列表、两旗标测距、航线标尺
class FlagPanel : public QWidget
{
    Q_OBJECT
public:
    FlagPanel(FlagStore *flags, RulerState *ruler, QWidget *parent = nullptr);

    // ---- 自检用 ----
    void placeFlag(const QString &lat, const QString &lon);
    QString coordError() const;
    QString geoText() const;
    QString rulerTotalText() const;
    QStringList segmentTexts() const;

private:
    void rebuildFlags();
    void rebuildRuler();
    FlagStore *m_flags;
    RulerState *m_ruler;
    QLineEdit *m_lat = nullptr, *m_lon = nullptr;
    QLabel *m_error = nullptr, *m_geo = nullptr, *m_empty = nullptr;
    QWidget *m_flagList = nullptr, *m_rulerBody = nullptr;
    QPushButton *m_rulerToggle = nullptr;
    QString m_editingId;
    QLabel *m_total = nullptr;
    QStringList m_segTexts;
};

#endif // SIDEPANELS_H
