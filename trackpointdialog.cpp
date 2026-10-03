#include "trackpointdialog.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QTextStream>
#include <QVBoxLayout>

namespace {

const double kFtPerM = 1.0 / 0.3048;

// "YYYY-MM-DD HH:MM:SS"（北京时间，与 RadarView 点迹表一致）；按 UTC 加 8 小时，不依赖本机时区
QString formatPointTime(qint64 ms)
{
    return QDateTime::fromMSecsSinceEpoch(ms + 8LL * 3600 * 1000, Qt::UTC)
        .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

// 序号列显示排序后的行号，其余列按 UserRole 的数值排序
class PointProxy : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    QVariant data(const QModelIndex &index, int role) const override
    {
        if (index.column() == TrackPointModel::Seq && role == Qt::DisplayRole)
            return index.row() + 1;
        return QSortFilterProxyModel::data(index, role);
    }
};

} // namespace

// ---------------------------------------------------------------
//  TrackPointModel
// ---------------------------------------------------------------
TrackPointModel::TrackPointModel(const Track &track, QObject *parent)
    : QAbstractTableModel(parent), m_points(track.points)
{
}

int TrackPointModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_points.size();
}

int TrackPointModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QString TrackPointModel::text(const TrackPoint &p, int row, int column)
{
    switch (column) {
    case Seq: return QString::number(row + 1);
    case Time: return formatPointTime(p.t);
    case Lat: return QString::number(p.lat, 'f', 6);
    case Lon: return QString::number(p.lon, 'f', 6);
    case AltFt: return QString::number(p.alt * kFtPerM, 'f', 0);
    case Heading: return QString::number(p.heading, 'f', 1);
    case Speed: return QString::number(p.speed, 'f', 0);
    case VRate: return QString::number(p.vrate, 'f', 1);
    default: return QString();
    }
}

QVariant TrackPointModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_points.size())
        return QVariant();
    const TrackPoint &p = m_points[index.row()];
    switch (role) {
    case Qt::DisplayRole:
        return text(p, index.row(), index.column());
    case Qt::UserRole:
        switch (index.column()) {
        case Seq: return index.row();
        case Time: return p.t;
        case Lat: return p.lat;
        case Lon: return p.lon;
        case AltFt: return double(p.alt);
        case Heading: return double(p.heading);
        case Speed: return double(p.speed);
        case VRate: return double(p.vrate);
        default: return QVariant();
        }
    case Qt::TextAlignmentRole:
        if (index.column() == Seq)
            return int(Qt::AlignCenter);
        if (index.column() == Time)
            return int(Qt::AlignLeft | Qt::AlignVCenter);
        return int(Qt::AlignRight | Qt::AlignVCenter);
    default:
        return QVariant();
    }
}

QVariant TrackPointModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal || role != Qt::DisplayRole)
        return QAbstractTableModel::headerData(section, o, role);
    static const char *const names[ColumnCount] = {
        "序号", "时间戳", "纬度", "经度", "高度(ft)", "航向(°)", "地速(kn)", "垂直率(ft/min)"
    };
    return section >= 0 && section < ColumnCount ? QString::fromUtf8(names[section]) : QVariant();
}

// ---------------------------------------------------------------
//  TrackPointDialog
// ---------------------------------------------------------------
TrackPointDialog::TrackPointDialog(const Track &track, QWidget *parent)
    : QDialog(parent), m_track(track)
{
    const QString title = track.flightNo.isEmpty() ? track.id : track.flightNo;
    setWindowTitle(tr("点迹数据 - %1").arg(title));
    setAttribute(Qt::WA_DeleteOnClose);

    // 标题行：航班号 / ICAO / 来源色点 / 点数 + 导出按钮
    QLabel *name = new QLabel(QStringLiteral("<b>%1</b>").arg(title.toHtmlEscaped()), this);
    QStringList info;
    if (!track.flightNo.isEmpty() && track.flightNo != track.id)
        info << tr("ICAO: %1").arg(track.id);
    info << tr("%1（%2）").arg(trackSourceName(track.source), track.fileName);
    info << tr("%L1 个点迹").arg(track.points.size());
    QLabel *dot = new QLabel(this);
    dot->setFixedSize(10, 10);
    dot->setStyleSheet(QStringLiteral("background:%1; border-radius:5px;")
                           .arg(trackSourceColor(track.source).name()));
    QLabel *meta = new QLabel(info.join(QStringLiteral("    ")), this);
    QPushButton *exportBtn = new QPushButton(tr("导出 CSV..."), this);
    exportBtn->setToolTip(tr("按当前排序把点迹导出为 CSV 文件"));
    connect(exportBtn, &QPushButton::clicked, this, &TrackPointDialog::onExport);

    QHBoxLayout *head = new QHBoxLayout;
    head->addWidget(name);
    head->addSpacing(8);
    head->addWidget(dot);
    head->addWidget(meta);
    head->addStretch(1);
    head->addWidget(exportBtn);

    m_model = new TrackPointModel(track, this);
    m_proxy = new PointProxy(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setSortRole(Qt::UserRole);

    m_view = new QTableView(this);
    m_view->setModel(m_proxy);
    m_view->setSortingEnabled(true);
    m_view->sortByColumn(TrackPointModel::Time, Qt::AscendingOrder);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setAlternatingRowColors(true);
    m_view->setWordWrap(false);
    m_view->verticalHeader()->hide();
    // 行高固定：几万行时滚动也不卡
    m_view->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_view->verticalHeader()->setDefaultSectionSize(m_view->fontMetrics().height() + 6);
    m_view->horizontalHeader()->setStretchLastSection(true);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSizeF(m_view->font().pointSizeF());
    m_view->setFont(mono);
    m_view->resizeColumnsToContents();

    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    QVBoxLayout *lay = new QVBoxLayout(this);
    lay->addLayout(head);
    lay->addWidget(m_view, 1);
    lay->addWidget(buttons);
    resize(1100, 700);
}

int TrackPointDialog::rowCount() const
{
    return m_proxy->rowCount();
}

bool TrackPointDialog::exportCsv(const QString &path, QString *error) const
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    QTextStream out(&f);
    out.setCodec("UTF-8");
    out.setGenerateByteOrderMark(true);
    QStringList header;
    for (int c = 0; c < TrackPointModel::ColumnCount; ++c)
        header << m_model->headerData(c, Qt::Horizontal).toString();
    out << header.join(QLatin1Char(',')) << "\r\n";
    for (int r = 0; r < m_proxy->rowCount(); ++r) {
        const int src = m_proxy->mapToSource(m_proxy->index(r, 0)).row();
        const TrackPoint &p = m_track.points[src];
        QStringList row;
        for (int c = 0; c < TrackPointModel::ColumnCount; ++c)
            row << TrackPointModel::text(p, r, c);   // 序号按导出顺序
        out << row.join(QLatin1Char(',')) << "\r\n";
    }
    out.flush();
    if (f.error() != QFile::NoError) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}

void TrackPointDialog::onExport()
{
    QString base = m_track.flightNo.isEmpty() ? m_track.id : m_track.flightNo;
    // 文件名里不能有的字符换成下划线（Windows 和 Linux 都安全）
    for (QChar &ch : base)
        if (QStringLiteral("\\/:*?\"<>|").contains(ch))
            ch = QLatin1Char('_');
    const QString path = QFileDialog::getSaveFileName(
        this, tr("导出点迹数据"), base + QStringLiteral("_points.csv"), tr("CSV 文件 (*.csv)"));
    if (path.isEmpty())
        return;
    QString err;
    if (!exportCsv(path, &err))
        QMessageBox::warning(this, tr("导出失败"), err);
}
