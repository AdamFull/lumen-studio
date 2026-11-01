#pragma once

#include <QFuture>
#include <QObject>
#include <QString>
#include <QUrl>

namespace lumen::net {
class HttpClient;
}

namespace lumen::runtime {

// The newest published release of Lumen Studio on GitHub. Drafts and pre-releases are never seen.
struct AppRelease
{
    QString version;          // "0.2.0"; empty when nothing is published yet
    QString notes;            // Markdown, as written on the release
    QUrl page;                // the release on GitHub
    QUrl installer;           // empty without an installer we could trust
    qint64 installerSize = -1;
    QString installerSha256;  // GitHub's own digest of the file
    QString error;

    bool ok() const { return error.isEmpty(); }
    // Only a file with a checksum and from an HTTPS address is ever run.
    bool canInstall() const { return !installer.isEmpty() && !installerSha256.isEmpty(); }
};

// True when `candidate` is a later version than `current` ("0.10.0" is later than "0.9.1").
bool isNewerVersion(const QString &candidate, const QString &current);

// Reads GitHub's answer for a release. `requireHttps` refuses an installer from a plain address;
// only tests against a local server turn it off.
AppRelease parseAppRelease(const QByteArray &json, bool requireHttps = true);

// True in a copy the installer put there, which the installer can replace. The portable version
// and a build folder only point at the download page.
bool isInstalledCopy();

// The folder downloaded installers wait in. They are removed at the next start.
QString appUpdateFolder();
void removeOldInstallers();

// Asks GitHub for the newest release. The future always finishes with a result: a failure is in
// AppRelease::error, and a repository with no releases yet gives an empty version.
class AppUpdateCheck : public QObject
{
    Q_OBJECT
public:
    explicit AppUpdateCheck(net::HttpClient *http, QObject *parent = nullptr);

    QFuture<AppRelease> latest();

    // Tests point this at a mock server.
    void setApiBase(const QUrl &base) { m_apiBase = base; }

private:
    net::HttpClient *m_http;
    QUrl m_apiBase{QStringLiteral("https://api.github.com")};
};

} // namespace lumen::runtime
