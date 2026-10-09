#include "core/CrashGuard.h"

#include "core/Log.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QRegularExpression>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

namespace lumen::crash {

SessionLock::SessionLock(const QString &dir)
{
    QDir().mkpath(dir);
    const QDir d(dir);
    static const QRegularExpression rx(QStringLiteral("^session-(\\d+)\\.lock$"));
    const qint64 self = QCoreApplication::applicationPid();
    for (const QString &name : d.entryList({QStringLiteral("session-*.lock")}, QDir::Files)) {
        const auto m = rx.match(name);
        if (!m.hasMatch())
            continue;
        const qint64 pid = m.captured(1).toLongLong();
        if (pid != self && processAlive(pid))
            continue; // another running instance
        QFile f(d.filePath(name));
        if (f.open(QIODevice::ReadOnly))
            m_stale << QString::fromUtf8(f.readAll()).trimmed();
        f.close();
        QFile::remove(d.filePath(name));
    }
    m_file = d.filePath(QStringLiteral("session-%1.lock").arg(self));
    QFile f(m_file);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QStringLiteral("started=%1 pid=%2 log=%3\n")
                    .arg(QDateTime::currentDateTime().toString(Qt::ISODate))
                    .arg(self)
                    .arg(log::currentLogFile())
                    .toUtf8());
    }
}

void SessionLock::release()
{
    if (!m_file.isEmpty())
        QFile::remove(m_file);
    m_file.clear();
}

bool SessionLock::processAlive(qint64 pid)
{
    if (pid <= 0)
        return false;
#ifdef Q_OS_WIN
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!h)
        return GetLastError() == ERROR_ACCESS_DENIED; // exists but belongs to someone else
    const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
#else
    return ::kill(pid_t(pid), 0) == 0;
#endif
}

// ---- last-chance handlers ---------------------------------------------------------------------------
// Everything a handler needs is prepared up front: handlers must not allocate or take locks.

namespace {

char g_reportPath[1024];
#ifdef Q_OS_WIN
wchar_t g_dumpPath[1024];
#endif
char g_version[64];

void writeReport(const char *what, const char *detail)
{
    std::FILE *f = std::fopen(g_reportPath, "w");
    if (!f)
        return;
    std::fprintf(f, "Lumen Studio %s crashed: %s\n%s\n", g_version, what, detail ? detail : "");
#ifdef Q_OS_WIN
    std::fprintf(f, "A minidump was written next to this file (crash-*.dmp).\n");
#endif
    std::fclose(f);
}

#ifdef Q_OS_WIN
using MiniDumpWriteDumpFn = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                           PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

void writeMinidump(EXCEPTION_POINTERS *info)
{
    HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    if (!dbghelp)
        return;
    auto fn = reinterpret_cast<MiniDumpWriteDumpFn>(reinterpret_cast<void *>(GetProcAddress(dbghelp, "MiniDumpWriteDump")));
    HANDLE file = CreateFileW(g_dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fn && file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = info;
        mei.ClientPointers = FALSE;
        fn(GetCurrentProcess(), GetCurrentProcessId(), file, MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
           info ? &mei : nullptr, nullptr, nullptr);
    }
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
}

LONG WINAPI unhandledException(EXCEPTION_POINTERS *info)
{
    char detail[256];
    std::snprintf(detail, sizeof detail, "exception 0x%08lX at %p", info ? info->ExceptionRecord->ExceptionCode : 0UL,
                  info ? info->ExceptionRecord->ExceptionAddress : nullptr);
    writeReport("unhandled exception", detail);
    writeMinidump(info);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void onTerminate()
{
    const char *detail = "std::terminate";
    if (auto e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception &ex) {
            detail = ex.what();
        } catch (...) {
            detail = "unknown exception";
        }
    }
    writeReport("uncaught C++ exception", detail);
#ifdef Q_OS_WIN
    writeMinidump(nullptr);
#endif
    std::_Exit(3);
}

void onAbort(int)
{
    writeReport("abort", "SIGABRT (assertion or fatal error; see the log)");
#ifdef Q_OS_WIN
    writeMinidump(nullptr);
#endif
    std::_Exit(3);
}

} // namespace

void installHandlers(const QString &dir)
{
    QDir().mkpath(dir);
    // Keep the five most recent reports and dumps.
    const QDir d(dir);
    for (const char *pattern : {"crash-*.txt", "crash-*.dmp"}) {
        const QStringList old = d.entryList({QLatin1String(pattern)}, QDir::Files, QDir::Name | QDir::Reversed);
        for (int i = 5; i < old.size(); ++i)
            QFile::remove(d.filePath(old[i]));
    }
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QByteArray report = QDir::toNativeSeparators(dir + QStringLiteral("/crash-%1.txt").arg(stamp)).toLocal8Bit();
    qstrncpy(g_reportPath, report.constData(), sizeof g_reportPath);
    qstrncpy(g_version, QCoreApplication::applicationVersion().toLatin1().constData(), sizeof g_version);
#ifdef Q_OS_WIN
    const QString dump = QDir::toNativeSeparators(dir + QStringLiteral("/crash-%1.dmp").arg(stamp));
    const int n = std::min<int>(int(dump.size()), int(sizeof g_dumpPath / sizeof(wchar_t)) - 1);
    dump.left(n).toWCharArray(g_dumpPath);
    g_dumpPath[n] = 0;
    SetUnhandledExceptionFilter(unhandledException);
#endif
    std::set_terminate(onTerminate);
    std::signal(SIGABRT, onAbort);
}

QStringList reports(const QString &dir)
{
    QStringList out;
    const QDir d(dir);
    for (const QString &name : d.entryList({QStringLiteral("crash-*.txt")}, QDir::Files, QDir::Name | QDir::Reversed))
        out << d.filePath(name);
    return out;
}

} // namespace lumen::crash
