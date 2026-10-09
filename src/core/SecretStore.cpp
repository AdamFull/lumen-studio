#include "core/SecretStore.h"

#include "core/Log.h"
#include "core/Paths.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <wincred.h>
#endif

namespace lumen {
namespace {

QMutex g_errorMutex;
QString g_lastError;

void setError(const QString &e)
{
    QMutexLocker lock(&g_errorMutex);
    g_lastError = e;
    if (!e.isEmpty())
        qCWarning(lcApp) << "SecretStore:" << e;
}

QString targetName(const QString &key)
{
    return QStringLiteral("LumenStudio/") + key;
}

#ifdef Q_OS_WIN
QString winError(DWORD code)
{
    wchar_t *buffer = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    QString msg = buffer ? QString::fromWCharArray(buffer).trimmed() : QStringLiteral("error %1").arg(code);
    if (buffer)
        LocalFree(buffer);
    return msg;
}
#else
QString fallbackFile()
{
    return paths::appData() + QStringLiteral("/secrets.json");
}

QJsonObject loadFallback()
{
    QFile f(fallbackFile());
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

bool saveFallback(const QJsonObject &obj)
{
    QFile f(fallbackFile());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    f.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    return true;
}
#endif

} // namespace

bool SecretStore::write(const QString &key, const QString &secret)
{
    setError({});
    if (secret.isEmpty())
        return remove(key);
#ifdef Q_OS_WIN
    const std::wstring target = targetName(key).toStdWString();
    const QByteArray blob = secret.toUtf8();
    CREDENTIALW cred{};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<LPWSTR>(target.c_str());
    cred.CredentialBlobSize = static_cast<DWORD>(blob.size());
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(blob.constData()));
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    static const std::wstring user = L"LumenStudio";
    cred.UserName = const_cast<LPWSTR>(user.c_str());
    if (!CredWriteW(&cred, 0)) {
        setError(winError(GetLastError()));
        return false;
    }
    log::addRedaction(secret);
    return true;
#else
    QJsonObject obj = loadFallback();
    obj.insert(key, secret);
    if (!saveFallback(obj)) {
        setError(QStringLiteral("Cannot write secrets file"));
        return false;
    }
    log::addRedaction(secret);
    return true;
#endif
}

QString SecretStore::read(const QString &key)
{
    setError({});
#ifdef Q_OS_WIN
    const std::wstring target = targetName(key).toStdWString();
    PCREDENTIALW cred = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred)) {
        const DWORD err = GetLastError();
        if (err != ERROR_NOT_FOUND)
            setError(winError(err));
        return {};
    }
    const QString value = QString::fromUtf8(reinterpret_cast<const char *>(cred->CredentialBlob),
                                            static_cast<qsizetype>(cred->CredentialBlobSize));
    CredFree(cred);
    log::addRedaction(value);
    return value;
#else
    const QString value = loadFallback().value(key).toString();
    log::addRedaction(value);
    return value;
#endif
}

bool SecretStore::remove(const QString &key)
{
    setError({});
#ifdef Q_OS_WIN
    const std::wstring target = targetName(key).toStdWString();
    if (!CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) {
        const DWORD err = GetLastError();
        if (err == ERROR_NOT_FOUND)
            return true;
        setError(winError(err));
        return false;
    }
    return true;
#else
    QJsonObject obj = loadFallback();
    obj.remove(key);
    return saveFallback(obj);
#endif
}

bool SecretStore::isNativeBackend()
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

QString SecretStore::lastError()
{
    QMutexLocker lock(&g_errorMutex);
    return g_lastError;
}

} // namespace lumen
