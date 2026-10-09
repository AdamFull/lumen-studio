#pragma once

#include <QString>
#include <QStringList>

namespace lumen::crash {

// Marks the running session with <dir>/session-<pid>.lock. A lock whose process is gone means that
// session ended without a clean exit (crash, kill, power loss). Several instances can run at once.
class SessionLock
{
public:
    explicit SessionLock(const QString &dir);
    SessionLock(const SessionLock &) = delete;
    SessionLock &operator=(const SessionLock &) = delete;

    // Stale locks found at start-up (their contents: start time, pid, log file), already removed.
    bool previousSessionCrashed() const { return !m_stale.isEmpty(); }
    QStringList staleSessions() const { return m_stale; }
    // Clean exit: removes this session's lock.
    void release();
    QString lockFile() const { return m_file; }

    // Whether a process id belongs to a running process (exposed for tests).
    static bool processAlive(qint64 pid);

private:
    QString m_file;
    QStringList m_stale;
};

// Installs last-chance handlers (unhandled SEH exceptions on Windows, std::terminate, SIGABRT)
// that write <dir>/crash-<timestamp>.txt and, on Windows, a minidump next to it.
void installHandlers(const QString &dir);

// Crash reports (.txt) in `dir`, newest first.
QStringList reports(const QString &dir);

} // namespace lumen::crash
