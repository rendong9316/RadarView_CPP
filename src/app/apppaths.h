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
// 在线底图磁盘缓存根目录（正常模式：app_data_dir/tilecache；自检模式：exe 旁 test_tilecache）
QString tileCacheDir();
// 旧版 HelloVscode 数据目录（仅用于迁移与只读回退）
QString legacyDataDir();
// 将旧数据目录（HelloVscode）中的数据库、绘图与瓦片缓存迁移到新目录（RadarView）。
// 仅在新目录尚缺对应条目时执行，不覆盖已存在的新数据；返回是否全部成功（无拷贝/迁移失败）。
bool migrateLegacyDataDir(const QString &legacyDir, const QString &newDir);

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
