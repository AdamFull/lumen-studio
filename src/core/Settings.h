#pragma once

#include <QObject>
#include <QSettings>
#include <QVariant>

namespace lumen {

// Application-wide settings backed by an INI file in the app-data directory.
// GUI-thread object; emits changed() for any key written through it.
class Settings : public QObject
{
    Q_OBJECT
public:
    static Settings &instance();

    QVariant value(const QString &key, const QVariant &defaultValue = {}) const;
    void setValue(const QString &key, const QVariant &value);
    void remove(const QString &key);
    bool contains(const QString &key) const;
    void sync();

    // Typed accessors for commonly used settings.
    QString theme() const { return value(QStringLiteral("ui/theme"), QStringLiteral("auto")).toString(); }
    // UI language ("" = the system language).
    QString language() const { return value(QStringLiteral("ui/language")).toString(); }
    int workerThreads() const;
    qint64 undoMemoryLimitMb() const { return value(QStringLiteral("perf/undoMemoryMb"), 1024).toLongLong(); }
    int autosaveIntervalSec() const { return value(QStringLiteral("doc/autosaveSec"), 60).toInt(); }
    QStringList recentProjects() const { return value(QStringLiteral("app/recentProjects")).toStringList(); }
    void addRecentProject(const QString &path);

signals:
    void changed(const QString &key, const QVariant &value);

private:
    Settings();
    QSettings m_settings;
};

} // namespace lumen
