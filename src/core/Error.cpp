#include "core/Error.h"

#include <QCoreApplication>

namespace lumen {

QString describeException(const std::exception_ptr &e)
{
    if (!e)
        return {};
    try {
        std::rethrow_exception(e);
    } catch (const std::exception &ex) {
        return describeException(ex);
    } catch (...) {
        return QCoreApplication::translate("lumen", "Unknown error");
    }
    return {};
}

QString describeException(const std::exception &e)
{
    if (const auto *unhandled = dynamic_cast<const QUnhandledException *>(&e)) {
        if (unhandled->exception())
            return describeException(unhandled->exception());
        return QCoreApplication::translate("lumen", "Unknown error");
    }
    return QString::fromUtf8(e.what());
}

} // namespace lumen
