#include "ai/Provider.h"

namespace lumen::ai {

Provider::Provider(ProviderType type, QString instanceId, net::HttpClient *http, QObject *parent)
    : QObject(parent)
    , m_type(std::move(type))
    , m_instanceId(std::move(instanceId))
    , m_http(http)
{
    for (const SettingField &f : m_type.fields) {
        if (f.defaultValue.isValid())
            m_settings.insert(f.key, f.defaultValue);
    }
}

void Provider::configure(const QVariantMap &settings)
{
    for (auto it = settings.begin(); it != settings.end(); ++it)
        m_settings.insert(it.key(), it.value());
    onConfigured();
    emit configurationChanged();
}

QVariant Provider::setting(const QString &key) const
{
    return m_settings.value(key);
}

QUrl Provider::baseUrl(const QString &key) const
{
    QString url = text(key);
    while (url.endsWith(u'/'))
        url.chop(1);
    return QUrl(url);
}

QUrl Provider::endpoint(const QString &path, const QString &baseKey) const
{
    QString base = text(baseKey);
    while (base.endsWith(u'/'))
        base.chop(1);
    return QUrl(base + (path.startsWith(u'/') ? path : u'/' + path));
}

QFuture<QStringList> Provider::listOptions(const QString &)
{
    return QtFuture::makeReadyValueFuture(QStringList());
}

ImageOperation *ImageProvider::runUtility(Utility utility, const QImage &, const QJsonObject &)
{
    auto *op = new ImageOperation(this);
    const QString name = utilityName(utility);
    QMetaObject::invokeMethod(op, [op, name] {
        op->fail(QObject::tr("This provider does not support %1").arg(name));
    }, Qt::QueuedConnection);
    return op;
}

QFuture<QStringList> ImageProvider::listSamplers()
{
    return QtFuture::makeReadyValueFuture(QStringList());
}

QFuture<QStringList> ImageProvider::listSchedulers()
{
    return QtFuture::makeReadyValueFuture(QStringList());
}

QFuture<QList<ServerLora>> ImageProvider::availableLoras()
{
    return QtFuture::makeReadyValueFuture(QList<ServerLora>());
}

QString errorMessage(const std::exception_ptr &e)
{
    try {
        if (e)
            std::rethrow_exception(e);
    } catch (const std::exception &ex) {
        return describeException(ex);
    } catch (...) {
        return QObject::tr("Unknown error");
    }
    return {};
}

} // namespace lumen::ai
