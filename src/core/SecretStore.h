#pragma once

#include <QString>

namespace lumen {

// Stores secrets (API keys, tokens) in the operating system credential store.
// Windows: Credential Manager (generic credentials, per-user, DPAPI protected).
// Other platforms: an owner-only file in app data with a warning (until a native backend is added).
class SecretStore
{
public:
    static bool write(const QString &key, const QString &secret);
    static QString read(const QString &key);
    static bool remove(const QString &key);
    static bool isNativeBackend();
    static QString lastError();
};

} // namespace lumen
