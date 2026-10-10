#pragma once

#include "runtime/ServerProfile.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFuture>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QPromise>
#include <QTimer>

#include <memory>
#include <optional>

class QProcess;

namespace lumen::net {
class HttpClient;
class HttpCall;
}

namespace lumen::runtime {

enum class ServerState { Stopped, Starting, Ready, Stopping, Failed };
QString serverStateLabel(ServerState s);

// Starts, watches and stops local servers described by ServerProfiles.
// * Every server runs in its own Windows job object: stopping it ends its whole process tree, and
//   the servers die with Lumen even when Lumen crashes.
// * A server is "ready" when its health endpoint answers 200 (llama.cpp answers 503 while loading).
// * Users acquire() a server before a request and release() it after; an unused server stops
//   after its idle time. Crashes while in use restart it (at most 3 times in 10 minutes).
// GUI-thread object.
class ServerSupervisor : public QObject
{
    Q_OBJECT
public:
    ServerSupervisor(RuntimeStore *store, net::HttpClient *http, QObject *parent = nullptr);
    ~ServerSupervisor() override;

    RuntimeStore *store() const { return m_store; }

    // <app data>/servers.json by default.
    void setConfigPath(const QString &path) { m_configPath = path; }
    void setLogDir(const QString &dir) { m_logDir = dir; }
    void load();
    void save() const;

    // ---- profiles -------------------------------------------------------------------------------
    QList<ServerProfile> profiles() const { return m_profiles; }
    std::optional<ServerProfile> profile(const QString &id) const;
    // A new, unsaved profile with defaults and a free port.
    ServerProfile createProfile(const QString &runtimeId, const QString &name = QString()) const;
    void addProfile(const ServerProfile &profile);
    void updateProfile(const ServerProfile &profile);
    void removeProfile(const QString &id);
    QString kindOf(const QString &profileId) const;
    LaunchCommand launchFor(const QString &profileId) const;

    // ---- control --------------------------------------------------------------------------------
    // Resolves to "" once the server is ready, or to an error message. Never throws.
    QFuture<QString> start(const QString &id);
    void stop(const QString &id);
    QFuture<QString> restart(const QString &id);
    // start() plus a usage count that keeps the server from idling out until release().
    QFuture<QString> acquire(const QString &id);
    void release(const QString &id);
    void startAutoStartServers();
    void stopAll();

    ServerState state(const QString &id) const;
    QString statusText(const QString &id) const;
    QString lastError(const QString &id) const;
    QStringList log(const QString &id) const;
    QString logPath(const QString &id) const;
    // The profile was edited while the server runs with the old command line.
    bool needsRestart(const QString &id) const;
    int users(const QString &id) const;

    int healthIntervalMs = 500;
    int maxRestarts = 3;

signals:
    void profilesChanged();
    void stateChanged(const QString &id);
    void logAppended(const QString &id, const QString &line);

private:
    struct Server
    {
        QPointer<QProcess> process;
        void *job = nullptr; // Windows job object
        ServerState state = ServerState::Stopped;
        QString status;
        QString error;
        QStringList log;
        QString partialLine;
        std::unique_ptr<QFile> logFile;
        QList<std::shared_ptr<QPromise<QString>>> waiters;
        QTimer probeTimer;
        QTimer idleTimer;
        QElapsedTimer started;
        QPointer<net::HttpCall> probe;
        QString commandLine;
        int users = 0;
        int startTimeoutSec = 600;
        QList<qint64> crashTimes; // msecs since epoch
        bool stopping = false;
    };

    Server *server(const QString &id);
    const Server *server(const QString &id) const;
    void launch(const QString &id);
    void probeHealth(const QString &id);
    void onOutput(const QString &id);
    void appendLog(const QString &id, Server *s, const QString &line);
    void onExited(const QString &id, int code, bool crashed);
    void setState(const QString &id, ServerState state, const QString &status = QString());
    void settleWaiters(Server *s, const QString &error);
    void killProcess(Server *s);
    void armIdle(const QString &id);
    QString configPath() const;

    RuntimeStore *m_store;
    net::HttpClient *m_http;
    QString m_configPath;
    QString m_logDir;
    QList<ServerProfile> m_profiles;
    QHash<QString, Server *> m_servers;
};

} // namespace lumen::runtime
