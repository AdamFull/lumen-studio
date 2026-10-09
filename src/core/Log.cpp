#include "core/Log.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QSet>
#include <QTextStream>
#include <QThread>

#include <cstdio>

Q_LOGGING_CATEGORY(lcApp, "lumen.app")
Q_LOGGING_CATEGORY(lcNet, "lumen.net")
Q_LOGGING_CATEGORY(lcAi, "lumen.ai")
Q_LOGGING_CATEGORY(lcDoc, "lumen.doc")
Q_LOGGING_CATEGORY(lcUi, "lumen.ui")
Q_LOGGING_CATEGORY(lcPerf, "lumen.perf")

namespace lumen::log {
namespace {

constexpr qint64 kMaxLogSize = 8 * 1024 * 1024;
constexpr int kMaxLogFiles = 5;

struct LogState {
    QMutex mutex;
    QFile file;
    QSet<QString> secrets;
    QtMessageHandler previous = nullptr;
};

LogState &state()
{
    static LogState s;
    return s;
}

void rotate(const QString &dir)
{
    const QString base = dir + QStringLiteral("/lumen.log");
    QFileInfo info(base);
    if (!info.exists() || info.size() < kMaxLogSize)
        return;
    QFile::remove(dir + QStringLiteral("/lumen.%1.log").arg(kMaxLogFiles - 1));
    for (int i = kMaxLogFiles - 2; i >= 1; --i)
        QFile::rename(dir + QStringLiteral("/lumen.%1.log").arg(i), dir + QStringLiteral("/lumen.%1.log").arg(i + 1));
    QFile::rename(base, dir + QStringLiteral("/lumen.1.log"));
}

const char *levelName(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg: return "D";
    case QtInfoMsg: return "I";
    case QtWarningMsg: return "W";
    case QtCriticalMsg: return "E";
    case QtFatalMsg: return "F";
    }
    return "?";
}

QString redactLocked(QString text, const QSet<QString> &secrets)
{
    for (const QString &s : secrets) {
        if (s.size() >= 4)
            text.replace(s, QStringLiteral("***"));
    }
    return text;
}

void handler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    LogState &s = state();
    QString line;
    {
        QMutexLocker lock(&s.mutex);
        line = QStringLiteral("%1 [%2] %3 (%4) %5")
                   .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs),
                        QString::fromLatin1(levelName(type)),
                        QString::fromLatin1(ctx.category ? ctx.category : "default"),
                        QString::number(reinterpret_cast<quintptr>(QThread::currentThreadId()), 16),
                        redactLocked(msg, s.secrets));
        if (s.file.isOpen()) {
            QByteArray bytes = line.toUtf8();
            bytes.append('\n');
            s.file.write(bytes);
            if (type >= QtWarningMsg)
                s.file.flush();
        }
    }
    const QByteArray out = line.toLocal8Bit();
    std::fprintf(stderr, "%s\n", out.constData());
    std::fflush(stderr);
    if (type == QtFatalMsg) {
        QMutexLocker lock(&s.mutex);
        s.file.flush();
    }
}

} // namespace

void install(const QString &logDirectory)
{
    LogState &s = state();
    QDir().mkpath(logDirectory);
    rotate(logDirectory);
    {
        QMutexLocker lock(&s.mutex);
        s.file.setFileName(logDirectory + QStringLiteral("/lumen.log"));
        s.file.open(QIODevice::Append | QIODevice::Text);
    }
    s.previous = qInstallMessageHandler(handler);
    qCInfo(lcApp) << "---- log started ----";
}

void shutdown()
{
    LogState &s = state();
    qInstallMessageHandler(s.previous);
    QMutexLocker lock(&s.mutex);
    if (s.file.isOpen()) {
        s.file.flush();
        s.file.close();
    }
}

void addRedaction(const QString &secret)
{
    if (secret.isEmpty())
        return;
    QMutexLocker lock(&state().mutex);
    state().secrets.insert(secret);
}

void removeRedaction(const QString &secret)
{
    QMutexLocker lock(&state().mutex);
    state().secrets.remove(secret);
}

QString redact(QString text)
{
    QMutexLocker lock(&state().mutex);
    return redactLocked(std::move(text), state().secrets);
}

QString currentLogFile()
{
    QMutexLocker lock(&state().mutex);
    return state().file.fileName();
}

} // namespace lumen::log
