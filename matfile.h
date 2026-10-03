#ifndef MATFILE_H
#define MATFILE_H

#include <QByteArray>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QVector>

// 精简的 MATLAB MAT v5 读取器（只读、小端）
// 支持：miCOMPRESSED（zlib）、数值矩阵、结构体（数组）、元胞、字符；稀疏/对象按空值处理
// 数据不做拷贝：MatArray 只记录在解压缓冲区里的偏移，结构体字段按需建立索引
class MatArray
{
public:
    enum Class { Invalid = 0, Cell = 1, Struct = 2, Object = 3, Char = 4, Sparse = 5,
                 Double = 6, Single = 7, Int8 = 8, UInt8 = 9, Int16 = 10, UInt16 = 11,
                 Int32 = 12, UInt32 = 13, Int64 = 14, UInt64 = 15 };

    MatArray() = default;

    bool isValid() const { return m_class != Invalid; }
    Class classId() const { return Class(m_class); }
    bool isStruct() const { return m_class == Struct; }
    bool isNumeric() const { return m_class >= Double && m_class <= UInt64; }
    QString name() const { return m_name; }
    const QVector<int> &dims() const { return m_dims; }
    int numel() const;

    // 数值：第 i 个元素（实部），失败时 ok=false 并返回 0
    double scalar(int i = 0, bool *ok = nullptr) const;
    QString toText() const;   // 字符数组

    // 结构体：字段名、第 elem 个元素的某字段
    QStringList fieldNames() const { return m_fields; }
    bool hasField(const QString &f) const { return m_fields.contains(f); }
    MatArray field(const QString &f, int elem = 0) const;
    // 元胞：第 i 个元素
    MatArray cell(int i) const;

private:
    friend class MatFile;
    static MatArray parse(const QSharedPointer<QByteArray> &buf, int off, int len);
    void buildIndex() const;

    QSharedPointer<QByteArray> m_buf;
    int m_class = Invalid;
    bool m_complex = false;
    QVector<int> m_dims;
    QString m_name;
    // 数值/字符数据
    int m_dataType = 0, m_dataOff = 0, m_dataBytes = 0;
    // 结构体/元胞：子元素起止
    QStringList m_fields;
    int m_childOff = 0, m_childEnd = 0;
    mutable QSharedPointer<QVector<int>> m_index;   // 每个子元素 miMATRIX 标签的偏移
};

class MatFile
{
public:
    bool open(const QString &path, QString *error = nullptr);
    QStringList variableNames() const;
    // 读取并解压指定变量；不存在时返回无效 MatArray
    MatArray variable(const QString &name, QString *error = nullptr) const;

private:
    struct Entry { QString name; int type; qint64 off; qint64 size; };
    QByteArray m_file;
    QVector<Entry> m_entries;
};

#endif // MATFILE_H
