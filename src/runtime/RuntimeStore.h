#pragma once

#include "runtime/Catalog.h"

#include <QDateTime>
#include <QFuture>
#include <QJsonArray>
#include <QObject>
#include <QProcessEnvironment>

#include <optional>

namespace lumen {
class Job;
class JobQueue;
}

namespace lumen::runtime {

// A server program on this computer: downloaded by Lumen ("managed") or an existing install the
// user pointed Lumen to ("adopted"; Lumen never modifies those).
struct InstalledRuntime
{
    QString id;               // "llama.cpp/b11539-cuda-12" (managed) or "adopted/<uuid>"
    QString kind;             // catalogue runtime id
    bool managed = false;
    QString version;          // release tag (managed) or the probed version
    QString backend;          // backend id of a managed install
    QString directory;
    QString executable;       // absolute path of the server executable
    QStringList libraryDirs;  // shared DLL folders (CUDA runtime), prepended to PATH at start
    QDateTime installed;
    QString versionInfo;      // first line of `<exe> --version`
    QJsonArray assets;        // [{name, sha256, size}] of a managed install

    QString displayName() const;
    QJsonObject toJson() const;
    static InstalledRuntime fromJson(const QJsonObject &o);
};

// Environment for running a runtime's executables (library folders on PATH).
QProcessEnvironment runtimeEnvironment(const InstalledRuntime &rt);

// Runs `<exe> --version` (or the catalogue's version arguments) and returns the first meaningful
// output line. Throws lumen::Error when the program cannot start (missing DLLs, wrong platform).
QFuture<QString> probeVersion(const InstalledRuntime &rt, const QStringList &args);

// Known server executables in PATH and the usual install folders (for "Add existing").
QStringList findExistingInstalls();

class RuntimeStore : public QObject
{
    Q_OBJECT
public:
    RuntimeStore(net::HttpClient *http, JobQueue *jobs, QObject *parent = nullptr);

    // <app data>/runtimes by default; tests use a temporary folder.
    void setRoot(const QString &root);
    QString root() const { return m_root; }

    void load();
    void save() const;

    QList<InstalledRuntime> runtimes() const { return m_runtimes; }
    QList<InstalledRuntime> runtimes(const QString &kind) const;
    std::optional<InstalledRuntime> runtime(const QString &id) const;
    // Newest managed install of a kind (for update checks).
    std::optional<InstalledRuntime> latestManaged(const QString &kind) const;

    ReleaseFeed *feed() { return &m_feed; }
    net::HttpClient *http() const { return m_http; }

    // Downloads, verifies and unpacks a release as a Queue job. The job result is the new runtime id.
    Job *install(const InstallPlan &plan);
    // Registers an existing server executable after checking that it runs.
    QFuture<InstalledRuntime> adopt(const QString &executable);
    // Deletes a managed install's folder (adopted ones are only forgotten). Shared CUDA runtime
    // folders are removed when no install uses them any more.
    bool remove(const QString &id, QString *error = nullptr);

    // Called by the install job.
    void addInstalled(const InstalledRuntime &rt);

signals:
    void changed();

private:
    QString configPath() const;

    net::HttpClient *m_http;
    JobQueue *m_jobs;
    ReleaseFeed m_feed;
    QString m_root;
    QList<InstalledRuntime> m_runtimes;
};

} // namespace lumen::runtime
