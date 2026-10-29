#ifndef APPPATHS_H
#define APPPATHS_H

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>

// 数据库与设置的存放位置。自检模式下全部放在 exe 旁并在启动时清空，不碰用户的真实数据
namespace app {

void setTestMode(bool on);
bool testMode();
// 航迹数据库（RadarView 为 app_data_dir/radarview.db）
QString databasePath();
// 绘图内容（DrawingManager 的 GeoJSON）自动保存文件，与数据库同目录
QString drawingPath();

// 键值设置，存在航迹数据库的 app_settings(key, value, type) 表里：
// 和航迹数据同一个文件，整包拷走就带走全部状态；value 存明文，SQL 里能直接看。
// 读取走内存缓存，写入先落缓存再合并写库（300ms），退出时 sync。
class Settings
{
public:
    QVariant value(const QString &key, const QVariant &defaultValue = QVariant());
    void setValue(const QString &key, const QVariant &value);
    void remove(const QString &key);
    bool contains(const QString &key);
    QStringList keys();
    void sync();                       // 立即把改动写进数据库
private:
    void ensureLoaded();
    QHash<QString, QVariant> m_cache;
    bool m_loaded = false;
    QSet<QString> m_dirty;             // 待写
    QSet<QString> m_removed;           // 待删
};

Settings &settings();

// 写设置：立即进内存缓存（后续 value() 马上能读到），落库合并到 300ms 后
void setValue(const QString &key, const QVariant &value);
void removeValue(const QString &key);
// 立即落库（关闭窗口、退出前调用）
void flushSettings();
// 关闭设置连接（切换自检模式时调用，否则删库文件会被占用）
void closeSettings();

} // namespace app

#endif // APPPATHS_H
