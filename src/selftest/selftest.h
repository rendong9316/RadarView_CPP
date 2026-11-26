#ifndef SELFTEST_H
#define SELFTEST_H

#include <QString>

class MainWindow;

namespace selftest {

int runParseTest(const QString &csv, const QString &mat);
int runPointTest(const QString &input, const QString &trackFile);
int runDbMigrationTest();
void runSelfTest(MainWindow *w);
void runTrackTest(MainWindow *w, const QString &csv, const QString &mat);
void runDebugTest(MainWindow *w, const QString &mat, const QString &detDir, const QString &adsbCsv);
int runStateCheck();

} // namespace selftest

#endif // SELFTEST_H
