#pragma once

#include <QLoggingCategory>
#include <QString>

Q_DECLARE_LOGGING_CATEGORY(lcApp)
Q_DECLARE_LOGGING_CATEGORY(lcNet)
Q_DECLARE_LOGGING_CATEGORY(lcAi)
Q_DECLARE_LOGGING_CATEGORY(lcDoc)
Q_DECLARE_LOGGING_CATEGORY(lcUi)
Q_DECLARE_LOGGING_CATEGORY(lcPerf)

namespace lumen::log {

// Installs the global message handler: writes to stderr and to a rotating log file.
void install(const QString &logDirectory);

// Flushes and closes the log file (called on shutdown).
void shutdown();

// Registers a secret that must never appear in logs. Every occurrence is replaced with "***".
void addRedaction(const QString &secret);
void removeRedaction(const QString &secret);

// Returns the text with all registered secrets redacted.
QString redact(QString text);

QString currentLogFile();

} // namespace lumen::log
