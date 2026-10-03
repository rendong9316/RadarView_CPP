#include "matfile.h"

#include <QFile>
#include <cstring>

// Windows 版 Qt 自带 zlib 并从 Qt5Core 导出（z_ 前缀）；Linux 用系统 zlib（.pro 里 -lz）
#ifdef Q_OS_WIN
#include <QtZlib/zlib.h>
#else
#include <zlib.h>
#endif

namespace {

// MAT v5 数据类型
enum {
    miINT8 = 1, miUINT8 = 2, miINT16 = 3, miUINT16 = 4, miINT32 = 5, miUINT32 = 6,
    miSINGLE = 7, miDOUBLE = 9, miINT64 = 12, miUINT64 = 13, miMATRIX = 14,
    miCOMPRESSED = 15, miUTF8 = 16, miUTF16 = 17, miUTF32 = 18
};

// 文件为小端；x86/x86_64 主机直接按内存拷贝读取
quint32 rd32(const char *p)
{
    quint32 v;
    std::memcpy(&v, p, 4);
    return v;
}

int pad8(qint64 n) { return int((n + 7) & ~qint64(7)); }

// 读一个数据元素标签（含 small element 格式），越界返回 false
struct Tag { int type = 0; int size = 0; int data = 0; int next = 0; };

bool readTag(const QByteArray &b, int off, int end, Tag *t)
{
    if (off + 8 > end)
        return false;
    const quint32 w0 = rd32(b.constData() + off);
    if (w0 >> 16) {                      // small element：类型和长度挤在 4 字节里
        t->type = int(w0 & 0xffff);
        t->size = int(w0 >> 16);
        t->data = off + 4;
        t->next = off + 8;
        return t->size <= 4;
    }
    t->type = int(w0);
    const quint32 sz = rd32(b.constData() + off + 4);
    if (sz > quint32(end - off - 8))
        return false;
    t->size = int(sz);
    t->data = off + 8;
    t->next = off + 8 + pad8(sz);
    if (t->next > end)
        t->next = end;
    return true;
}

int typeBytes(int type)
{
    switch (type) {
    case miINT8: case miUINT8: case miUTF8: return 1;
    case miINT16: case miUINT16: case miUTF16: return 2;
    case miINT32: case miUINT32: case miSINGLE: case miUTF32: return 4;
    case miDOUBLE: case miINT64: case miUINT64: return 8;
    default: return 0;
    }
}

// miMATRIX 头部：数组标志、维度、名称
struct Header { int cls = 0; bool complex = false; QVector<int> dims; QString name; int next = 0; };

bool readHeader(const QByteArray &b, int off, int end, Header *h)
{
    Tag t;
    if (!readTag(b, off, end, &t) || t.type != miUINT32 || t.size < 8)
        return false;
    const quint32 flags = rd32(b.constData() + t.data);
    h->cls = int(flags & 0xff);
    h->complex = (flags & 0x800) != 0;

    if (!readTag(b, t.next, end, &t) || t.type != miINT32)
        return false;
    h->dims.clear();
    for (int i = 0; i + 4 <= t.size; i += 4)
        h->dims.append(int(rd32(b.constData() + t.data + i)));

    if (!readTag(b, t.next, end, &t))
        return false;
    h->name = QString::fromLatin1(b.constData() + t.data, t.size);
    h->next = t.next;
    return true;
}

// 解压 zlib 流；maxOut > 0 时只解出前 maxOut 字节（用于读变量名）
bool inflateData(const char *src, qint64 srcLen, QByteArray *out, int maxOut = 0)
{
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    if (inflateInit(&zs) != Z_OK)
        return false;
    qint64 cap = maxOut > 0 ? maxOut : qMax<qint64>(srcLen * 6, 4096);
    out->resize(int(qMin<qint64>(cap, 0x7fff0000)));
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(src));
    zs.avail_in = uInt(srcLen);
    qint64 produced = 0;
    int rc = Z_OK;
    while (true) {
        zs.next_out = reinterpret_cast<Bytef *>(out->data() + produced);
        zs.avail_out = uInt(out->size() - produced);
        rc = inflate(&zs, Z_NO_FLUSH);
        produced = out->size() - zs.avail_out;
        if (rc == Z_STREAM_END || (maxOut > 0 && produced >= maxOut))
            break;
        if (rc != Z_OK && rc != Z_BUF_ERROR)
            break;
        if (zs.avail_out == 0) {
            if (out->size() >= 0x3fff0000)
                break;
            out->resize(out->size() * 2);
        } else if (rc == Z_BUF_ERROR) {
            break;   // 输入耗尽但未结束：数据被截断
        }
    }
    inflateEnd(&zs);
    out->resize(int(produced));
    return rc == Z_STREAM_END || (maxOut > 0 && produced > 0);
}

} // namespace

// ---------------------------------------------------------------
//  MatArray
// ---------------------------------------------------------------
int MatArray::numel() const
{
    if (m_dims.isEmpty())
        return 0;
    qint64 n = 1;
    for (int d : m_dims)
        n *= d;
    return int(qBound<qint64>(0, n, 0x7fffffff));
}

MatArray MatArray::parse(const QSharedPointer<QByteArray> &buf, int off, int len)
{
    MatArray a;
    const QByteArray &b = *buf;
    const int end = off + len;
    if (len <= 0 || end > b.size())
        return a;   // 空矩阵（size 为 0 的 miMATRIX）
    Header h;
    if (!readHeader(b, off, end, &h))
        return a;

    a.m_buf = buf;
    a.m_dims = h.dims;
    a.m_name = h.name;
    a.m_complex = h.complex;
    a.m_class = h.cls;

    Tag t;
    if (a.isNumeric() || h.cls == Char) {
        if (readTag(b, h.next, end, &t)) {
            a.m_dataType = t.type;
            a.m_dataOff = t.data;
            a.m_dataBytes = t.size;
        }
    } else if (h.cls == Struct) {
        if (!readTag(b, h.next, end, &t) || t.type != miINT32)
            return MatArray();
        const int nameLen = int(rd32(b.constData() + t.data));
        if (!readTag(b, t.next, end, &t) || nameLen <= 0)
            return MatArray();
        for (int i = 0; i + nameLen <= t.size; i += nameLen) {
            const char *p = b.constData() + t.data + i;
            a.m_fields << QString::fromLatin1(p, int(qstrnlen(p, uint(nameLen))));
        }
        a.m_childOff = t.next;
        a.m_childEnd = end;
    } else if (h.cls == Cell) {
        a.m_childOff = h.next;
        a.m_childEnd = end;
    }
    // Object / Sparse 等不支持的类型：保留类型信息，取值时返回空
    return a;
}

void MatArray::buildIndex() const
{
    if (m_index)
        return;
    m_index.reset(new QVector<int>);
    const QByteArray &b = *m_buf;
    int off = m_childOff;
    Tag t;
    while (off < m_childEnd && readTag(b, off, m_childEnd, &t)) {
        m_index->append(off);
        off = t.next;
    }
}

double MatArray::scalar(int i, bool *ok) const
{
    if (ok)
        *ok = false;
    const int sz = typeBytes(m_dataType);
    if (!isNumeric() || sz == 0 || i < 0 || qint64(i + 1) * sz > m_dataBytes)
        return 0.0;
    const char *p = m_buf->constData() + m_dataOff + i * sz;
    double v = 0.0;
    switch (m_dataType) {
    case miINT8: v = *reinterpret_cast<const qint8 *>(p); break;
    case miUINT8: v = *reinterpret_cast<const quint8 *>(p); break;
    case miINT16: { qint16 x; std::memcpy(&x, p, 2); v = x; break; }
    case miUINT16: { quint16 x; std::memcpy(&x, p, 2); v = x; break; }
    case miINT32: { qint32 x; std::memcpy(&x, p, 4); v = x; break; }
    case miUINT32: { quint32 x; std::memcpy(&x, p, 4); v = x; break; }
    case miSINGLE: { float x; std::memcpy(&x, p, 4); v = x; break; }
    case miDOUBLE: std::memcpy(&v, p, 8); break;
    case miINT64: { qint64 x; std::memcpy(&x, p, 8); v = double(x); break; }
    case miUINT64: { quint64 x; std::memcpy(&x, p, 8); v = double(x); break; }
    default: return 0.0;
    }
    if (ok)
        *ok = true;
    return v;
}

QString MatArray::toText() const
{
    if (m_class != Char || !m_buf)
        return QString();
    const char *p = m_buf->constData() + m_dataOff;
    if (m_dataType == miUINT16 || m_dataType == miUTF16)
        return QString::fromUtf16(reinterpret_cast<const ushort *>(p), m_dataBytes / 2);
    return QString::fromUtf8(p, m_dataBytes);
}

MatArray MatArray::field(const QString &f, int elem) const
{
    const int idx = m_fields.indexOf(f);
    if (m_class != Struct || idx < 0 || elem < 0 || elem >= numel())
        return MatArray();
    buildIndex();
    const int child = elem * m_fields.size() + idx;
    if (child >= m_index->size())
        return MatArray();
    Tag t;
    const int off = m_index->at(child);
    if (!readTag(*m_buf, off, m_childEnd, &t) || t.type != miMATRIX)
        return MatArray();
    return parse(m_buf, t.data, t.size);
}

MatArray MatArray::cell(int i) const
{
    if (m_class != Cell || i < 0)
        return MatArray();
    buildIndex();
    if (i >= m_index->size())
        return MatArray();
    Tag t;
    if (!readTag(*m_buf, m_index->at(i), m_childEnd, &t) || t.type != miMATRIX)
        return MatArray();
    return parse(m_buf, t.data, t.size);
}

// ---------------------------------------------------------------
//  MatFile
// ---------------------------------------------------------------
bool MatFile::open(const QString &path, QString *error)
{
    auto fail = [error](const QString &msg) {
        if (error)
            *error = msg;
        return false;
    };
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("无法打开文件：%1").arg(f.errorString()));
    m_file = f.readAll();
    m_entries.clear();
    if (m_file.size() < 128 || !m_file.startsWith("MATLAB 5.0 MAT-file"))
        return fail(QStringLiteral("不是 MATLAB 5.0 MAT 文件（v7.3/HDF5 格式暂不支持）"));
    if (m_file.at(126) != 'I' || m_file.at(127) != 'M')
        return fail(QStringLiteral("不支持大端字节序的 MAT 文件"));

    qint64 off = 128;
    while (off + 8 <= m_file.size()) {
        const int type = int(rd32(m_file.constData() + off));
        const qint64 size = rd32(m_file.constData() + off + 4);
        if (off + 8 + size > m_file.size())
            break;
        Entry e{QString(), type, off + 8, size};
        if (type == miCOMPRESSED) {
            QByteArray head;
            // 只解出开头 512 字节读变量名；外层 miMATRIX 标签声明的长度远大于这段缓冲，
            // 所以这里直接读类型字段，不走 readTag 的越界检查
            if (inflateData(m_file.constData() + e.off, size, &head, 512) && head.size() >= 8) {
                Header h;
                if (int(rd32(head.constData())) == miMATRIX && readHeader(head, 8, head.size(), &h))
                    e.name = h.name;
            }
            off += 8 + size;          // 压缩元素之间不补齐 8 字节
        } else {
            Header h;
            if (type == miMATRIX && readHeader(m_file, int(e.off), int(e.off + size), &h))
                e.name = h.name;
            off += 8 + pad8(size);
        }
        if (!e.name.isEmpty())
            m_entries.append(e);
    }
    if (m_entries.isEmpty())
        return fail(QStringLiteral("MAT 文件中没有可读取的变量"));
    return true;
}

QStringList MatFile::variableNames() const
{
    QStringList names;
    for (const Entry &e : m_entries)
        names << e.name;
    return names;
}

MatArray MatFile::variable(const QString &name, QString *error) const
{
    for (const Entry &e : m_entries) {
        if (e.name != name)
            continue;
        QSharedPointer<QByteArray> buf(new QByteArray);
        if (e.type == miCOMPRESSED) {
            if (!inflateData(m_file.constData() + e.off, e.size, buf.data())) {
                if (error)
                    *error = QStringLiteral("变量 %1 解压失败").arg(name);
                return MatArray();
            }
            Tag t;
            if (!readTag(*buf, 0, buf->size(), &t) || t.type != miMATRIX)
                return MatArray();
            return MatArray::parse(buf, t.data, t.size);
        }
        *buf = m_file.mid(int(e.off), int(e.size));
        return MatArray::parse(buf, 0, buf->size());
    }
    if (error)
        *error = QStringLiteral("MAT 文件中没有变量 %1").arg(name);
    return MatArray();
}
