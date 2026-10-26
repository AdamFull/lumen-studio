#include "ui/dialogs/AiSettingsPage.h"

#include "ai/ModelDefaults.h"
#include "ai/ProviderRegistry.h"
#include "core/Error.h"
#include "runtime/Supervisor.h"
#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/dialogs/SetupWizard.h"
#include "ui/ext/Extension.h"
#include "ui/widgets/Controls.h"

#include <QActionGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <utility>

namespace lumen::ui {

AiSettingsPage::AiSettingsPage(Studio *studio, QWidget *parent)
    : QWidget(parent)
    , m_studio(studio)
{
    auto *content = new QWidget;
    auto *v = new QVBoxLayout(content);
    v->setContentsMargins(16, 16, 16, 16);
    v->setSpacing(10);
    v->addWidget(mutedLabel(tr("What makes and changes your images. Change… picks another engine you have set up, or sets up a new one."),
                            content));
    m_image = makeRow(QStringLiteral("aiImage"), tr("Image engine"), tr("Makes new images and changes them from your descriptions."));
    m_edit = makeRow(QStringLiteral("aiEdit"), tr("Engine for changes"),
                     tr("Changes a selection or the whole image: colorize, remove, replace. Usually the image engine; choose another "
                        "one when it is better at following such instructions."));
    m_language = makeRow(QStringLiteral("aiLanguage"), tr("Language model"), QString());
    m_languageUsers = m_language.box->findChild<QLabel *>(QStringLiteral("aiLanguageText"));
    for (const Row *row : {&m_image, &m_edit, &m_language})
        v->addWidget(row->box);
    auto *setupRow = new QHBoxLayout;
    auto *setup = new QPushButton(Icons::get(QStringLiteral("sparkles")), tr("Set up AI…"), content);
    setup->setObjectName(QStringLiteral("aiSetUp"));
    connect(setup, &QPushButton::clicked, this, &AiSettingsPage::openWizard);
    setupRow->addWidget(setup);
    setupRow->addWidget(mutedLabel(tr("Every setting of each engine is on the Providers, Local servers and Models tabs."), content), 1);
    v->addLayout(setupRow);
    v->addStretch();
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(verticalScroll(content, this));

    connect(m_image.menu, &QMenu::aboutToShow, this, [this] { fillImageMenu(m_image.menu, false); });
    connect(m_edit.menu, &QMenu::aboutToShow, this, [this] { fillImageMenu(m_edit.menu, true); });
    connect(m_language.menu, &QMenu::aboutToShow, this, [this] { fillLanguageMenu(m_language.menu); });
    connect(m_studio->providers(), &ai::ProviderRegistry::providersChanged, this, [this] {
        m_checked.clear(); // settings may have changed: check again
        refresh();
    });
    connect(m_studio->providers(), &ai::ProviderRegistry::rolesChanged, this, &AiSettingsPage::refresh);
    connect(m_studio->servers(), &runtime::ServerSupervisor::stateChanged, this, &AiSettingsPage::refresh);
    connect(m_studio->servers(), &runtime::ServerSupervisor::profilesChanged, this, &AiSettingsPage::refresh);
    refresh();
}

void AiSettingsPage::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    if (!std::exchange(m_shown, true))
        refresh();
}

AiSettingsPage::Row AiSettingsPage::makeRow(const QString &id, const QString &title, const QString &text)
{
    Row row;
    row.box = card();
    row.box->setObjectName(id);
    auto *h = new QHBoxLayout(row.box);
    h->setContentsMargins(14, 10, 14, 10);
    h->setSpacing(14);
    auto *about = new QVBoxLayout;
    auto *heading = new QLabel(title, row.box);
    Theme::setRole(heading, QStringLiteral("heading"));
    about->addWidget(heading);
    auto *description = mutedLabel(text, row.box);
    description->setObjectName(id + QStringLiteral("Text"));
    about->addWidget(description);
    about->addStretch();
    h->addLayout(about, 2);
    auto *now = new QVBoxLayout;
    row.engine = new ElidedLabel(QString(), row.box);
    row.engine->setObjectName(id + QStringLiteral("Engine"));
    QFont bold = row.engine->font();
    bold.setBold(true);
    row.engine->setFont(bold);
    now->addWidget(row.engine);
    row.model = new ElidedLabel(QString(), row.box);
    row.model->setObjectName(id + QStringLiteral("Model"));
    now->addWidget(row.model);
    row.status = new QLabel(row.box);
    row.status->setObjectName(id + QStringLiteral("Status"));
    row.status->setWordWrap(true);
    now->addWidget(row.status);
    now->addStretch();
    h->addLayout(now, 3);
    row.change = new QPushButton(tr("Change…"), row.box);
    row.change->setObjectName(id + QStringLiteral("Change"));
    row.menu = new QMenu(row.change);
    row.change->setMenu(row.menu);
    h->addWidget(row.change, 0, Qt::AlignTop);
    return row;
}

QString AiSettingsPage::modelOf(ai::Provider *p, bool edits) const
{
    const ai::ModelDefaults preset = ai::modelDefaultsOf(p);
    if (preset.isValid())
        return preset.name;
    if (edits && !p->text(QStringLiteral("edit_model")).isEmpty())
        return p->text(QStringLiteral("edit_model"));
    if (!p->text(QStringLiteral("model")).isEmpty())
        return p->text(QStringLiteral("model"));
    return p->type().local ? tr("the one loaded on the server") : QString();
}

QString AiSettingsPage::statusOf(ai::Provider *p, bool *good)
{
    *good = false;
    const QString id = p->instanceId();
    if (!m_studio->providers()->isEnabled(id))
        return tr("Turned off (Providers tab)");
    const QString server = p->text(QStringLiteral("managed_server"));
    if (!server.isEmpty() && m_studio->servers()->profile(server)) {
        runtime::ServerSupervisor *servers = m_studio->servers();
        switch (servers->state(server)) {
        case runtime::ServerState::Ready:
            *good = true;
            return tr("Running on this computer");
        case runtime::ServerState::Starting:
            *good = true;
            return tr("Starting…");
        case runtime::ServerState::Stopping:
            return tr("Stopping…");
        case runtime::ServerState::Failed:
            return tr("Did not start: %1").arg(servers->statusText(server));
        case runtime::ServerState::Stopped:
            *good = true;
            return tr("On this computer; starts when it is needed");
        }
    }
    // Other engines: one connection check per engine while the page is open.
    if (m_checked.contains(id)) {
        *good = m_reachable.value(id);
        return m_checked.value(id);
    }
    if (m_shown && !m_checking.contains(id)) {
        m_checking.insert(id);
        QPointer<AiSettingsPage> self(this);
        p->testConnection()
            .then(this, [self, id](const QString &) {
                if (!self)
                    return;
                self->m_checking.remove(id);
                self->m_checked.insert(id, tr("Connected"));
                self->m_reachable.insert(id, true);
                self->refresh();
            })
            .onFailed(this, [self, id](const std::exception &e) {
                if (!self)
                    return;
                self->m_checking.remove(id);
                self->m_checked.insert(id, tr("Not reachable: %1").arg(describeException(e)));
                self->m_reachable.insert(id, false);
                self->refresh();
            });
    }
    *good = true;
    return tr("Checking…");
}

void AiSettingsPage::fill(Row &row, ai::Provider *provider, const QString &none, bool edits)
{
    if (!provider) {
        row.engine->setFullText(none);
        row.model->setFullText(QString());
        row.model->setVisible(false);
        row.status->setText(tr("Not set up"));
        Theme::setRole(row.status, QStringLiteral("error"));
        return;
    }
    row.engine->setFullText(provider->displayName());
    const QString model = modelOf(provider, edits);
    row.model->setFullText(model.isEmpty() ? QString() : tr("Model: %1").arg(model));
    row.model->setVisible(!model.isEmpty());
    bool good = false;
    row.status->setText(statusOf(provider, &good));
    Theme::setRole(row.status, good ? QStringLiteral("muted") : QStringLiteral("error"));
}

void AiSettingsPage::refresh()
{
    ai::ProviderRegistry *registry = m_studio->providers();
    ai::ImageProvider *image = registry->defaultImageProvider();
    fill(m_image, image, tr("No image engine yet"), false);
    m_image.change->setText(image ? tr("Change…") : tr("Set up…"));

    const QString editId = registry->editImageProviderId();
    ai::ImageProvider *edit = editId.isEmpty() ? nullptr : registry->imageProvider(editId);
    if (edit && registry->isEnabled(editId)) {
        fill(m_edit, edit, QString(), true);
    } else {
        fill(m_edit, image, tr("No image engine yet"), true);
        if (image)
            m_edit.engine->setFullText(tr("The image engine (%1)").arg(image->displayName()));
    }
    m_edit.change->setEnabled(!registry->imageProviders().isEmpty());

    const QStringList needing = ext::Extensions::instance().needingLanguageModel();
    m_language.box->setVisible(!needing.isEmpty());
    if (!needing.isEmpty()) {
        if (m_languageUsers)
            m_languageUsers->setText(tr("Used by %1 (plugins). The editor itself works without one.").arg(needing.join(QStringLiteral(", "))));
        QString model;
        ai::LlmProvider *llm = registry->llmFor(ai::LlmRole::Assistant, &model);
        fill(m_language, llm, tr("No language model"), false);
        if (llm && !model.isEmpty()) {
            m_language.model->setFullText(tr("Model: %1").arg(model));
            m_language.model->setVisible(true);
        }
        if (!llm) {
            m_language.status->setText(tr("Not set up; the plugins' assistant and planning do not work without one."));
            Theme::setRole(m_language.status, QStringLiteral("muted"));
        }
    }
}

void AiSettingsPage::fillImageMenu(QMenu *menu, bool edits)
{
    menu->clear();
    ai::ProviderRegistry *registry = m_studio->providers();
    auto *group = new QActionGroup(menu);
    if (edits) {
        QAction *same = menu->addAction(tr("The image engine"));
        same->setCheckable(true);
        same->setChecked(registry->editImageProviderId().isEmpty());
        group->addAction(same);
        connect(same, &QAction::triggered, this, [this] { m_studio->providers()->setEditImageProvider(QString()); });
        menu->addSeparator();
    }
    const QString current = edits ? registry->editImageProviderId()
                                  : (registry->defaultImageProvider() ? registry->defaultImageProvider()->instanceId() : QString());
    for (ai::ImageProvider *p : registry->imageProviders()) {
        const QString id = p->instanceId();
        const QString model = modelOf(p, edits);
        QAction *a = menu->addAction(model.isEmpty() ? p->displayName() : QStringLiteral("%1 — %2").arg(p->displayName(), model));
        a->setCheckable(true);
        a->setChecked(id == current);
        a->setEnabled(registry->isEnabled(id));
        group->addAction(a);
        connect(a, &QAction::triggered, this, [this, id, edits] {
            if (edits)
                m_studio->providers()->setEditImageProvider(id);
            else
                m_studio->providers()->setDefaultImageProvider(id);
        });
    }
    if (!registry->imageProviders().isEmpty())
        menu->addSeparator();
    menu->addAction(Icons::get(QStringLiteral("sparkles")), tr("Set up a new engine…"), this, &AiSettingsPage::openWizard);
}

void AiSettingsPage::fillLanguageMenu(QMenu *menu)
{
    menu->clear();
    ai::ProviderRegistry *registry = m_studio->providers();
    auto *group = new QActionGroup(menu);
    const QString current = registry->llmFor(ai::LlmRole::Assistant) ? registry->llmFor(ai::LlmRole::Assistant)->instanceId() : QString();
    for (ai::LlmProvider *p : registry->llmProviders()) {
        const QString id = p->instanceId();
        QAction *a = menu->addAction(p->displayName());
        a->setCheckable(true);
        a->setChecked(id == current);
        a->setEnabled(registry->isEnabled(id));
        group->addAction(a);
        connect(a, &QAction::triggered, this, [this, id] { m_studio->providers()->setRole(ai::LlmRole::Assistant, {id, QString()}); });
    }
    if (!registry->llmProviders().isEmpty())
        menu->addSeparator();
    menu->addAction(tr("Models for each task…"), this, [this] { emit tabRequested(QStringLiteral("roles")); });
    menu->addAction(Icons::get(QStringLiteral("server")), tr("Run one on this computer…"), this, [this] { emit tabRequested(QStringLiteral("servers")); });
}

void AiSettingsPage::openWizard()
{
    openSetupWizard(m_studio, window());
}

} // namespace lumen::ui
