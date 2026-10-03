#ifndef MANAGEPANEL_H
#define MANAGEPANEL_H

#include <QAbstractTableModel>
#include <QSet>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QWidget>
#include <functional>

#include "trackdb.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QAbstractButton;
class QTableView;
class TrackStore;

// 航迹管理状态，对应 RadarView useTrackManagement.ts + useUndoStack.ts + useTrackHighlight.ts：
//   地图可见集合（非空时地图只显示集合内的航迹）、软删除与撤销栈、高亮、筛选/排序/分页
class ManageState : public QObject
{
    Q_OBJECT
public:
    struct UndoEntry {
        QString label;
        QStringList keys;
        QSet<QString> wasVisible;   // 删除前在可见集合里的，撤销时放回去
    };

    explicit ManageState(TrackStore *store, QObject *parent = nullptr);

    const QSet<QString> &visibleKeys() const { return m_visible; }
    bool isVisible(const QString &key) const { return m_visible.contains(key); }
    void toggleVisible(const QString &key);
    void showAll(const QStringList &keys);
    void clearVisible();

    // 软删除：数据保留在数据库，从内存和地图移除，压入撤销栈
    void softDelete(const QStringList &keys, const QString &label);
    bool undoDelete();
    const QVector<UndoEntry> &undoStack() const { return m_undo; }

    const QSet<QString> &highlighted() const { return m_highlight; }
    void addHighlight(const QString &icao);
    void clearHighlights();

    ManageFilter filter;
    int page = 1;
    int pageSize = 100;
    void saveSettings();
    void loadSettings();
    // 数据库内容变化（导入入库完成、删除、撤销）
    void markStale() { emit dataChanged(); }
    // 确保这些航迹已加载进内存（从数据库读）
    void ensureLoaded(const QStringList &keys);

signals:
    void aboutToChangeStore();
    void storeChanged();
    void displayChanged();       // 可见集合变化
    void dataChanged();          // 需要重新查询数据库
    void undoChanged();
    void highlightChanged();

private:
    TrackStore *m_store;
    QSet<QString> m_visible;
    QSet<QString> m_highlight;
    QVector<UndoEntry> m_undo;
};

class ManageModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column { Eye, Src, Icao, Flt, Reg, Type, Aln, Route, Pts, Time, Act, ColumnCount };
    ManageModel(ManageState *state, QObject *parent = nullptr);
    void setRows(const QVector<ManageRow> &rows);
    const QVector<ManageRow> &rows() const { return m_rows; }
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation o, int role = Qt::DisplayRole) const override;
    static QString cellText(const ManageRow &r, int column);
    static QString sortKey(int column);       // 可排序列的数据库列名，不可排序为空

private:
    ManageState *m_state;
    QVector<ManageRow> m_rows;
};

// 逐行绘制（ManageDataTable.vue 的行底色、来源圆点、眼睛 / 圆圈、悬停才出现的删除按钮）
class ManageDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit ManageDelegate(ManageState *state, QObject *parent = nullptr);
    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &index) const override;
    int hoverRow = -1;
    std::function<QColor(const ManageRow &)> rowColor;

private:
    ManageState *m_state;
};

class ManagePanel : public QWidget
{
    Q_OBJECT
public:
    ManagePanel(ManageState *state, QWidget *parent = nullptr);

    // 重新查询（统计、下拉选项、当前页）
    void refresh();
    std::function<QColor(const ManageRow &)> rowColor;   // 来源圆点颜色（文件颜色）

    // ---- 自检用 ----
    int rowCount() const;
    int totalCount() const { return m_total; }
    const QVector<ManageRow> &rows() const;
    QString statsText() const;
    QString toolbarText() const;
    QString pageText() const;
    QString emptyText() const;
    void setSearchText(const QString &text);       // 立即应用（跳过 300 ms 防抖）
    void clickSort(int column);
    void clickEye(int row);
    void deleteRow(int row);                       // 直接走删除流程（确认框由 ui::setAutoConfirm 控制）
    void viewRowPoints(int row);
    void clickShowAllOnPage();
    void clickDeleteVisible();
    void clickClearMap();
    void clickResetFilters();
    void setPageSize(int n);
    bool exportVisible(const QString &path, QString *error = nullptr);

signals:
    void viewPointsRequested(const QString &trackKey);

protected:
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    void buildUi();
    void applyFilterToUi();
    void reloadOptions();
    void reloadPage();
    void reloadStats();
    void updateToolbar();
    void onTableClicked(const QModelIndex &idx);
    void onContextMenu(const QPoint &pos);
    void confirmDeleteRows(const QVector<ManageRow> &rows);
    void onExport();

    ManageState *m_state;
    ManageModel *m_model = nullptr;
    ManageDelegate *m_delegate = nullptr;
    QTableView *m_table = nullptr;
    QLabel *m_stats = nullptr, *m_visibleInfo = nullptr, *m_toolbarInfo = nullptr, *m_pageInfo = nullptr;
    QLabel *m_empty = nullptr;
    QPushButton *m_clearVisibleMini = nullptr, *m_clearHighlight = nullptr;
    QPushButton *m_prev = nullptr, *m_next = nullptr;
    QLineEdit *m_search = nullptr, *m_minPts = nullptr, *m_maxPts = nullptr;
    QAbstractButton *m_clearSearch = nullptr;
    QComboBox *m_source = nullptr, *m_airline = nullptr, *m_type = nullptr, *m_batch = nullptr, *m_pageSize = nullptr;
    QTimer m_searchTimer;
    int m_total = 0;
    bool m_syncing = false;
    ManageStats m_lastStats;
};

#endif // MANAGEPANEL_H
