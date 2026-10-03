#ifndef APPPATHS_H
#define APPPATHS_H

#include <QString>

class QSettings;

// 数据库与设置的存放位置。自检模式下全部放在 exe 旁并在启动时清空，不碰用户的真实数据
namespace app {

void setTestMode(bool on);
bool testMode();
// 航迹数据库（RadarView 为 app_data_dir/radarview.db）
QString databasePath();
// 用户设置（正常运行用 QSettings("RadarView", "HelloVscode")，自检用 exe 旁的 ini）
QSettings &settings();

} // namespace app

#endif // APPPATHS_H
