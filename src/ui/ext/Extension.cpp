#include "ui/ext/Extension.h"

#include "core/Settings.h"

#include <algorithm>

namespace lumen::ui::ext {

namespace {

QString enabledKey(const QString &id)
{
    return QStringLiteral("plugins/%1/enabled").arg(id);
}

template<typename Spec>
void sortByOrder(QList<Spec> &specs)
{
    std::stable_sort(specs.begin(), specs.end(), [](const Spec &a, const Spec &b) { return a.order < b.order; });
}

} // namespace

Extensions &Extensions::instance()
{
    static Extensions registry;
    return registry;
}

void Extensions::add(std::unique_ptr<Extension> extension)
{
    if (!extension || find(extension->id()))
        return;
    m_extensions.push_back(std::move(extension));
    m_snapshot = false;
}

QList<const Extension *> Extensions::all() const
{
    QList<const Extension *> out;
    for (const auto &e : m_extensions)
        out << e.get();
    return out;
}

const Extension *Extensions::find(const QString &id) const
{
    for (const auto &e : m_extensions) {
        if (e->id() == id)
            return e.get();
    }
    return nullptr;
}

bool Extensions::isEnabled(const QString &id) const
{
    return Settings::instance().value(enabledKey(id), kEnabledByDefault).toBool();
}

void Extensions::setEnabled(const QString &id, bool enabled)
{
    Settings::instance().setValue(enabledKey(id), enabled);
}

void Extensions::ensureSnapshot() const
{
    if (m_snapshot)
        return;
    m_active.clear();
    for (const auto &e : m_extensions) {
        if (isEnabled(e->id()))
            m_active << e.get();
    }
    m_snapshot = true;
}

QList<const Extension *> Extensions::active() const
{
    ensureSnapshot();
    return m_active;
}

bool Extensions::isActive(const QString &id) const
{
    ensureSnapshot();
    return std::any_of(m_active.cbegin(), m_active.cend(), [&](const Extension *e) { return e->id() == id; });
}

void Extensions::refresh()
{
    m_snapshot = false;
}

QList<PageSpec> Extensions::pages() const
{
    QList<PageSpec> out;
    for (const Extension *e : active())
        out << e->pages();
    sortByOrder(out);
    return out;
}

QList<PanelSpec> Extensions::panels() const
{
    QList<PanelSpec> out;
    for (const Extension *e : active())
        out << e->panels();
    sortByOrder(out);
    return out;
}

QList<SettingsTabSpec> Extensions::settingsTabs() const
{
    QList<SettingsTabSpec> out;
    for (const Extension *e : active())
        out << e->settingsTabs();
    sortByOrder(out);
    return out;
}

bool Extensions::hasPage(const QString &id) const
{
    for (const Extension *e : active()) {
        for (const PageSpec &p : e->pages()) {
            if (p.id == id)
                return true;
        }
    }
    return false;
}

} // namespace lumen::ui::ext
