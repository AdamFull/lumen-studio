#include "core/Settings.h"

#include "core/Paths.h"

#include <QThread>

namespace lumen {

Settings &Settings::instance()
{
    static Settings s;
    return s;
}

Settings::Settings()
    : m_settings(paths::appData() + QStringLiteral("/settings.ini"), QSettings::IniFormat)
{
}

QVariant Settings::value(const QString &key, const QVariant &defaultValue) const
{
    return m_settings.value(key, defaultValue);
}

void Settings::setValue(const QString &key, const QVariant &value)
{
    if (m_settings.value(key) == value && m_settings.contains(key))
        return;
    m_settings.setValue(key, value);
    emit changed(key, value);
}

void Settings::remove(const QString &key)
{
    m_settings.remove(key);
    emit changed(key, {});
}

bool Settings::contains(const QString &key) const
{
    return m_settings.contains(key);
}

void Settings::sync()
{
    m_settings.sync();
}

int Settings::workerThreads() const
{
    const int configured = value(QStringLiteral("perf/workerThreads"), 0).toInt();
    if (configured > 0)
        return configured;
    return qMax(2, QThread::idealThreadCount() - 1);
}

void Settings::addRecentProject(const QString &path)
{
    QStringList list = recentProjects();
    list.removeAll(path);
    list.prepend(path);
    while (list.size() > 12)
        list.removeLast();
    setValue(QStringLiteral("app/recentProjects"), list);
}

} // namespace lumen
