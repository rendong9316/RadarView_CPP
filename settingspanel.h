#ifndef SETTINGSPANEL_H
#define SETTINGSPANEL_H

#include <QWidget>
#include <functional>

class GlobeWidget;
class QLabel;
class QSlider;
class QVBoxLayout;
class TrackStore;
class ManageState;

// 设置面板（RadarView SettingsPanel.vue）：线条颜色 / 线宽 / 圆球直径 / 点迹大小 / 旗标大小 / 字号大小 / 工具。
// 各项保存在 QSettings display.*，构造时读回并立即应用到地图。
class SettingsPanel : public QWidget
{
    Q_OBJECT
public:
    struct Host {
        TrackStore *store = nullptr;
        GlobeWidget *globe = nullptr;
        ManageState *manage = nullptr;
    };
    explicit SettingsPanel(const Host &host, QWidget *parent = nullptr);

    void refresh();              // 刷新批次 / 航迹数

    // ---- 自检用 ----
    int fontValue() const;
    void setFontValue(int px);

signals:
    void toggleLabelsRequested();
    void resetViewRequested();
    void clearDisplayRequested();
    void openManageRequested();

private:
    QVBoxLayout *addGroup(QVBoxLayout *main, int icon, const QString &title, const QString &tip);
    QSlider *addSlider(QVBoxLayout *body, const QString &label, const QColor &color, double minV, double maxV,
                       double step, double value, const QString &unit, std::function<void(double)> onValue);

    Host m_host;
    QSlider *m_font = nullptr;
    QLabel *m_batchLabel = nullptr;
};

#endif // SETTINGSPANEL_H
