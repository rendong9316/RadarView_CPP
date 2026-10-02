#include "tilesource.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QFileInfo>

TileSource::TileSource()
    : m_conn(QStringLiteral("mbtiles_%1").arg(reinterpret_cast<quintptr>(this)))
{
}

TileSource::~TileSource()
{
    close();
}

bool TileSource::open(const QString &path, QString *error)
{
    close();

    QString err;
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        err = QStringLiteral("缺少 SQLite 驱动（sqldrivers/qsqlite.dll）");
    } else if (!QFileInfo(path).isFile()) {
        err = QStringLiteral("文件不存在：%1").arg(path);
    } else {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_conn);
        db.setDatabaseName(path);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (!db.open()) {
            err = db.lastError().text();
        } else {
            QSqlQuery q(db);
            if (!q.exec(QStringLiteral("SELECT MIN(zoom_level), MAX(zoom_level) FROM tiles"))
                    || !q.next() || q.value(0).isNull()) {
                err = QStringLiteral("不是有效的 mbtiles（缺少 tiles 表或没有瓦片）");
            } else {
                m_minZoom = q.value(0).toInt();
                m_maxZoom = q.value(1).toInt();
                m_name = QFileInfo(path).completeBaseName();
                if (q.exec(QStringLiteral("SELECT value FROM metadata WHERE name='name'")) && q.next())
                    m_name = q.value(0).toString();

                m_query = new QSqlQuery(db);
                m_query->prepare(QStringLiteral(
                    "SELECT tile_data FROM tiles WHERE zoom_level=? AND tile_column=? AND tile_row=?"));
                m_path = path;
                m_open = true;
            }
        }
    }

    if (!m_open) {
        close();
        if (error)
            *error = err;
    }
    return m_open;
}

void TileSource::close()
{
    delete m_query;
    m_query = nullptr;
    m_open = false;
    m_minZoom = m_maxZoom = 0;
    m_name.clear();
    m_path.clear();

    if (QSqlDatabase::contains(m_conn)) {
        {
            QSqlDatabase db = QSqlDatabase::database(m_conn, false);
            db.close();
        }
        QSqlDatabase::removeDatabase(m_conn);
    }
}

QImage TileSource::tile(int z, int x, int y)
{
    if (!m_open)
        return QImage();

    // mbtiles 按 TMS 存储：行号从南往北数
    const int tmsY = (1 << z) - 1 - y;
    m_query->bindValue(0, z);
    m_query->bindValue(1, x);
    m_query->bindValue(2, tmsY);
    if (!m_query->exec() || !m_query->next())
        return QImage();

    const QByteArray data = m_query->value(0).toByteArray();
    m_query->finish();

    QImage img;
    img.loadFromData(data);
    return img;
}
