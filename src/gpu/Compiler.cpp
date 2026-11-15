#include "gpu/Compiler.h"

#include "core/Paths.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>

namespace lumen::gpu {

namespace {

QMutex g_lock;
QString g_program;

bool samePath(const QString &a, const QString &b)
{
#ifdef Q_OS_WIN
    const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
    const Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
    return QDir::cleanPath(QDir::fromNativeSeparators(a)).compare(QDir::cleanPath(QDir::fromNativeSeparators(b)), cs) == 0;
}

// Lumen's modules as files slangc can read: written once per version of them, into the cache.
QString modulesFolder(QString *error)
{
    QCryptographicHash hash(QCryptographicHash::Sha1);
    QList<QPair<QString, QByteArray>> files;
    QDirIterator it(QStringLiteral(":/gpu/modules"), {QStringLiteral("*.slang")}, QDir::Files);
    while (it.hasNext()) {
        it.next();
        QFile f(it.filePath());
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QByteArray bytes = f.readAll();
        hash.addData(it.fileName().toUtf8());
        hash.addData(bytes);
        files.append({it.fileName(), bytes});
    }
    const QString folder = paths::cache() + QStringLiteral("/slang-modules/") + QString::fromLatin1(hash.result().toHex().left(12));
    static QMutex lock;
    QMutexLocker locker(&lock);
    if (QFileInfo::exists(folder + QStringLiteral("/effect.slang")))
        return folder;
    QDir().mkpath(folder);
    for (const auto &[name, bytes] : std::as_const(files)) {
        QFile out(folder + u'/' + name);
        if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size()) {
            if (error)
                *error = QStringLiteral("The effect modules could not be written to %1.").arg(QDir::toNativeSeparators(folder));
            return {};
        }
    }
    return folder;
}

} // namespace

void Compiler::setProgram(const QString &slangc)
{
    QMutexLocker locker(&g_lock);
    g_program = slangc;
}

QString Compiler::program()
{
    const QString env = qEnvironmentVariable("LUMEN_SLANGC");
    if (!env.isEmpty())
        return env;
    QMutexLocker locker(&g_lock);
    return g_program;
}

bool Compiler::isAvailable()
{
    const QString slangc = program();
    return !slangc.isEmpty() && QFileInfo(slangc).isFile();
}

QList<CompileMessage> Compiler::parseMessages(const QString &output, const QString &sourcePath)
{
    // code \t severity \t file \t line \t column \t end line \t end column \t text; a "span" line
    // after a message says it more exactly ("undefined identifier 'x'").
    QList<CompileMessage> out;
    for (QString line : output.split(u'\n')) {
        line = line.trimmed();
        const QStringList f = line.split(u'\t');
        if (f.size() < 8)
            continue;
        const QString severity = f.at(1).toLower();
        const QString text = f.mid(7).join(u'\t').trimmed();
        if (severity == QLatin1String("span")) {
            if (!out.isEmpty() && out.last().code == f.at(0) && !text.isEmpty())
                out.last().text = text;
            continue;
        }
        CompileMessage m;
        m.code = f.at(0);
        m.severity = severity;
        m.text = text;
        m.inSource = samePath(f.at(2), sourcePath);
        m.line = f.at(3).toInt();
        m.column = f.at(4).toInt();
        m.endLine = f.at(5).toInt();
        m.endColumn = f.at(6).toInt();
        out << m;
    }
    return out;
}

Compiled Compiler::compile(const QString &source, const QString &name)
{
    Compiled result;
    const QString slangc = program();
    if (slangc.isEmpty() || !QFileInfo(slangc).isFile()) {
        result.error = QStringLiteral("The Slang compiler is not installed.");
        return result;
    }
    const QString modules = modulesFolder(&result.error);
    if (modules.isEmpty())
        return result;
    QTemporaryDir work;
    if (!work.isValid()) {
        result.error = QStringLiteral("No folder to compile in: %1").arg(work.errorString());
        return result;
    }
    // Never a module's name ("effect" would import itself).
    QString file = name;
    file.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")), QStringLiteral("_"));
    const QString sourcePath = work.filePath(QStringLiteral("your_") + file + QStringLiteral(".slang"));
    {
        QFile f(sourcePath);
        if (!f.open(QIODevice::WriteOnly) || f.write(source.toUtf8()) < 0) {
            result.error = QStringLiteral("The source could not be written to %1.").arg(QDir::toNativeSeparators(sourcePath));
            return result;
        }
    }
    const QString spv = work.filePath(QStringLiteral("effect.spv"));
    const QString json = work.filePath(QStringLiteral("effect.json"));
    // As the built-in effects are compiled (cmake/LumenSlang.cmake), every compute function at once.
    const QStringList args{sourcePath, QStringLiteral("-I"), modules, QStringLiteral("-target"), QStringLiteral("spirv"),
                           QStringLiteral("-profile"), QStringLiteral("spirv_1_3"), QStringLiteral("-emit-spirv-directly"),
                           QStringLiteral("-fvk-use-entrypoint-name"), QStringLiteral("-warnings-disable"), QStringLiteral("39001"),
                           QStringLiteral("-enable-machine-readable-diagnostics"), QStringLiteral("-o"), spv,
                           QStringLiteral("-reflection-json"), json};
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.setWorkingDirectory(work.path());
    process.start(slangc, args);
    if (!process.waitForStarted(10000)) {
        result.error = QStringLiteral("The Slang compiler did not start: %1").arg(process.errorString());
        return result;
    }
    if (!process.waitForFinished(60000)) {
        process.kill();
        process.waitForFinished(2000);
        result.error = QStringLiteral("The Slang compiler took more than a minute.");
        return result;
    }
    const QString output = QString::fromUtf8(process.readAll());
    result.messages = parseMessages(output, sourcePath);
    for (const CompileMessage &m : std::as_const(result.messages)) {
        if (m.severity == QLatin1String("error") && result.error.isEmpty())
            result.error = m.line > 0 && m.inSource ? QStringLiteral("Line %1: %2").arg(m.line).arg(m.text) : m.text;
    }
    // This slangc may crash on its way out after an error: its messages say what went wrong.
    if (!result.error.isEmpty())
        return result;
    QFile spvFile(spv);
    QFile jsonFile(json);
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 || !spvFile.open(QIODevice::ReadOnly)
        || !jsonFile.open(QIODevice::ReadOnly)) {
        const QString said = output.trimmed().left(400);
        result.error = said.isEmpty() ? QStringLiteral("The Slang compiler stopped (exit code %1).").arg(process.exitCode()) : said;
        return result;
    }
    result.spirv = spvFile.readAll();
    result.reflection = jsonFile.readAll();
    return result;
}

} // namespace lumen::gpu
