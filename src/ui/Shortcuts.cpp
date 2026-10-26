#include "ui/Shortcuts.h"

#include "core/Settings.h"

#include <QAbstractButton>
#include <QAction>
#include <QWidget>

#include <algorithm>

namespace lumen::ui {

namespace {

QString settingsKey(const QString &id)
{
    return QStringLiteral("shortcuts/") + id;
}

QString cleanName(QString text)
{
    text.remove(u'&');
    if (text.endsWith(QLatin1String("...")))
        text.chop(3);
    if (text.endsWith(QChar(0x2026)))
        text.chop(1);
    return text.trimmed();
}

bool scopesConflict(const QString &a, const QString &b)
{
    return a == b || a == QLatin1String("global") || b == QLatin1String("global");
}

// Marks the proxies of shareAppWide(), so they are not counted as a window's own actions.
constexpr const char *kProxyProperty = "lumenAppWideProxy";

} // namespace

Shortcuts &Shortcuts::instance()
{
    static Shortcuts s;
    return s;
}

void Shortcuts::bind(const Info &info, QObject *owner, std::function<void(const QList<QKeySequence> &)> applyFn)
{
    if (!m_infos.contains(info.id))
        m_infos.insert(info.id, info);
    auto &list = m_bindings[info.id];
    list.removeIf([](const Binding &b) { return !b.owner; });
    list.append(Binding{owner, applyFn});
    applyFn(keys(info.id));
}

void Shortcuts::bindAction(QAction *action, const QString &id, const QString &category, const QString &scope,
                           QList<QKeySequence> defaults, const QString &name, bool tooltip)
{
    if (defaults.isEmpty())
        defaults = action->shortcuts();
    const QString label = name.isEmpty() ? cleanName(action->text()) : name;
    bind(Info{id, category, label, scope, defaults}, action, [action, label, tooltip](const QList<QKeySequence> &keys) {
        action->setShortcuts(keys);
        if (tooltip)
            action->setToolTip(keys.isEmpty() ? label : QStringLiteral("%1 (%2)").arg(label, toText(keys)));
    });
}

void Shortcuts::bindButton(QAbstractButton *button, const QString &id, const QString &category, const QString &scope,
                           const QKeySequence &defaultKey, const QString &name)
{
    bind(Info{id, category, name, scope, {defaultKey}}, button,
         [button](const QList<QKeySequence> &keys) { button->setShortcut(keys.value(0)); });
}

QList<Shortcuts::Info> Shortcuts::all() const
{
    QList<Info> list = m_infos.values();
    std::sort(list.begin(), list.end(), [](const Info &a, const Info &b) {
        return a.category != b.category ? a.category < b.category : a.name.localeAwareCompare(b.name) < 0;
    });
    return list;
}

QList<QKeySequence> Shortcuts::keys(const QString &id) const
{
    const Settings &s = Settings::instance();
    if (s.contains(settingsKey(id))) {
        QList<QKeySequence> list = QKeySequence::listFromString(s.value(settingsKey(id)).toString(), QKeySequence::PortableText);
        list.removeIf([](const QKeySequence &k) { return k.isEmpty(); }); // "" = the user removed every key
        return list;
    }
    return m_infos.value(id).defaults;
}

bool Shortcuts::isCustomized(const QString &id) const
{
    return Settings::instance().contains(settingsKey(id));
}

void Shortcuts::setKeys(const QString &id, const QList<QKeySequence> &newKeys)
{
    QList<QKeySequence> cleaned;
    for (const QKeySequence &k : newKeys) {
        if (!k.isEmpty() && !cleaned.contains(k))
            cleaned << k;
    }
    if (cleaned == m_infos.value(id).defaults)
        Settings::instance().remove(settingsKey(id));
    else
        Settings::instance().setValue(settingsKey(id), QKeySequence::listToString(cleaned, QKeySequence::PortableText));
    apply(id);
}

void Shortcuts::reset(const QString &id)
{
    Settings::instance().remove(settingsKey(id));
    apply(id);
}

void Shortcuts::resetAll()
{
    for (const QString &id : m_infos.keys()) {
        if (isCustomized(id))
            reset(id);
    }
}

QStringList Shortcuts::conflicts(const QString &id, const QKeySequence &key) const
{
    QStringList out;
    if (key.isEmpty())
        return out;
    const QString scope = m_infos.value(id).scope;
    for (auto it = m_infos.cbegin(); it != m_infos.cend(); ++it) {
        if (it.key() != id && scopesConflict(scope, it->scope) && keys(it.key()).contains(key))
            out << it.key();
    }
    out.sort();
    return out;
}

QString Shortcuts::toText(const QList<QKeySequence> &keys)
{
    QStringList parts;
    for (const QKeySequence &k : keys)
        parts << k.toString(QKeySequence::NativeText);
    return parts.join(QStringLiteral(", "));
}

void Shortcuts::addAppWide(QAction *action)
{
    // One command per name: a main window made again (the tests make several) replaces the one
    // before, or another window would get two actions with one key, and neither would work.
    const QString name = action->objectName();
    m_appWide.removeIf([action, &name](const QPointer<QAction> &a) {
        return a.isNull() || a == action || (!name.isEmpty() && a->objectName() == name);
    });
    m_appWide.append(action);
}

void Shortcuts::shareAppWide(QWidget *window)
{
    m_appWide.removeIf([](const QPointer<QAction> &a) { return a.isNull(); });
    for (const QPointer<QAction> &command : std::as_const(m_appWide)) {
        auto *proxy = new QAction(window);
        proxy->setObjectName(QStringLiteral("appWide-") + command->objectName());
        proxy->setProperty(kProxyProperty, true);
        proxy->setShortcutContext(Qt::WindowShortcut);
        window->addAction(proxy);
        const QPointer<QAction> target = command;
        connect(proxy, &QAction::triggered, command, [target] {
            if (target && target->isEnabled())
                target->trigger();
        });
        // The command's keys and state, without the keys of the window's own actions: two actions
        // with one key in a window are ambiguous, and neither would work.
        auto sync = [proxy, target, window] {
            if (!target)
                return;
            QList<QKeySequence> own;
            for (const QAction *a : window->findChildren<QAction *>()) {
                if (!a->property(kProxyProperty).toBool())
                    own << a->shortcuts();
            }
            for (const QAbstractButton *b : window->findChildren<QAbstractButton *>()) {
                if (!b->shortcut().isEmpty())
                    own << b->shortcut();
            }
            QList<QKeySequence> keys = target->shortcuts();
            keys.removeIf([&own](const QKeySequence &k) { return own.contains(k); });
            proxy->setShortcuts(keys);
            proxy->setEnabled(target->isEnabled());
        };
        connect(command, &QAction::changed, proxy, sync);
        connect(this, &Shortcuts::changed, proxy, sync);
        sync();
    }
}

void Shortcuts::apply(const QString &id)
{
    const QList<QKeySequence> current = keys(id);
    auto &list = m_bindings[id];
    list.removeIf([](const Binding &b) { return !b.owner; });
    for (const Binding &b : std::as_const(list))
        b.apply(current);
    emit changed(id);
}

} // namespace lumen::ui
