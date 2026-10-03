#include "managepanel.h"
#include "apppaths.h"
#include "lucide.h"
#include "theme.h"
#include "track.h"
#include "uiwidgets.h"

#include <QComboBox>
#include <QDateTime>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QDialog>
#include <QVBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QTableView>

namespace {

// 自动换行的横向布局（CSS flex-wrap）
class FlowLayout : public QLayout
{
public:
    FlowLayout(QWidget *parent, int hSpacing, int vSpacing) : QLayout(parent), m_h(hSpacing), m_v(vSpacing)
    {
        setContentsMargins(0, 0, 0, 0);
    }
    ~FlowLayout() override
    {
        while (QLayoutItem *it = takeAt(0))
            delete it;
    }
    void addItem(QLayoutItem *item) override { m_items.append(item); }
    int count() const override { return m_items.size(); }
    QLayoutItem *itemAt(int i) const override { return m_items.value(i); }
    QLayoutItem *takeAt(int i) override { return i >= 0 && i < m_items.size() ? m_items.takeAt(i) : nullptr; }
    Qt::Orientations expandingDirections() const override { return Qt::Orientations(); }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return doLayout(QRect(0, 0, w, 0), true); }
    void setGeometry(const QRect &r) override
    {
        QLayout::setGeometry(r);
        doLayout(r, false);
    }
    QSize sizeHint() const override { return minimumSize(); }
    QSize minimumSize() const override
    {
        QSize s;
        for (QLayoutItem *it : m_items)
            s = s.expandedTo(it->minimumSize());
        const QMargins m = contentsMargins();
        return s + QSize(m.left() + m.right(), m.top() + m.bottom());
    }
    // 最后一项（stretch 标记）靠右
    bool lastRight = false;

private:
    int doLayout(const QRect &rect, bool testOnly) const
    {
        const QMargins m = contentsMargins();
        const QRect r = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
        int x = r.x(), y = r.y(), lineH = 0;
        for (int i = 0; i < m_items.size(); ++i) {
            QLayoutItem *it = m_items[i];
            if (it->widget() && it->widget()->isHidden())
                continue;
            const QSize sz = it->sizeHint();
            int next = x + sz.width() + m_h;
            if (next - m_h > r.right() + 1 && lineH > 0) {
                x = r.x();
                y += lineH + m_v;
                next = x + sz.width() + m_h;
                lineH = 0;
            }
            int px = x;
            if (lastRight && i == m_items.size() - 1 && x + sz.width() <= r.right() + 1)
                px = r.right() + 1 - sz.width();
            if (!testOnly)
                it->setGeometry(QRect(QPoint(px, y), sz));
            x = next;
            lineH = qMax(lineH, sz.height());
        }
        return y + lineH - rect.y() + m.bottom();
    }
    QList<QLayoutItem *> m_items;
    int m_h, m_v;
};

QString sourceLabelShort(TrackSource s)   // 表格「来源」列 / 统计栏
{
    switch (s) {
    case TrackSource::Adsb: return QStringLiteral("ADS-B");
    case TrackSource::Radar: return QStringLiteral("雷达");
    case TrackSource::RadarRaw: return QStringLiteral("原始");
    }
    return QString();
}

QString filterSourceLabel(TrackSource s)   // 来源下拉（ManageFilterBar DB_SOURCE_LABELS）
{
    switch (s) {
    case TrackSource::Adsb: return QStringLiteral("ADS-B");
    case TrackSource::Radar: return QStringLiteral("Radar");
    case TrackSource::RadarRaw: return QStringLiteral("Raw");
    }
    return QString();
}

QLabel *iconLabel(LucideIcon icon, int px, const char *colorVar, QWidget *parent)
{
    QLabel *l = new QLabel(parent);
    l->setPixmap(lucidePixmap(icon, px, themeColor(colorVar), parent->devicePixelRatioF()));
    return l;
}

QString msToMonthDay(qint64 ms)
{
    return QDateTime::fromMSecsSinceEpoch(ms, Qt::OffsetFromUTC, 8 * 3600).toString(QStringLiteral("MM-dd"));
}

const int kRowH = 18;

} // namespace

// ---------------------------------------------------------------
//  ManageState
// ---------------------------------------------------------------
ManageState::ManageState(TrackStore *store, QObject *parent) : QObject(parent), m_store(store)
{
}

void ManageState::ensureLoaded(const QStringList &keys)
{
    QStringList missing;
    for (const QString &k : keys)
        if (m_store->indexOf(k) < 0)
            missing << k;
    if (missing.isEmpty() || !trackdb::isOpen())
        return;
    const QVector<Track> loaded = trackdb::loadTracks(missing);
    if (loaded.isEmpty())
        return;
    emit aboutToChangeStore();
    m_store->addTracks(loaded);
    emit storeChanged();
}

void ManageState::toggleVisible(const QString &key)
{
    if (m_visible.contains(key)) {
        m_visible.remove(key);
    } else {
        ensureLoaded(QStringList() << key);
        m_visible.insert(key);
    }
    saveSettings();
    emit displayChanged();
}

void ManageState::showAll(const QStringList &keys)
{
    ensureLoaded(keys);
    for (const QString &k : keys)
        m_visible.insert(k);
    saveSettings();
    emit displayChanged();
}

void ManageState::clearVisible()
{
    if (m_visible.isEmpty())
        return;
    m_visible.clear();
    saveSettings();
    emit displayChanged();
}

void ManageState::softDelete(const QStringList &keys, const QString &label)
{
    if (keys.isEmpty())
        return;
    trackdb::setDeleted(keys, true);
    UndoEntry e;
    e.label = label;
    e.keys = keys;
    for (const QString &k : keys) {
        if (m_visible.remove(k))
            e.wasVisible.insert(k);
    }
    emit aboutToChangeStore();
    m_store->removeTracks(QSet<QString>(keys.begin(), keys.end()));
    emit storeChanged();
    m_undo.append(e);
    saveSettings();
    emit undoChanged();
    emit displayChanged();
    emit dataChanged();
}

// 与 RadarView undoDelete 一致：恢复后这些航迹加入地图可见集合
bool ManageState::undoDelete()
{
    if (m_undo.isEmpty())
        return false;
    const UndoEntry e = m_undo.takeLast();
    trackdb::setDeleted(e.keys, false);
    ensureLoaded(e.keys);
    for (const QString &k : e.keys)
        m_visible.insert(k);
    saveSettings();
    emit undoChanged();
    emit displayChanged();
    emit dataChanged();
    return true;
}

void ManageState::addHighlight(const QString &icao)
{
    m_highlight.insert(icao);
    emit highlightChanged();
}

// 加载某批次：把该批次软删除的航迹也一并读回内存（RadarView handleLoadBatch 走 load_batch_tracks）
bool ManageState::loadBatch(qint64 batchId, QString *error)
{
    const QVector<BatchInfo> bs = trackdb::batches();
    bool found = false;
    for (const BatchInfo &b : bs) {
        if (b.id != batchId)
            continue;
        found = true;
        // 数据库读回该批次全部航迹（含软删除的），加进内存并加入可见集合
        const QVector<Track> loaded = trackdb::loadBatchTracks(batchId);
        if (!loaded.isEmpty()) {
            QStringList keys;
            for (const Track &t : loaded) {
                m_visible.insert(t.key());
                keys << t.key();
            }
            emit aboutToChangeStore();
            m_store->addTracks(loaded);
            emit storeChanged();
        }
        if (error)
            *error = QString();
        return true;
    }
    if (error)
        *error = QStringLiteral("批次不存在或为空");
    return found;
}

// 批次级硬删除：删数据库 + 从内存 / 可见集合移除该批次航迹（不可撤销）
bool ManageState::hardDeleteBatch(qint64 batchId, QString *error)
{
    const QStringList batchKeys = trackdb::batchKeys(batchId);
    if (!trackdb::deleteBatch(batchId, error))
        return false;
    for (const QString &k : batchKeys)
        m_visible.remove(k);
    if (!batchKeys.isEmpty()) {
        emit aboutToChangeStore();
        m_store->removeTracks(QSet<QString>(batchKeys.begin(), batchKeys.end()));
        emit storeChanged();
    }
    saveSettings();
    emit displayChanged();
    emit dataChanged();
    return true;
}

void ManageState::clearHighlights()
{
    m_highlight.clear();
    emit highlightChanged();
}
void ManageState::saveSettings()
{
    QSettings &s = app::settings();
    s.setValue(QStringLiteral("manage.filter"), QString::fromUtf8(QJsonDocument(filter.filterJson()).toJson(QJsonDocument::Compact)));
    s.setValue(QStringLiteral("manage.sort"), QString::fromUtf8(QJsonDocument(filter.sortJson()).toJson(QJsonDocument::Compact)));
    s.setValue(QStringLiteral("manage.page_size"), pageSize);
    QStringList vis = m_visible.values();
    vis.sort();
    s.setValue(QStringLiteral("manage.visible_keys"), QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(vis)).toJson(QJsonDocument::Compact)));
}

void ManageState::loadSettings()
{
    QSettings &s = app::settings();
    filter.loadJson(QJsonDocument::fromJson(s.value(QStringLiteral("manage.filter")).toString().toUtf8()).object(),
                    QJsonDocument::fromJson(s.value(QStringLiteral("manage.sort")).toString().toUtf8()).object());
    const int ps = s.value(QStringLiteral("manage.page_size"), 100).toInt();
    pageSize = ps == 20 || ps == 50 ? ps : 100;
    m_visible.clear();
    for (const QJsonValue &v : QJsonDocument::fromJson(s.value(QStringLiteral("manage.visible_keys")).toString().toUtf8()).array())
        if (m_store->indexOf(v.toString()) >= 0)
            m_visible.insert(v.toString());
}

// ---------------------------------------------------------------
//  ManageModel
// ---------------------------------------------------------------
ManageModel::ManageModel(ManageState *state, QObject *parent) : QAbstractTableModel(parent), m_state(state)
{
}

void ManageModel::setRows(const QVector<ManageRow> &rows)
{
    beginResetModel();
    m_rows = rows;
    endResetModel();
}

int ManageModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

int ManageModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QString ManageModel::cellText(const ManageRow &r, int column)
{
    auto dash = [](const QString &s) { return s.isEmpty() ? QStringLiteral("—") : s; };
    switch (column) {
    case Src: return sourceLabelShort(r.source);
    case Icao: return r.icao;
    case Flt: return dash(r.flightNo);
    case Reg: return dash(r.registration);
    case Type: return dash(r.aircraftType);
    case Aln: return dash(r.airline);
    case Route:
        if (r.origin.isEmpty() && r.destination.isEmpty())
            return QStringLiteral("—");
        return (r.origin.isEmpty() ? QStringLiteral("???") : r.origin) + QStringLiteral(" → ")
             + (r.destination.isEmpty() ? QStringLiteral("???") : r.destination);
    case Pts: return QLocale(QLocale::English).toString(r.pointCount);
    case Time: {
        if (r.minTs.isEmpty() && r.maxTs.isEmpty())
            return QStringLiteral("—");
        auto f = [](const QString &t) { return t.size() >= 16 ? t.mid(5, 11) : t; };
        return f(r.minTs) + QStringLiteral(" ~ ") + f(r.maxTs);
    }
    default: return QString();
    }
}

QString ManageModel::sortKey(int column)
{
    switch (column) {
    case Icao: return QStringLiteral("icao_address");
    case Flt: return QStringLiteral("flight_no");
    case Reg: return QStringLiteral("registration");
    case Type: return QStringLiteral("aircraft_type");
    case Aln: return QStringLiteral("airline");
    case Pts: return QStringLiteral("point_count");
    case Time: return QStringLiteral("min_timestamp");
    default: return QString();
    }
}

QVariant ManageModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return QVariant();
    const ManageRow &r = m_rows[index.row()];
    if (role == Qt::DisplayRole)
        return cellText(r, index.column());
    if (role == Qt::ToolTipRole) {
        if (index.column() == Eye)
            return QStringLiteral("切换地图可见性");
        if (index.column() == Act)
            return QStringLiteral("删除此航迹");
    }
    return QVariant();
}

QVariant ManageModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal)
        return QVariant();
    static const char *const titles[] = { "", "来源", "ICAO", "航班号", "注册号", "机型", "航司", "起降地", "点数", "时间", "操作" };
    static const char *const tips[] = { "", "", "按 ICAO 地址排序", "按航班号排序", "按注册号排序", "按机型排序",
                                        "按航空公司排序", "", "按航迹点数排序", "按时间排序", "" };
    if (role == Qt::DisplayRole) {
        QString t = QString::fromUtf8(titles[section]);
        const QString key = sortKey(section);
        if (!key.isEmpty()) {
            t += QLatin1Char(' ');
            if (m_state->filter.sortBy == key)
                t += m_state->filter.sortDesc ? QStringLiteral("▼") : QStringLiteral("▲");
        }
        return t;
    }
    if (role == Qt::ToolTipRole && tips[section][0])
        return QString::fromUtf8(tips[section]);
    if (role == Qt::DecorationRole && section == Eye)
        return lucidePixmap(LucideIcon::Eye, 12, themeColor("text-tertiary"), 2.0);
    if (role == Qt::TextAlignmentRole)
        return int(section == Eye || section == Act ? Qt::AlignCenter : Qt::AlignLeft | Qt::AlignVCenter);
    return QVariant();
}

// ---------------------------------------------------------------
//  ManageDelegate
// ---------------------------------------------------------------
ManageDelegate::ManageDelegate(ManageState *state, QObject *parent) : QStyledItemDelegate(parent), m_state(state)
{
}

QSize ManageDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const
{
    return QSize(40, kRowH);
}

void ManageDelegate::paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const
{
    const ManageModel *model = static_cast<const ManageModel *>(index.model());
    const ManageRow &r = model->rows()[index.row()];
    const bool even = index.row() % 2 == 1;   // CSS nth-child(even)：第 2、4… 行
    const bool hover = index.row() == hoverRow;
    const bool visible = m_state->isVisible(r.trackKey());
    const bool hl = m_state->highlighted().contains(r.icao);
    const QRect rc = opt.rect;

    // 行底色，层叠顺序同 ManageDataTable.vue 的 CSS
    QColor bg(Qt::transparent);
    if (even) bg = themeColor("bg-tertiary");
    if (hover) bg = themeColor("button-hover");
    if (visible) bg = QColor(0, 122, 204, 31);
    if (hl) bg = QColor(255, 200, 0, 38);
    if (visible && even) bg = QColor(0, 122, 204, 41);
    p->save();
    if (bg.alpha() > 0)
        p->fillRect(rc, bg);
    if (hl) {
        p->setPen(QColor(255, 200, 0, 102));
        p->drawLine(rc.topLeft(), rc.topRight());
        p->drawLine(rc.bottomLeft(), rc.bottomRight());
    }

    const int col = index.column();
    if (col == ManageModel::Eye) {
        const QRectF box(rc.center().x() - 6.5 + 0.5, rc.center().y() - 6.5 + 0.5, 13, 13);
        if (visible) {
            drawLucide(*p, LucideIcon::Eye, box, themeColor("accent-primary"));
        } else {
            QColor c = themeColor("text-tertiary");
            c.setAlphaF(hover ? 1.0 : 0.4);
            drawLucide(*p, LucideIcon::Circle, box, c);
        }
        p->restore();
        return;
    }
    if (col == ManageModel::Act) {
        if (hover)
            drawLucide(*p, LucideIcon::Trash2, QRectF(rc.center().x() - 6 + 0.5, rc.center().y() - 6 + 0.5, 12, 12),
                       themeColor("text-tertiary"));
        p->restore();
        return;
    }
    QRect tr = rc.adjusted(4, 0, -4, 0);
    QFont f = opt.font;
    f.setPixelSize(ui::px(0.714));
    if (col == ManageModel::Icao)
        f.setFamilies(QStringList() << QStringLiteral("Consolas") << QStringLiteral("DejaVu Sans Mono")
                                    << QStringLiteral("Liberation Mono"));
    if (col == ManageModel::Flt)
        f.setWeight(QFont::DemiBold);
    p->setFont(f);
    if (col == ManageModel::Src) {
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(Qt::NoPen);
        p->setBrush(rowColor ? rowColor(r) : trackSourceColor(r.source));
        p->drawEllipse(QRectF(tr.left(), rc.center().y() - 3 + 0.5, 6, 6));
        tr.setLeft(tr.left() + 10);
    }
    p->setPen(themeColor("text-primary"));
    const QString text = QFontMetrics(f).elidedText(ManageModel::cellText(r, col), Qt::ElideRight, tr.width());
    p->drawText(tr, Qt::AlignVCenter | (col == ManageModel::Pts ? Qt::AlignRight : Qt::AlignLeft), text);
    p->restore();
}

// ---------------------------------------------------------------
//  ManagePanel
// ---------------------------------------------------------------
ManagePanel::ManagePanel(ManageState *state, QWidget *parent) : QWidget(parent), m_state(state)
{
    setObjectName(QStringLiteral("managePanel"));
    buildUi();
    m_searchTimer.setSingleShot(true);
    m_searchTimer.setInterval(300);
    connect(&m_searchTimer, &QTimer::timeout, this, [this]() {
        m_state->page = 1;
        m_state->saveSettings();
        reloadPage();
    });
    connect(m_state, &ManageState::dataChanged, this, &ManagePanel::refresh);
    connect(m_state, &ManageState::displayChanged, this, [this]() {
        m_table->viewport()->update();
        updateToolbar();
    });
    connect(m_state, &ManageState::highlightChanged, this, [this]() {
        m_table->viewport()->update();
        updateToolbar();
        // 滚动到第一个高亮行
        for (int i = 0; i < m_model->rows().size(); ++i)
            if (m_state->highlighted().contains(m_model->rows()[i].icao)) {
                m_table->scrollTo(m_model->index(i, 0), QAbstractItemView::PositionAtCenter);
                break;
            }
    });
    connect(Theme::instance(), &Theme::changed, this, [this]() {
        reloadStats();
        m_table->viewport()->update();
    });
}

void ManagePanel::buildUi()
{
    QVBoxLayout *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    // ---- 统计栏 ----
    QWidget *statsBar = new QWidget(this);
    statsBar->setObjectName(QStringLiteral("statsBar"));
    FlowLayout *sl = new FlowLayout(statsBar, 2, 2);
    sl->setContentsMargins(8, 4, 8, 4);
    sl->lastRight = true;
    m_stats = new QLabel(statsBar);   // 由 reloadStats 填充子控件；这里只作容器标记
    m_stats->hide();
    v->addWidget(statsBar);

    // ---- 筛选栏 ----
    QWidget *fb = new QWidget(this);
    fb->setObjectName(QStringLiteral("filterBar"));
    QVBoxLayout *fl = new QVBoxLayout(fb);
    fl->setContentsMargins(8, 4, 8, 4);
    fl->setSpacing(3);
    QHBoxLayout *r1 = new QHBoxLayout;
    r1->setSpacing(4);
    m_search = new QLineEdit(fb);
    m_search->setObjectName(QStringLiteral("search"));
    m_search->setPlaceholderText(QStringLiteral("搜索 ICAO / 航班号 / 注册号 / 机型 / 航司 / 起降地..."));
    m_search->setToolTip(QStringLiteral("输入关键字模糊搜索航迹"));
    m_clearSearch = new ui::IconButton(LucideIcon::X, 13, "text-tertiary", "text-primary", fb);
    m_clearSearch->setToolTip(QStringLiteral("清除搜索内容"));
    m_clearSearch->setFixedSize(20, 20);
    m_source = new QComboBox(fb);
    m_source->setToolTip(QStringLiteral("按数据来源或文件筛选"));
    m_source->setFixedWidth(90);
    r1->addWidget(m_search, 1);
    r1->addWidget(m_clearSearch);
    r1->addWidget(m_source);
    fl->addLayout(r1);
    QHBoxLayout *r2 = new QHBoxLayout;
    r2->setSpacing(4);
    m_airline = new QComboBox(fb);
    m_type = new QComboBox(fb);
    m_batch = new QComboBox(fb);
    for (QComboBox *c : { m_airline, m_type, m_batch }) {
        c->setMinimumWidth(0);
        c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        c->setMinimumContentsLength(3);
        r2->addWidget(c, 1);
    }
    fl->addLayout(r2);
    QHBoxLayout *r3 = new QHBoxLayout;
    r3->setSpacing(4);
    QLabel *ptsLabel = new QLabel(QStringLiteral("点数:"), fb);
    ptsLabel->setObjectName(QStringLiteral("flabel"));
    m_minPts = new QLineEdit(fb);
    m_maxPts = new QLineEdit(fb);
    m_minPts->setPlaceholderText(QStringLiteral("≥"));
    m_maxPts->setPlaceholderText(QStringLiteral("≤"));
    m_minPts->setToolTip(QStringLiteral("最小航迹点数"));
    m_maxPts->setToolTip(QStringLiteral("最大航迹点数"));
    for (QLineEdit *e : { m_minPts, m_maxPts }) {
        e->setObjectName(QStringLiteral("fnum"));
        e->setFixedWidth(52);
        e->setValidator(new QIntValidator(0, 100000000, e));
    }
    QLabel *sep = new QLabel(QStringLiteral("~"), fb);
    sep->setObjectName(QStringLiteral("flabel"));
    auto pbtn = [fb](LucideIcon icon, const QString &text, const QString &tip, const QString &name) {
        QPushButton *b = new QPushButton(text, fb);
        b->setObjectName(name);
        b->setToolTip(tip);
        b->setCursor(Qt::PointingHandCursor);
        b->setProperty("_icon", int(icon));
        return b;
    };
    QPushButton *p24 = pbtn(LucideIcon::Clock, QStringLiteral("24h内"), QStringLiteral("筛选最近24小时内的航迹"), QStringLiteral("pbtn"));
    QPushButton *p100 = pbtn(LucideIcon::ChartColumn, QStringLiteral("≥100点"), QStringLiteral("筛选点数不少于100的航迹"), QStringLiteral("pbtn"));
    QPushButton *pReset = pbtn(LucideIcon::RotateCcw, QStringLiteral("重置全部"), QStringLiteral("清除所有筛选条件"), QStringLiteral("pbtnReset"));
    r3->addWidget(ptsLabel);
    r3->addWidget(m_minPts);
    r3->addWidget(sep);
    r3->addWidget(m_maxPts);
    r3->addStretch(1);
    r3->addWidget(p24);
    r3->addWidget(p100);
    r3->addWidget(pReset);
    fl->addLayout(r3);
    v->addWidget(fb);

    // ---- 工具栏 ----
    QWidget *tb = new QWidget(this);
    tb->setObjectName(QStringLiteral("toolbar"));
    FlowLayout *tl = new FlowLayout(tb, 3, 2);
    tl->setContentsMargins(8, 2, 8, 2);
    m_toolbarInfo = new QLabel(tb);
    m_toolbarInfo->setObjectName(QStringLiteral("tbInfo"));
    tl->addWidget(m_toolbarInfo);
    auto tbBtn = [tb, tl](LucideIcon icon, bool withIcon, const QString &text, const QString &tip, const QString &name) {
        QPushButton *b = new QPushButton(text, tb);
        b->setObjectName(name);
        b->setToolTip(tip);
        b->setCursor(Qt::PointingHandCursor);
        if (withIcon)
            b->setProperty("_icon", int(icon));
        tl->addWidget(b);
        return b;
    };
    m_clearHighlight = tbBtn(LucideIcon::Sparkles, true, QStringLiteral("取消高亮"), QStringLiteral("取消所有高亮标记"), QStringLiteral("tbHighlight"));
    QPushButton *bShowAll = tbBtn(LucideIcon::Eye, true, QStringLiteral("本页全显"), QStringLiteral("将本页所有航迹设为地图可见"), QStringLiteral("tbBtn"));
    QPushButton *bClearMap = tbBtn(LucideIcon::Eye, false, QStringLiteral("清空地图"), QString(), QStringLiteral("tbBtn"));
    QPushButton *bDelVis = tbBtn(LucideIcon::Trash2, true, QStringLiteral("删可见"), QStringLiteral("软删除所有地图可见航迹"), QStringLiteral("tbDanger"));
    QPushButton *bExport = tbBtn(LucideIcon::Download, true, QStringLiteral("导出"), QStringLiteral("导出当前筛选结果为 JSON 文件"), QStringLiteral("tbBtn"));
    QPushButton *bRefresh = tbBtn(LucideIcon::RefreshCw, true, QStringLiteral("刷新"), QStringLiteral("刷新数据库统计和元数据"), QStringLiteral("tbBtn"));
    QPushButton *bBatches = tbBtn(LucideIcon::Package, true, QStringLiteral("批量管理"), QStringLiteral("批量数据管理：加载 / 从数据库永久删除批次"), QStringLiteral("tbBtn"));
    connect(bBatches, &QPushButton::clicked, this, &ManagePanel::clickBatchManage);
    v->addWidget(tb);

    // ---- 表格 ----
    m_model = new ManageModel(m_state, this);
    m_delegate = new ManageDelegate(m_state, this);
    m_delegate->rowColor = [this](const ManageRow &r) { return rowColor ? rowColor(r) : trackSourceColor(r.source); };
    m_table = new QTableView(this);
    m_table->setObjectName(QStringLiteral("dataTable"));
    m_table->setModel(m_model);
    m_table->setItemDelegate(m_delegate);
    m_table->setShowGrid(false);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setFocusPolicy(Qt::NoFocus);
    m_table->setMouseTracking(true);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setDefaultSectionSize(kRowH);
    m_table->verticalHeader()->setMinimumSectionSize(kRowH);
    m_table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    QHeaderView *hh = m_table->horizontalHeader();
    hh->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    hh->setHighlightSections(false);
    hh->setSectionsClickable(true);
    hh->setMinimumSectionSize(20);
    hh->setFixedHeight(20);
    const int widths[] = { 28, 52, 74, 68, 64, 52, 48, 100, 56, 150, 28 };
    for (int i = 0; i < ManageModel::ColumnCount; ++i) {
        hh->resizeSection(i, widths[i]);
        hh->setSectionResizeMode(i, i == ManageModel::Route ? QHeaderView::Stretch : QHeaderView::Interactive);
    }
    m_table->viewport()->installEventFilter(this);
    m_empty = new QLabel(this);
    m_empty->setObjectName(QStringLiteral("empty"));
    m_empty->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_empty->setContentsMargins(8, 24, 8, 24);
    v->addWidget(m_table, 1);
    v->addWidget(m_empty, 1);

    // ---- 分页 ----
    QWidget *pg = new QWidget(this);
    pg->setObjectName(QStringLiteral("pagination"));
    QHBoxLayout *pl = new QHBoxLayout(pg);
    pl->setContentsMargins(8, 3, 8, 3);
    pl->setSpacing(4);
    m_pageInfo = new QLabel(pg);
    m_pageInfo->setObjectName(QStringLiteral("pageInfo"));
    m_prev = new QPushButton(QStringLiteral("< 上一页"), pg);
    m_next = new QPushButton(QStringLiteral("下一页 >"), pg);
    m_prev->setToolTip(QStringLiteral("跳转到上一页"));
    m_next->setToolTip(QStringLiteral("跳转到下一页"));
    for (QPushButton *b : { m_prev, m_next }) {
        b->setObjectName(QStringLiteral("pageBtn"));
        b->setCursor(Qt::PointingHandCursor);
    }
    m_pageSize = new QComboBox(pg);
    m_pageSize->setObjectName(QStringLiteral("psSelect"));
    m_pageSize->setToolTip(QStringLiteral("选择每页显示条数"));
    for (int n : { 20, 50, 100 })
        m_pageSize->addItem(QStringLiteral("%1条/页").arg(n), n);
    pl->addWidget(m_pageInfo);
    pl->addStretch(1);
    pl->addWidget(m_prev);
    pl->addWidget(m_next);
    pl->addWidget(m_pageSize);
    v->addWidget(pg);

    // ---- 按钮图标：随主题重绘 ----
    auto applyIcons = [this]() {
        for (QPushButton *b : findChildren<QPushButton *>()) {
            const QVariant ic = b->property("_icon");
            if (!ic.isValid())
                continue;
            const char *var = b->objectName() == QLatin1String("tbDanger") ? "error"
                            : b->objectName() == QLatin1String("tbHighlight") ? nullptr
                            : b->objectName() == QLatin1String("pbtnReset") ? nullptr : "text-secondary";
            const QColor c = var ? themeColor(var)
                           : b->objectName() == QLatin1String("tbHighlight") ? QColor(0xe8, 0xa0, 0x20) : QColor(0xe8, 0xa0, 0x40);
            b->setIcon(lucideQIcon(LucideIcon(ic.toInt()), 11, c));
            b->setIconSize(QSize(11, 11));
        }
    };
    applyIcons();
    connect(Theme::instance(), &Theme::changed, this, applyIcons);

    // ---- 样式（逐项取自 ManagePanel.vue / ManageFilterBar.vue / ManageDataTable.vue / ManagePagination.vue）----
    setThemedStyle(this, QStringLiteral(
        "#managePanel { background: transparent; }"
        "QLabel { background: transparent; }"
        "#statsBar, #filterBar, #toolbar { border-bottom: 1px solid var(--border-secondary); }"
        "#statsBar QLabel { color: var(--text-secondary); font-size: 9px; }"
        "#statsBar QLabel[strong=\"true\"] { color: var(--text-primary); font-weight: bold; }"
        "#statsBar QLabel[sep=\"true\"] { color: var(--text-tertiary); }"
        "#btnMini { font-size: 8px; padding: 0px 4px; border: 1px solid var(--border-secondary); border-radius: 2px;"
        " background: var(--button-bg); color: var(--text-secondary); }"
        "#btnMini:hover { background: var(--button-hover); color: var(--text-primary); }"
        "#search { padding: 3px 6px; font-size: 10px; background: var(--bg-secondary); border: 1px solid var(--border-secondary);"
        " border-radius: 3px; color: var(--text-primary); }"
        "#search:focus { border-color: var(--accent-primary); }"
        "QComboBox { padding: 2px 3px; font-size: 9px; background: var(--bg-secondary); border: 1px solid var(--border-secondary);"
        " border-radius: 3px; color: var(--text-primary); }"
        "QComboBox::drop-down { border: none; width: 12px; }"
        "QComboBox QAbstractItemView { background: var(--bg-secondary); color: var(--text-primary);"
        " selection-background-color: var(--dropdown-hover); font-size: 11px; }"
        "#flabel { font-size: 9px; color: var(--text-tertiary); }"
        "#fnum { padding: 2px 3px; font-size: 9px; background: var(--bg-secondary); border: 1px solid var(--border-secondary);"
        " border-radius: 3px; color: var(--text-primary); }"
        "#pbtn, #pbtnReset { font-size: 8px; padding: 1px 5px; border: 1px solid var(--border-secondary); border-radius: 3px;"
        " background: var(--button-bg); color: var(--text-secondary); }"
        "#pbtn:hover, #pbtnReset:hover { background: var(--button-hover); color: var(--text-primary); }"
        "#pbtnReset { color: #e8a040; }"
        "#tbInfo { font-size: 9px; color: var(--text-secondary); }"
        "#tbBtn, #tbDanger, #tbHighlight { font-size: 9px; padding: 1px 5px; border: 1px solid var(--border-secondary);"
        " border-radius: 3px; background: var(--button-bg); color: var(--text-secondary); }"
        "#tbBtn:hover { background: var(--button-hover); color: var(--text-primary); }"
        "#tbDanger { color: var(--error); }"
        "#tbDanger:hover { background: #26dc3232; }"
        "#tbHighlight { color: #e8a020; border-color: #66e8a020; }"
        "#tbHighlight:hover { background: #1ae8a020; color: #f0c040; }"
        "#dataTable { background: transparent; border: none; color: var(--text-primary); }"
        "#dataTable QHeaderView { background: var(--bg-secondary); }"
        "#dataTable QHeaderView::section { background: var(--bg-secondary); color: var(--text-tertiary); padding: 3px 4px;"
        " border: none; border-bottom: 1px solid var(--border-secondary); font-size: 9px; font-weight: 600; }"
        "#dataTable QHeaderView::section:hover { color: var(--text-primary); }"
        "#empty { color: var(--text-tertiary); font-size: 11px; }"
        "#pagination { border-top: 1px solid var(--border-secondary); }"
        "#pageInfo { font-size: 9px; color: var(--text-secondary); }"
        "#pageBtn { padding: 2px 6px; font-size: 9px; border: 1px solid var(--border-secondary); border-radius: 3px;"
        " background: var(--button-bg); color: var(--text-secondary); }"
        "#pageBtn:hover { background: var(--button-hover); color: var(--text-primary); }"
        "#pageBtn:disabled { color: #66808080; }"
        "#psSelect { padding: 1px 3px; }"
        "QScrollBar:vertical { background: var(--scrollbar-bg); width: 10px; margin: 0; }"
        "QScrollBar::handle:vertical { background: var(--scrollbar-thumb); min-height: 20px; border-radius: 3px; }"
        "QScrollBar:horizontal { background: var(--scrollbar-bg); height: 10px; margin: 0; }"
        "QScrollBar::handle:horizontal { background: var(--scrollbar-thumb); min-width: 20px; border-radius: 3px; }"
        "QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }"
        "QScrollBar::add-page, QScrollBar::sub-page { background: none; }")
        .replace(QStringLiteral("font-size"), QStringLiteral("font-family: ") + ui::uiFamilies() + QStringLiteral("; font-size")));

    // ---- 交互 ----
    connect(m_search, &QLineEdit::textEdited, this, [this](const QString &t) {
        m_state->filter.searchText = t;
        m_clearSearch->setVisible(!t.isEmpty());
        m_searchTimer.start();
    });
    connect(m_clearSearch, &QToolButton::clicked, this, [this]() { setSearchText(QString()); });
    connect(m_source, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
        const QString val = m_source->itemData(i).toString();
        const int sep = val.indexOf(QLatin1String("::"));
        if (sep > 0) {
            m_state->filter.source = val.left(sep);
            m_state->filter.batchId = val.mid(sep + 2).toLongLong();
        } else {
            m_state->filter.source = val;
            m_state->filter.batchId = -1;
        }
        m_state->page = 1;
        m_state->saveSettings();
        reloadOptions();
        reloadPage();
    });
    auto comboFilter = [this](QComboBox *c, std::function<void(const QVariant &)> set) {
        connect(c, QOverload<int>::of(&QComboBox::activated), this, [this, c, set](int i) {
            set(c->itemData(i));
            m_state->page = 1;
            m_state->saveSettings();
            reloadOptions();
            reloadPage();
        });
    };
    comboFilter(m_airline, [this](const QVariant &v) { m_state->filter.airline = v.toString(); });
    comboFilter(m_type, [this](const QVariant &v) { m_state->filter.aircraftType = v.toString(); });
    comboFilter(m_batch, [this](const QVariant &v) { m_state->filter.batchId = v.isValid() ? v.toLongLong() : -1; });
    auto numFilter = [this](QLineEdit *e, int ManageFilter::*field) {
        connect(e, &QLineEdit::editingFinished, this, [this, e, field]() {
            const int v = e->text().isEmpty() ? -1 : e->text().toInt();
            if (m_state->filter.*field == v)
                return;
            m_state->filter.*field = v;
            m_state->page = 1;
            m_state->saveSettings();
            reloadPage();
        });
    };
    numFilter(m_minPts, &ManageFilter::minPoints);
    numFilter(m_maxPts, &ManageFilter::maxPoints);
    connect(p24, &QPushButton::clicked, this, [this]() {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        m_state->filter.minTimeMs = now - 24LL * 3600 * 1000;
        m_state->filter.maxTimeMs = now;
        m_state->page = 1;
        m_state->saveSettings();
        reloadPage();
    });
    connect(p100, &QPushButton::clicked, this, [this]() {
        m_state->filter.minPoints = 100;
        m_state->page = 1;
        m_state->saveSettings();
        applyFilterToUi();
        reloadPage();
    });
    connect(pReset, &QPushButton::clicked, this, &ManagePanel::clickResetFilters);
    connect(m_clearHighlight, &QPushButton::clicked, m_state, &ManageState::clearHighlights);
    connect(bShowAll, &QPushButton::clicked, this, &ManagePanel::clickShowAllOnPage);
    connect(bClearMap, &QPushButton::clicked, this, &ManagePanel::clickClearMap);
    connect(bDelVis, &QPushButton::clicked, this, &ManagePanel::clickDeleteVisible);
    connect(bExport, &QPushButton::clicked, this, &ManagePanel::onExport);
    connect(bRefresh, &QPushButton::clicked, this, &ManagePanel::refresh);
    connect(hh, &QHeaderView::sectionClicked, this, &ManagePanel::clickSort);
    connect(m_table, &QTableView::clicked, this, &ManagePanel::onTableClicked);
    connect(m_table, &QTableView::customContextMenuRequested, this, &ManagePanel::onContextMenu);
    connect(m_prev, &QPushButton::clicked, this, [this]() {
        if (m_state->page > 1) {
            --m_state->page;
            reloadPage();
        }
    });
    connect(m_next, &QPushButton::clicked, this, [this]() {
        ++m_state->page;
        reloadPage();
    });
    connect(m_pageSize, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
        setPageSize(m_pageSize->itemData(i).toInt());
    });
}

bool ManagePanel::eventFilter(QObject *obj, QEvent *e)
{
    if (obj == m_table->viewport()) {
        if (e->type() == QEvent::MouseMove) {
            const int row = m_table->indexAt(static_cast<QMouseEvent *>(e)->pos()).row();
            if (row != m_delegate->hoverRow) {
                m_delegate->hoverRow = row;
                m_table->viewport()->update();
            }
        } else if (e->type() == QEvent::Leave) {
            m_delegate->hoverRow = -1;
            m_table->viewport()->update();
        }
    }
    return QWidget::eventFilter(obj, e);
}

void ManagePanel::refresh()
{
    reloadStats();
    reloadOptions();
    reloadPage();
}

// 把筛选状态同步到控件（启动恢复、重置、外部设置搜索词时）
void ManagePanel::applyFilterToUi()
{
    m_syncing = true;
    if (m_search->text() != m_state->filter.searchText)
        m_search->setText(m_state->filter.searchText);
    m_clearSearch->setVisible(!m_state->filter.searchText.isEmpty());
    m_minPts->setText(m_state->filter.minPoints >= 0 ? QString::number(m_state->filter.minPoints) : QString());
    m_maxPts->setText(m_state->filter.maxPoints >= 0 ? QString::number(m_state->filter.maxPoints) : QString());
    m_pageSize->setCurrentIndex(m_pageSize->findData(m_state->pageSize));
    m_syncing = false;
}

void ManagePanel::reloadOptions()
{
    const ManageFilter &f = m_state->filter;
    const QVector<BatchInfo> batches = trackdb::batches();
    // 来源下拉：数据源只有一个批次时直接列出，多个批次时加「（全部）」并逐个列出文件
    m_source->clear();
    m_source->addItem(QStringLiteral("全部来源"), QString());
    QVector<TrackSource> order;
    for (const BatchInfo &b : batches)
        if (!order.contains(b.source))
            order << b.source;
    for (TrackSource s : order) {
        QVector<BatchInfo> list;
        for (const BatchInfo &b : batches)
            if (b.source == s)
                list << b;
        if (list.size() == 1) {
            m_source->addItem(filterSourceLabel(s), trackSourceKey(s));
        } else {
            m_source->addItem(filterSourceLabel(s) + QStringLiteral("（全部）"), trackSourceKey(s));
            for (const BatchInfo &b : list)
                m_source->addItem(QStringLiteral("   · ") + b.fileName, trackSourceKey(s) + QStringLiteral("::") + QString::number(b.id));
        }
    }
    QString cur = f.source;
    if (!f.source.isEmpty() && f.batchId >= 0)
        cur = f.source + QStringLiteral("::") + QString::number(f.batchId);
    int ci = m_source->findData(cur);
    if (ci < 0)
        ci = m_source->findData(f.source);
    m_source->setCurrentIndex(qMax(0, ci));

    auto fillCombo = [](QComboBox *c, const QString &allText, const QStringList &vals, const QString &cur) {
        c->clear();
        c->addItem(allText, QString());
        for (const QString &v : vals)
            c->addItem(v, v);
        c->setCurrentIndex(qMax(0, c->findData(cur)));
    };
    fillCombo(m_airline, QStringLiteral("全部航司"), trackdb::distinctValues(QStringLiteral("airline"), f.source), f.airline);
    fillCombo(m_type, QStringLiteral("全部机型"), trackdb::distinctValues(QStringLiteral("aircraft_type"), f.source), f.aircraftType);
    m_batch->clear();
    m_batch->addItem(QStringLiteral("全部批次"), QVariant());
    TrackSource fs;
    const bool bySrc = trackSourceFromKey(f.source, &fs);
    for (const BatchInfo &b : batches)
        if (!bySrc || b.source == fs)
            m_batch->addItem(b.fileName, b.id);
    m_batch->setCurrentIndex(f.batchId >= 0 ? qMax(0, m_batch->findData(f.batchId)) : 0);
    applyFilterToUi();
}

void ManagePanel::reloadPage()
{
    int total = 0;
    QVector<ManageRow> rows = trackdb::query(m_state->filter, m_state->pageSize,
                                             (m_state->page - 1) * m_state->pageSize, &total);
    m_total = total;
    m_model->setRows(rows);
    m_delegate->hoverRow = -1;
    const int pages = qMax(1, (total + m_state->pageSize - 1) / m_state->pageSize);
    m_pageInfo->setText(QStringLiteral("第 %1 / %2 页 (共 %3 条)").arg(m_state->page).arg(pages)
                            .arg(QLocale(QLocale::English).toString(total)));
    m_prev->setEnabled(m_state->page > 1);
    m_next->setEnabled(m_state->page < pages);
    const bool empty = rows.isEmpty();
    m_table->setVisible(!empty);
    m_empty->setVisible(empty);
    m_empty->setText(total == 0 ? QStringLiteral("暂无航迹数据，请先导入文件") : QStringLiteral("未找到匹配的航迹，请调整筛选条件"));
    updateToolbar();
}

void ManagePanel::reloadStats()
{
    QWidget *bar = findChild<QWidget *>(QStringLiteral("statsBar"));
    FlowLayout *fl = static_cast<FlowLayout *>(bar->layout());
    while (QLayoutItem *it = fl->takeAt(0)) {
        if (it->widget() && it->widget() != m_stats)
            it->widget()->deleteLater();
        delete it;
    }
    auto addText = [&](const QString &t, const char *prop = nullptr) {
        QLabel *l = new QLabel(t, bar);
        if (prop)
            l->setProperty(prop, true);
        fl->addWidget(l);
        return l;
    };
    // 「图标 文字 <b>数字</b> 文字」作为一个不拆行的整体
    auto addItem = [&](LucideIcon icon, int iconPx, bool withIcon, const QString &pre, const QString &strong, const QString &post) {
        QWidget *w = new QWidget(bar);
        QHBoxLayout *h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(3);
        if (withIcon)
            h->addWidget(iconLabel(icon, iconPx, "text-secondary", w));
        if (!pre.isEmpty())
            h->addWidget(new QLabel(pre, w));
        if (!strong.isEmpty()) {
            QLabel *s = new QLabel(strong, w);
            s->setProperty("strong", true);
            h->addWidget(s);
        }
        if (!post.isEmpty())
            h->addWidget(new QLabel(post, w));
        fl->addWidget(w);
    };
    auto sep = [&]() { addText(QStringLiteral("|"), "sep")->setContentsMargins(3, 0, 3, 0); };
    QStringList plain;
    if (!trackdb::isOpen()) {
        addText(QStringLiteral("暂无数据，请先导入航迹"));
        plain << QStringLiteral("暂无数据，请先导入航迹");
    } else {
        const ManageStats s = trackdb::stats();
        m_lastStats = s;
        addItem(LucideIcon::ChartColumn, 12, true, QStringLiteral("总计"), QString::number(s.totalTracks), QStringLiteral("条"));
        plain << QStringLiteral("总计 %1 条").arg(s.totalTracks);
        sep();
        QStringList srcs;
        for (TrackSource src : { TrackSource::Adsb, TrackSource::Radar, TrackSource::RadarRaw }) {
            const QString name = trackSourceName(src);
            if (!s.bySource.contains(name))
                continue;
            addItem(LucideIcon::Dot, 0, false, sourceLabelShort(src), QString::number(s.bySource.value(name)), QString());
            srcs << sourceLabelShort(src) + QLatin1Char(' ') + QString::number(s.bySource.value(name));
        }
        plain << srcs.join(QLatin1Char(' '));
        sep();
        addItem(LucideIcon::Hash, 11, true, QString::number(s.uniqueIcao) + QStringLiteral(" ICAO"), QString(), QString());
        plain << QStringLiteral("%1 ICAO").arg(s.uniqueIcao);
        sep();
        const QString range = s.timeMin > 0 && s.timeMax > 0 ? msToMonthDay(s.timeMin) + QStringLiteral(" ~ ") + msToMonthDay(s.timeMax)
                                                             : QStringLiteral("—");
        addItem(LucideIcon::Clock, 11, true, range, QString(), QString());
        plain << range;
        sep();
        addItem(LucideIcon::Package, 11, true, QString::number(s.totalBatches) + QStringLiteral(" 批次"), QString(), QString());
        plain << QStringLiteral("%1 批次").arg(s.totalBatches);
    }
    m_stats->setText(plain.join(QStringLiteral(" | ")));
    // 右侧：地图可见 N 条 + 清空
    QWidget *right = new QWidget(bar);
    right->setObjectName(QStringLiteral("statRight"));
    QHBoxLayout *rh = new QHBoxLayout(right);
    rh->setContentsMargins(0, 0, 0, 0);
    rh->setSpacing(4);
    QWidget *vis = new QWidget(right);
    QHBoxLayout *vh = new QHBoxLayout(vis);
    vh->setContentsMargins(0, 0, 0, 0);
    vh->setSpacing(3);
    vh->addWidget(new QLabel(QStringLiteral("地图可见"), vis));
    m_visibleInfo = new QLabel(vis);
    m_visibleInfo->setProperty("strong", true);
    vh->addWidget(m_visibleInfo);
    vh->addWidget(new QLabel(QStringLiteral("条"), vis));
    rh->addWidget(vis);
    m_clearVisibleMini = new QPushButton(QStringLiteral("清空"), right);
    m_clearVisibleMini->setObjectName(QStringLiteral("btnMini"));
    m_clearVisibleMini->setToolTip(QStringLiteral("清空地图可见航迹集合"));
    m_clearVisibleMini->setCursor(Qt::PointingHandCursor);
    connect(m_clearVisibleMini, &QPushButton::clicked, this, &ManagePanel::clickClearMap);
    rh->addWidget(m_clearVisibleMini);
    fl->addWidget(right);
    // 重新套一次样式，新建的标签才会按属性上色
    setThemedStyle(this, property("_themeQss").toString());
    updateToolbar();
}

void ManagePanel::updateToolbar()
{
    int onPage = 0;
    for (const ManageRow &r : m_model->rows())
        if (m_state->isVisible(r.trackKey()))
            ++onPage;
    m_toolbarInfo->setText(QStringLiteral("匹配 %1 条 · 本页 %2 条 · 地图可见 %3 条").arg(m_total).arg(m_model->rowCount()).arg(onPage));
    m_clearHighlight->setVisible(!m_state->highlighted().isEmpty());
    if (m_visibleInfo) {
        m_visibleInfo->setText(QString::number(m_state->visibleKeys().size()));
        m_clearVisibleMini->setVisible(!m_state->visibleKeys().isEmpty());
    }
    if (QWidget *tb = findChild<QWidget *>(QStringLiteral("toolbar")))
        tb->updateGeometry();
}

void ManagePanel::onTableClicked(const QModelIndex &idx)
{
    if (!idx.isValid())
        return;
    if (idx.column() == ManageModel::Eye)
        clickEye(idx.row());
    else if (idx.column() == ManageModel::Act)
        deleteRow(idx.row());
}

void ManagePanel::onContextMenu(const QPoint &pos)
{
    const QModelIndex idx = m_table->indexAt(pos);
    if (!idx.isValid())
        return;
    const int row = idx.row();
    ui::SmallMenu menu(this);
    menu.addItem(LucideIcon::ClipboardList, QStringLiteral("查看点迹数据"), [this, row]() { viewRowPoints(row); })
        ->setToolTip(QStringLiteral("查看此航迹的详细点数据"));
    menu.addItem(LucideIcon::Trash2, QStringLiteral("删除该航迹"), [this, row]() { deleteRow(row); }, true)
        ->setToolTip(QStringLiteral("删除此航迹"));
    menu.exec(m_table->viewport()->mapToGlobal(pos));
}

void ManagePanel::confirmDeleteRows(const QVector<ManageRow> &rows)
{
    if (rows.isEmpty())
        return;
    QStringList keys;
    for (const ManageRow &r : rows)
        keys << r.trackKey();
    if (rows.size() == 1) {
        const ManageRow &r = rows.first();
        if (!ui::confirm(this, QStringLiteral("确定隐藏航迹 %1 (%2)？\n数据保留在数据库中，可通过撤销恢复。")
                                   .arg(r.icao, trackSourceName(r.source)),
                         QStringLiteral("隐藏航迹"), true))
            return;
        m_state->softDelete(keys, QStringLiteral("航迹 %1 (%2)").arg(r.icao, trackSourceName(r.source)));
    } else {
        if (!ui::confirm(this, QStringLiteral("确定隐藏 %1 条航迹？\n数据保留在数据库中，可通过撤销恢复。").arg(rows.size()),
                         QStringLiteral("批量隐藏航迹"), true))
            return;
        m_state->softDelete(keys, QStringLiteral("%1 条航迹").arg(rows.size()));
    }
}

int ManagePanel::rowCount() const { return m_model->rowCount(); }
const QVector<ManageRow> &ManagePanel::rows() const { return m_model->rows(); }
QString ManagePanel::statsText() const { return m_stats->text(); }
QString ManagePanel::toolbarText() const { return m_toolbarInfo->text(); }
QString ManagePanel::pageText() const { return m_pageInfo->text(); }
QString ManagePanel::emptyText() const { return m_empty->isVisible() ? m_empty->text() : QString(); }

void ManagePanel::setSearchText(const QString &text)
{
    m_state->filter.searchText = text;
    m_state->page = 1;
    m_searchTimer.stop();
    m_state->saveSettings();
    applyFilterToUi();
    reloadPage();
}

void ManagePanel::clickSort(int column)
{
    const QString key = ManageModel::sortKey(column);
    if (key.isEmpty())
        return;
    if (m_state->filter.sortBy == key) {
        m_state->filter.sortDesc = !m_state->filter.sortDesc;
    } else {
        m_state->filter.sortBy = key;
        m_state->filter.sortDesc = false;
    }
    m_state->page = 1;
    m_state->saveSettings();
    reloadPage();
    m_model->headerDataChanged(Qt::Horizontal, 0, ManageModel::ColumnCount - 1);
}

void ManagePanel::clickEye(int row)
{
    if (row >= 0 && row < m_model->rows().size())
        m_state->toggleVisible(m_model->rows()[row].trackKey());
}

void ManagePanel::deleteRow(int row)
{
    if (row >= 0 && row < m_model->rows().size())
        confirmDeleteRows(QVector<ManageRow>() << m_model->rows()[row]);
}

void ManagePanel::viewRowPoints(int row)
{
    if (row < 0 || row >= m_model->rows().size())
        return;
    const QString key = m_model->rows()[row].trackKey();
    m_state->ensureLoaded(QStringList() << key);
    emit viewPointsRequested(key);
}

void ManagePanel::clickBatchManage()
{
    const QVector<BatchInfo> batches = trackdb::batches();
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("批量数据管理"));
    QVBoxLayout *dv = new QVBoxLayout(&dlg);
    dv->setContentsMargins(12, 12, 12, 12);
    dv->setSpacing(8);
    QLabel *hint = new QLabel(QStringLiteral("点击「加载」把该批次航迹读回地图；「删除」从数据库永久移除（不可撤销）。"), &dlg);
    hint->setObjectName(QStringLiteral("batchHint"));
    dv->addWidget(hint);

    if (batches.isEmpty())
        dv->addWidget(new QLabel(QStringLiteral("暂无已保存的数据"), &dlg));

    for (const BatchInfo &b : batches) {
        QWidget *row = new QWidget(&dlg);
        row->setObjectName(QStringLiteral("batchRow"));
        QHBoxLayout *rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(6);
        QLabel *info = new QLabel(QStringLiteral("%1 · %2 · %3 条 · %4")
                                     .arg(filterSourceLabel(b.source), b.fileName,
                                          QString::number(b.trackCount), b.importedAt), row);
        info->setObjectName(QStringLiteral("batchInfo"));
        rl->addWidget(info, 1);
        QPushButton *load = new QPushButton(QStringLiteral("加载"), row);
        load->setCursor(Qt::PointingHandCursor);
        load->setObjectName(QStringLiteral("batchBtn"));
        QPushButton *del = new QPushButton(QStringLiteral("删除"), row);
        del->setCursor(Qt::PointingHandCursor);
        del->setObjectName(QStringLiteral("batchDel"));
        rl->addWidget(load);
        rl->addWidget(del);
        dv->addWidget(row);

        connect(load, &QPushButton::clicked, this, [this, id = b.id]() {
            QString err;
            if (!m_state->loadBatch(id, &err) && !err.isEmpty())
                ui::confirm(this, err, QStringLiteral("加载批次失败"));
            m_state->markStale();
            refresh();
        });
        connect(del, &QPushButton::clicked, this, [this, id = b.id, name = b.fileName]() {
            if (!ui::confirm(this,
                            QStringLiteral("确定从数据库中删除 \"%1\"？\n\n该操作不可撤销。").arg(name),
                            QStringLiteral("删除批次"), true))
                return;
            QString err;
            if (m_state->hardDeleteBatch(id, &err)) {
                m_state->markStale();
                refresh();
            } else if (!err.isEmpty()) {
                ui::confirm(this, QStringLiteral("删除失败：%1").arg(err), QStringLiteral("删除批次"));
            }
        });
    }

    setThemedStyle(&dlg, QStringLiteral(
        "#batchHint { color: var(--text-secondary); font-size: 11px; }"
        "#batchRow { background: var(--bg-secondary); border: 1px solid var(--border-secondary); border-radius: 3px; padding: 4px 6px; }"
        "#batchInfo { color: var(--text-primary); font-size: 11px; }"
        "#batchBtn { font-size: 10px; padding: 2px 8px; background: var(--button-bg); color: var(--button-fg);"
        " border: 1px solid var(--border-primary); border-radius: 3px; }"
        "#batchBtn:hover { background: var(--button-hover); }"
        "#batchDel { font-size: 10px; padding: 2px 8px; color: var(--error); background: transparent;"
        " border: 1px solid var(--error); border-radius: 3px; }"
        "#batchDel:hover { background: var(--error-bg); }"
        "QDialog { background: var(--bg-primary); }"));
    dlg.exec();
}

void ManagePanel::clickShowAllOnPage()
{
    QStringList keys;
    for (const ManageRow &r : m_model->rows())
        keys << r.trackKey();
    m_state->showAll(keys);
}

void ManagePanel::clickDeleteVisible()
{
    QVector<ManageRow> rows;
    for (const ManageRow &r : m_model->rows())
        if (m_state->isVisible(r.trackKey()))
            rows << r;
    confirmDeleteRows(rows);
}

void ManagePanel::clickClearMap()
{
    m_state->clearVisible();
}

void ManagePanel::clickResetFilters()
{
    m_state->filter.resetAll();
    m_state->page = 1;
    m_state->saveSettings();
    reloadOptions();
    reloadPage();
    m_model->headerDataChanged(Qt::Horizontal, 0, ManageModel::ColumnCount - 1);
}

void ManagePanel::setPageSize(int n)
{
    m_state->pageSize = n;
    m_state->page = 1;
    m_state->saveSettings();
    applyFilterToUi();
    reloadPage();
}

// 导出本页地图可见的航迹（RadarView exportVisibleTracks，字段名同后端 Track 序列化）
bool ManagePanel::exportVisible(const QString &path, QString *error)
{
    QStringList keys;
    for (const ManageRow &r : m_model->rows())
        if (m_state->isVisible(r.trackKey()))
            keys << r.trackKey();
    if (keys.isEmpty())
        return false;
    QJsonArray arr;
    for (const Track &t : trackdb::loadTracks(keys)) {
        QJsonObject o;
        o.insert(QStringLiteral("icao_address"), t.id);
        o.insert(QStringLiteral("source"), trackSourceName(t.source));
        o.insert(QStringLiteral("file_name"), t.fileName);
        o.insert(QStringLiteral("flight_no"), t.flightNo);
        o.insert(QStringLiteral("icao_flight_no"), t.icaoFlightNo);
        o.insert(QStringLiteral("aircraft_type"), t.aircraftType);
        o.insert(QStringLiteral("registration"), t.registration);
        o.insert(QStringLiteral("airline"), t.airline);
        o.insert(QStringLiteral("origin"), t.origin);
        o.insert(QStringLiteral("destination"), t.destination);
        QJsonArray pos;
        for (const TrackPoint &p : t.points) {
            QJsonObject po;
            po.insert(QStringLiteral("timestamp"), trackdb::beijingText(p.t));
            po.insert(QStringLiteral("latitude"), p.lat);
            po.insert(QStringLiteral("longitude"), p.lon);
            po.insert(QStringLiteral("altitude"), double(p.alt));
            po.insert(QStringLiteral("heading"), double(p.heading));
            po.insert(QStringLiteral("ground_speed"), double(p.speed));
            po.insert(QStringLiteral("vertical_rate"), double(p.vrate));
            pos.append(po);
        }
        o.insert(QStringLiteral("positions"), pos);
        arr.append(o);
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
    return true;
}

void ManagePanel::onExport()
{
    bool any = false;
    for (const ManageRow &r : m_model->rows())
        any = any || m_state->isVisible(r.trackKey());
    if (!any)
        return;
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出"), QStringLiteral("radarview_export.json"),
                                                      QStringLiteral("JSON (*.json);;All Files (*)"));
    if (!path.isEmpty())
        exportVisible(path);
}
