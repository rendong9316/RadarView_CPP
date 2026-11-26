#ifndef SELFTEST_COMMON_H
#define SELFTEST_COMMON_H

#include <QEvent>
#include <QObject>

namespace selftest {

// 自检用：丢掉来自窗口系统的鼠标事件，只保留测试代码发出的。
// Enter/Leave 由 QApplication 转发时不是 spontaneous，所以一律拦下，只放行 allowed 指向的那个
class RealMouseBlocker : public QObject
{
public:
    using QObject::QObject;
    QEvent *allowed = nullptr;
    bool eventFilter(QObject *, QEvent *e) override
    {
        switch (e->type()) {
        case QEvent::Enter: case QEvent::Leave:
            return e != allowed;
        case QEvent::MouseMove: case QEvent::MouseButtonPress: case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick: case QEvent::ContextMenu: case QEvent::Wheel:
            return e->spontaneous();
        default:
            return false;
        }
    }
};

} // namespace selftest

#endif // SELFTEST_COMMON_H
