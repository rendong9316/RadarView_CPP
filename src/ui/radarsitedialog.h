#ifndef RADARSITEDIALOG_H
#define RADARSITEDIALOG_H

#include <QColor>
#include <QDialog>
#include <QString>

#include "radarsite.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QListWidget;
class QPushButton;

// 雷达站址与 PPI 极坐标态势网格管理弹窗
class RadarSiteDialog : public QDialog
{
    Q_OBJECT
public:
    explicit RadarSiteDialog(RadarSiteStore *store, QWidget *parent = nullptr);

signals:
    void flyToSiteRequested(double lonDeg, double latDeg, double altKm);

private:
    void rebuildList();
    void loadSelectedIntoForm();
    void saveFormToCurrent();
    void onAddSite();
    void onRemoveSite();
    void onSetActive();
    void onFlyTo();
    void updateColorBtn();

    RadarSiteStore *m_store = nullptr;
    QListWidget *m_list = nullptr;
    QString m_currentId;
    bool m_syncing = false;

    QLineEdit *m_nameEdit = nullptr;
    QCheckBox *m_visibleChk = nullptr;
    QCheckBox *m_hasTxChk = nullptr;
    QDoubleSpinBox *m_txLat = nullptr;
    QDoubleSpinBox *m_txLon = nullptr;
    QCheckBox *m_hasRxChk = nullptr;
    QDoubleSpinBox *m_rxLat = nullptr;
    QDoubleSpinBox *m_rxLon = nullptr;
    QCheckBox *m_hasSectorChk = nullptr;
    QDoubleSpinBox *m_minR = nullptr;
    QDoubleSpinBox *m_maxR = nullptr;
    QDoubleSpinBox *m_startAz = nullptr;
    QDoubleSpinBox *m_endAz = nullptr;
    QDoubleSpinBox *m_beamDir = nullptr;
    QDoubleSpinBox *m_ringStep = nullptr;
    QDoubleSpinBox *m_azStep = nullptr;
    QCheckBox *m_fullRingsChk = nullptr;
    QComboBox *m_anchorCombo = nullptr;
    QPushButton *m_colorBtn = nullptr;
    QColor m_color = QColor(0x38, 0xbd, 0xf8);
};

#endif // RADARSITEDIALOG_H
