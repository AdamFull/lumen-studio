#pragma once

#include <QKeySequence>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <vector>

class QWidget;

namespace lumen::ui {

class Studio;

namespace ext {

// What a plugin adds to the application. The core never includes a plugin header: plugins link
// against the core, register an Extension at start (Extensions::add), and the core asks the
// registry for the contributions of the plugins that are switched on.

// A page of its own (Create, Poses, Parts): an action in the Tools menu opens it in a window of its
// own, built the first time it is opened. The main window is always the editor.
struct PageSpec
{
    QString id;      // "poses": Studio::showPage, the --page option
    QString icon;    // Icons name; the menu action's shortcut is "nav.<icon>" (ids of earlier versions)
    QString label;
    QString tooltip;
    int order = 100; // position in the Tools menu
    // Default key, the same whichever other plugins are on (Alt+1 create, Alt+4 poses, Alt+5 parts;
    // Alt+2 and Alt+6 are New from prompt and Browse project images).
    QKeySequence shortcut;
    std::function<QWidget *(Studio *studio, QWidget *parent)> create;
};

// A panel over the editor, on the right (the assistant), switched on and off from the View menu.
// The panel is created when first shown; `close` hides it and unchecks the menu action.
struct PanelSpec
{
    QString id;
    QString icon;
    QString label;
    QString tooltip;
    QString shortcutId;      // optional: bound through Shortcuts under "General"
    QKeySequence shortcut;
    int order = 100;
    std::function<QWidget *(Studio *studio, QWidget *parent, std::function<void()> close)> create;
};

// A tab in the Settings dialog.
struct SettingsTabSpec
{
    QString id;      // SettingsDialog::showTab / MainWindow::openSettings
    QString icon;
    QString label;
    int order = 100; // the core tabs: ai 5, providers 10, roles 20, servers 30, models 40, general 60,
                     // shortcuts 70, plugins 80
    std::function<QWidget *(Studio *studio, QWidget *parent)> create;
};

class Extension
{
public:
    virtual ~Extension() = default;
    virtual QString id() const = 0;
    virtual QString name() const = 0;
    virtual QString description() const = 0;

    virtual QList<PageSpec> pages() const { return {}; }
    virtual QList<PanelSpec> panels() const { return {}; }
    virtual QList<SettingsTabSpec> settingsTabs() const { return {}; }
    // The plugin's features need a language model (the core editor does not): the setup wizard
    // offers one and Settings → AI lists it while such a plugin is on.
    virtual bool needsLanguageModel() const { return false; }
};

// The registry. Which plugins are on is read once (the first query, or refresh()); switching one
// takes effect at the next start, because the menus and panels are built with the main window.
class Extensions
{
public:
    static Extensions &instance();

    // Registers a plugin; a second plugin with the same id is ignored.
    void add(std::unique_ptr<Extension> extension);
    QList<const Extension *> all() const;
    const Extension *find(const QString &id) const;

    // Stored in Settings ("plugins/<id>/enabled"). Off by default: the editor works without any
    // of them (plan M17).
    bool isEnabled(const QString &id) const;
    void setEnabled(const QString &id, bool enabled);
    static constexpr bool kEnabledByDefault = false;

    // The plugins that are on for this session.
    QList<const Extension *> active() const;
    bool isActive(const QString &id) const;
    // Re-reads which plugins are on (start-up after registration, tests).
    void refresh();

    // Contributions of the active plugins, sorted by order.
    QList<PageSpec> pages() const;
    QList<PanelSpec> panels() const;
    QList<SettingsTabSpec> settingsTabs() const;
    bool hasPage(const QString &id) const;
    // Names of the active plugins that need a language model (Extension::needsLanguageModel).
    QStringList needingLanguageModel() const;

private:
    void ensureSnapshot() const;

    std::vector<std::unique_ptr<Extension>> m_extensions;
    mutable QList<const Extension *> m_active;
    mutable bool m_snapshot = false;
};

} // namespace ext
} // namespace lumen::ui
