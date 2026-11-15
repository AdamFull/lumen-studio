#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

// Compiling your own effects (plan M20 step 7): Slang source to SPIR-V and its reflection, with the
// compiler's messages by line, so the effect editor can show them in place. The compiler is slangc,
// downloaded as the "slang" component (or the one LUMEN_SLANGC names); Lumen's own modules (effect,
// colorspace, sampling) are there to import, as for the built-in effects.
namespace lumen::gpu {

struct CompileMessage
{
    QString severity;  // "error", "warning", "note"
    QString code;      // "E30015"
    QString text;      // in the compiler's words
    int line = 0;      // 1-based, in the effect's source; 0 when it is about something else
    int column = 0;    // 1-based
    int endLine = 0;
    int endColumn = 0;
    bool inSource = false; // about the effect's own file (not one of the modules)
};

struct Compiled
{
    QByteArray spirv;
    QByteArray reflection;
    QList<CompileMessage> messages;
    QString error; // the compiler could not run, or the source has errors (the first one)

    bool ok() const { return error.isEmpty() && !spirv.isEmpty(); }
};

class Compiler
{
public:
    // The slangc to use; empty: none (LUMEN_SLANGC when set wins).
    static void setProgram(const QString &slangc);
    static QString program();
    static bool isAvailable();

    // Compiles an effect's source (blocking: call it from a worker thread). `name` names the file
    // in messages.
    static Compiled compile(const QString &source, const QString &name = QStringLiteral("effect"));

    // The machine-readable lines slangc writes, as messages; `sourcePath` is the effect's file.
    static QList<CompileMessage> parseMessages(const QString &output, const QString &sourcePath);
};

} // namespace lumen::gpu
