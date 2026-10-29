#ifndef APPPATHS_H
#define APPPATHS_H

#include <QString>
#include <QVariant>

class QSettings;

// 数据库与设置的存放位置。自检模式下全部放在 exe 旁并在启动时清空，不碰用户的真实数据
namespace app {

void setTestMode(bool on);
bool testMode();
// 航迹数据库（RadarView 为 app_data_dir/radarview.db）
QString databasePath();
// 用户设置（正常运行用 QSettings("RadarView", "HelloVscode")，自检用 exe 旁的 ini）
QSettings &settings();

// 写设置：立即写进 QSettings 的内存缓存（后续 value() 马上能读到），
// 真正落盘合并到 300ms 后一次 sync（拖动滑块/拖拽分栏时不会每像素写一次盘）。
// 读设置仍直接用 settings().value()。
void setValue(const QString &key, const QVariant &value);
void removeValue(const QString &key);
// 立即同步到磁盘（关闭窗口、退出前调用）
void flushSettings();

// 绘图内容（DrawingManager 的 GeoJSON）自动保存文件，与数据库同目录
QString drawingPath();

} // namespace app

#endif // APPPATHS_H
