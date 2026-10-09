#pragma once

#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QObject>
#include <QPointer>

#include <functional>

class QAbstractButton;
class QAction;

namespace lumen::ui {

// Central registry of customisable keyboard shortcuts. Every shortcut-bearing action binds to it by
// id; the registry remembers the defaults, applies user overrides (persisted in the settings under
// "shortcuts/<id>") and reports conflicts. Scopes: shortcuts in the same scope, or in the "global"
// scope, cannot share a key; different pages ("editor", "parts", ...) can.
class Shortcuts : public QObject
{
    Q_OBJECT
public:
    struct Info
    {
        QString id;
        QString category; // display group, e.g. "Editor tools"
        QString name;
        QString scope; // "global", "editor", "parts", "generate"...
        QList<QKeySequence> defaults;
    };

    static Shortcuts &instance();

    // Registers a shortcut (first registration wins for the metadata) and applies its current keys
    // through `apply` now and after every change, for as long as `owner` lives.
    void bind(const Info &info, QObject *owner, std::function<void(const QList<QKeySequence> &)> apply);
    // Convenience: binds an action; empty defaults take the action's current shortcuts. With
    // `tooltip` the tool tip shows "<name> (<keys>)".
    void bindAction(QAction *action, const QString &id, const QString &category, const QString &scope,
                    QList<QKeySequence> defaults = {}, const QString &name = QString(), bool tooltip = false);
    void bindButton(QAbstractButton *button, const QString &id, const QString &category, const QString &scope,
                    const QKeySequence &defaultKey, const QString &name);

    QList<Info> all() const; // sorted by category, then name
    bool contains(const QString &id) const { return m_infos.contains(id); }
    Info info(const QString &id) const { return m_infos.value(id); }
    QList<QKeySequence> keys(const QString &id) const;
    bool isCustomized(const QString &id) const;
    void setKeys(const QString &id, const QList<QKeySequence> &keys);
    void reset(const QString &id);
    void resetAll();
    // Ids (other than `id`) whose keys include `key` within a conflicting scope.
    QStringList conflicts(const QString &id, const QKeySequence &key) const;

    // Text of a key list for display ("Ctrl+Shift+Z, Ctrl+Y").
    static QString toText(const QList<QKeySequence> &keys);

signals:
    void changed(const QString &id);

private:
    Shortcuts() = default;
    void apply(const QString &id);

    struct Binding
    {
        QPointer<QObject> owner;
        std::function<void(const QList<QKeySequence> &)> apply;
    };
    QHash<QString, Info> m_infos;
    QHash<QString, QList<Binding>> m_bindings;
};

} // namespace lumen::ui
