#include "ui/dialogs/SettingsDialog.h"
#include "ui/dialogs/LocalServersPage.h"
#include "ui/dialogs/LocalSetupDialog.h"
#include "ui/dialogs/ModelsPage.h"
#include "ui/dialogs/ShortcutsPage.h"

#include "ai/Gate.h"
#include "ai/GenerationService.h"
#include "ai/ProviderRegistry.h"
#include "core/Async.h"
#include "core/Error.h"
#include "core/Paths.h"
#include "core/SecretStore.h"
#include "core/Settings.h"
#include "runtime/Catalog.h"
#include "runtime/ServerProfile.h"
#include "runtime/Supervisor.h"
#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/ext/Extension.h"
#include "ui/Theme.h"
#include "ui/widgets/Controls.h"
#include "ui/widgets/ProviderCombo.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QThread>
#include <QToolButton>
#include <QVBoxLayout>

namespace lumen::ui {

using F = ai::SettingField::Type;

// ---- ProviderForm ---------------------------------------------------------------------------------------

ProviderForm::ProviderForm(Studio *studio, ai::Provider *provider, QWidget *parent)
    : QWidget(parent)
    , m_studio(studio)
    , m_provider(provider)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    auto *content = new QWidget(scroll);
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);
    auto *v = new QVBoxLayout(content);
    v->setContentsMargins(16, 12, 16, 12);
    v->setSpacing(10);

    const ai::ProviderType &type = provider->type();
    auto *title = new QLabel(type.name, content);
    Theme::setRole(title, QStringLiteral("title"));
    v->addWidget(title);
    v->addWidget(mutedLabel(type.description, content));
    if (type.documentation.isValid()) {
        auto *docs = new QLabel(QStringLiteral("<a href='%1'>%2</a>").arg(type.documentation.toString(), tr("Documentation")), content);
        docs->setOpenExternalLinks(true);
        v->addWidget(docs);
    }

    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(10);
    m_name = new QLineEdit(provider->displayName(), content);
    form->addRow(tr("Name"), m_name);
    // Providers that talk to a server kind Lumen can run may be bound to one of its servers.
    QString serverKind;
    for (const runtime::RuntimeSpec &spec : runtime::Catalog::instance().runtimes) {
        if (runtime::providerTypeFor(spec.id) == type.id)
            serverKind = spec.id;
    }
    if (!serverKind.isEmpty()) {
        m_server = new QComboBox(content);
        m_server->addItem(tr("External server (URL below)"), QString());
        for (const runtime::ServerProfile &p : studio->servers()->profiles()) {
            if (studio->servers()->kindOf(p.id) == serverKind)
                m_server->addItem(Icons::get(QStringLiteral("server")), tr("%1 (started by Lumen)").arg(p.name), p.id);
        }
        m_server->setCurrentIndex(std::max(0, m_server->findData(provider->text(QStringLiteral("managed_server")))));
        m_server->setToolTip(tr("Servers are set up under Settings → Local servers."));
        form->addRow(tr("Server"), m_server);
    }
    QList<ai::SettingField> advanced;
    for (const ai::SettingField &f : type.fields) {
        if (f.advanced) {
            advanced << f;
            continue;
        }
        QWidget *w = createField(f, provider->setting(f.key));
        form->addRow(f.label + (f.required ? QStringLiteral(" *") : QString()), w);
    }
    v->addLayout(form);
    if (m_server) {
        auto syncUrl = [this] {
            const QString id = m_server->currentData().toString();
            const auto profile = m_studio->servers()->profile(id);
            auto *url = qobject_cast<QLineEdit *>(m_fields.value(QStringLiteral("url")));
            if (!url)
                return;
            url->setEnabled(!profile);
            if (profile)
                url->setText(runtime::providerUrl(m_studio->servers()->kindOf(id), profile->port));
        };
        connect(m_server, &QComboBox::currentIndexChanged, this, syncUrl);
        syncUrl();
    }
    if (!advanced.isEmpty()) {
        auto *box = new QGroupBox(tr("Advanced"), content);
        box->setCheckable(true);
        box->setChecked(false);
        auto *af = new QFormLayout(box);
        af->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto *inner = new QWidget(box);
        auto *innerForm = new QFormLayout(inner);
        innerForm->setContentsMargins(0, 0, 0, 0);
        innerForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        for (const ai::SettingField &f : advanced)
            innerForm->addRow(f.label, createField(f, provider->setting(f.key)));
        af->addRow(inner);
        inner->setVisible(false);
        connect(box, &QGroupBox::toggled, inner, &QWidget::setVisible);
        v->addWidget(box);
    }

    auto *buttons = new QHBoxLayout;
    auto *testBtn = new QPushButton(Icons::get(QStringLiteral("refresh")), tr("Test connection"), content);
    auto *saveBtn = new QPushButton(Icons::get(QStringLiteral("check")), tr("Save"), content);
    Theme::setRole(saveBtn, QStringLiteral("primary"));
    buttons->addWidget(testBtn);
    // ComfyUI's own workflows (was in the Help menu).
    if (type.id == QLatin1String("comfyui")) {
        auto *workflows = new QPushButton(Icons::get(QStringLiteral("folder")),
                                          QCoreApplication::translate("lumen::ui::MainWindow", "Open ComfyUI workflow folder"), content);
        workflows->setToolTip(tr("Workflows (.json, API format) put here are offered for this provider"));
        connect(workflows, &QPushButton::clicked, this, [] {
            const QString dir = paths::appData() + QStringLiteral("/workflows/comfyui");
            QDir().mkpath(dir);
            QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
        });
        buttons->addWidget(workflows);
    }
    buttons->addStretch();
    buttons->addWidget(saveBtn);
    v->addLayout(buttons);
    m_status = new QLabel(content);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(m_status);
    if (!SecretStore::isNativeBackend())
        v->addWidget(mutedLabel(tr("Note: API keys are stored in a private file because no system keychain is available."), content));
    v->addStretch();
    connect(testBtn, &QPushButton::clicked, this, &ProviderForm::test);
    connect(saveBtn, &QPushButton::clicked, this, &ProviderForm::save);
}

QWidget *ProviderForm::createField(const ai::SettingField &f, const QVariant &value)
{
    QWidget *w = nullptr;
    switch (f.type) {
    case F::Text:
    case F::Url: {
        auto *e = new QLineEdit(value.toString());
        e->setPlaceholderText(f.defaultValue.toString());
        w = e;
        break;
    }
    case F::Secret: {
        auto *e = new QLineEdit(value.toString());
        e->setEchoMode(QLineEdit::Password);
        e->setPlaceholderText(tr("not set"));
        QAction *toggle = e->addAction(Icons::get(QStringLiteral("eye")), QLineEdit::TrailingPosition);
        connect(toggle, &QAction::triggered, e, [e] {
            e->setEchoMode(e->echoMode() == QLineEdit::Password ? QLineEdit::Normal : QLineEdit::Password);
        });
        w = e;
        break;
    }
    case F::Integer: {
        auto *s = new QSpinBox;
        s->setRange(int(f.maximum > f.minimum ? f.minimum : 0), int(f.maximum > f.minimum ? f.maximum : 1000000));
        s->setValue(value.toInt());
        w = s;
        break;
    }
    case F::Number: {
        auto *s = new QDoubleSpinBox;
        s->setRange(f.maximum > f.minimum ? f.minimum : -1e9, f.maximum > f.minimum ? f.maximum : 1e9);
        s->setDecimals(3);
        s->setValue(value.toDouble());
        w = s;
        break;
    }
    case F::Boolean: {
        auto *c = new QCheckBox;
        c->setChecked(value.toBool());
        w = c;
        break;
    }
    case F::Choice: {
        auto *c = new QComboBox;
        c->addItems(f.choices);
        const int idx = c->findText(value.toString());
        if (idx >= 0)
            c->setCurrentIndex(idx);
        else if (!value.toString().isEmpty()) {
            c->addItem(value.toString());
            c->setCurrentIndex(c->count() - 1);
        }
        w = c;
        break;
    }
    case F::Model: {
        auto *row = new QWidget;
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        auto *c = new QComboBox(row);
        c->setEditable(true);
        c->addItems(f.choices);
        c->setCurrentText(value.toString());
        c->setMinimumWidth(240);
        c->lineEdit()->setPlaceholderText(f.key == QLatin1String("model") ? tr("server default") : QString());
        auto *refresh = iconButton(QStringLiteral("refresh"), tr("Load the list from the server"), row);
        h->addWidget(c, 1);
        h->addWidget(refresh);
        connect(refresh, &QToolButton::clicked, this, [this, key = f.key] { refreshModels(key); });
        m_fields.insert(f.key, c);
        if (!f.help.isEmpty())
            row->setToolTip(f.help);
        return row;
    }
    case F::MultilineText: {
        auto *e = new QPlainTextEdit(value.toString());
        e->setFixedHeight(80);
        w = e;
        break;
    }
    }
    if (!f.help.isEmpty())
        w->setToolTip(f.help);
    m_fields.insert(f.key, w);
    return w;
}

QString ProviderForm::name() const
{
    return m_name->text().trimmed();
}

QVariantMap ProviderForm::values() const
{
    QVariantMap out;
    if (!m_provider)
        return out;
    for (const ai::SettingField &f : m_provider->type().fields) {
        QWidget *w = m_fields.value(f.key);
        if (!w)
            continue;
        if (auto *e = qobject_cast<QLineEdit *>(w))
            out.insert(f.key, e->text().trimmed());
        else if (auto *s = qobject_cast<QSpinBox *>(w))
            out.insert(f.key, s->value());
        else if (auto *d = qobject_cast<QDoubleSpinBox *>(w))
            out.insert(f.key, d->value());
        else if (auto *c = qobject_cast<QCheckBox *>(w))
            out.insert(f.key, c->isChecked());
        else if (auto *cb = qobject_cast<QComboBox *>(w))
            out.insert(f.key, cb->currentText().trimmed());
        else if (auto *pt = qobject_cast<QPlainTextEdit *>(w))
            out.insert(f.key, pt->toPlainText());
    }
    if (m_server)
        out.insert(QStringLiteral("managed_server"), m_server->currentData().toString());
    return out;
}

void ProviderForm::refreshModels(const QString &key)
{
    if (!m_provider)
        return;
    // Apply the current (unsaved) form values temporarily so the request uses the typed URL/key.
    m_provider->configure(values());
    auto *combo = qobject_cast<QComboBox *>(m_fields.value(key));
    m_status->setText(tr("Loading..."));
    QPointer<ai::Provider> provider(m_provider);
    QFuture<QStringList> future = ai::gatedList(m_provider, [provider, key] {
        if (!provider)
            return QtFuture::makeReadyValueFuture(QStringList());
        return key == QLatin1String("model") ? provider->listModels() : provider->listOptions(key);
    });
    QPointer<ProviderForm> self(this);
    future.then(this, [self, combo](QStringList items) {
              if (!self || !combo)
                  return;
              const QString current = combo->currentText();
              combo->clear();
              combo->addItems(items);
              combo->setCurrentText(current.isEmpty() && !items.isEmpty() ? items.first() : current);
              self->m_status->setText(tr("%n option(s) found.", nullptr, int(items.size())));
              Theme::setRole(self->m_status, QStringLiteral("success"));
              combo->showPopup();
          })
        .onFailed(this, [self](const std::exception &e) {
            if (!self)
                return;
            self->m_status->setText(describeException(e));
            Theme::setRole(self->m_status, QStringLiteral("error"));
        });
}

void ProviderForm::test()
{
    if (!m_provider)
        return;
    m_provider->configure(values());
    m_status->setText(tr("Connecting..."));
    Theme::setRole(m_status, QStringLiteral("muted"));
    QPointer<ProviderForm> self(this);
    QPointer<ai::Provider> provider(m_provider);
    ai::gatedCall(m_provider, [provider] { return provider ? provider->testConnection() : QtFuture::makeReadyValueFuture(QString()); })
        .then(this, [self](QString msg) {
            if (!self)
                return;
            self->m_status->setText(QStringLiteral("✓ ") + msg);
            Theme::setRole(self->m_status, QStringLiteral("success"));
        })
        .onFailed(this, [self](const std::exception &e) {
            if (!self)
                return;
            self->m_status->setText(QStringLiteral("✗ ") + describeException(e));
            Theme::setRole(self->m_status, QStringLiteral("error"));
        });
}

void ProviderForm::save()
{
    if (!m_provider)
        return;
    const QVariantMap v = values();
    for (const ai::SettingField &f : m_provider->type().fields) {
        if (f.required && v.value(f.key).toString().isEmpty()) {
            m_status->setText(tr("'%1' is required.").arg(f.label));
            Theme::setRole(m_status, QStringLiteral("error"));
            return;
        }
    }
    m_studio->providers()->updateProvider(m_provider->instanceId(), name().isEmpty() ? m_provider->type().name : name(), v);
    m_status->setText(tr("Saved."));
    Theme::setRole(m_status, QStringLiteral("success"));
    emit saved();
}

// ---- SettingsDialog ----------------------------------------------------------------------------------------

SettingsDialog::SettingsDialog(Studio *studio, QWidget *parent)
    : QDialog(parent)
    , m_studio(studio)
{
    setWindowTitle(tr("Settings"));
    resize(1020, 700);
    auto *v = new QVBoxLayout(this);
    m_tabs = new QTabWidget(this);
    // Core tabs and the tabs of plugins that are on, by order (ext::SettingsTabSpec).
    struct Tab
    {
        int order;
        QString id;
        std::function<QWidget *()> create;
        QString icon;
        QString label;
    };
    QList<Tab> tabs = {
        {10, QStringLiteral("providers"), [this] { return buildProvidersTab(); }, QStringLiteral("server"), tr("Providers")},
        // "&&": a single "&" would turn the next letter into a mnemonic and disappear.
        {20, QStringLiteral("roles"), [this] { return buildRolesTab(); }, QStringLiteral("chat"),
         tr("Models & roles").replace(QLatin1Char('&'), QStringLiteral("&&"))},
        {30, QStringLiteral("servers"), [this, studio] { return new LocalServersPage(studio, this); }, QStringLiteral("server"), tr("Local servers")},
        {40, QStringLiteral("models"), [this, studio] { return new ModelsPage(studio, this); }, QStringLiteral("layers"), tr("Models")},
        {60, QStringLiteral("general"), [this] { return buildGeneralTab(); }, QStringLiteral("settings"), tr("General")},
        {70, QStringLiteral("shortcuts"), [this] { return new ShortcutsPage(this); }, QStringLiteral("text"), tr("Shortcuts")},
        {80, QStringLiteral("plugins"), [this] { return buildPluginsTab(); }, QStringLiteral("parts"), tr("Plugins")},
    };
    for (const ext::SettingsTabSpec &spec : ext::Extensions::instance().settingsTabs())
        tabs.append({spec.order, spec.id, [this, studio, create = spec.create] { return create(studio, this); }, spec.icon, spec.label});
    std::stable_sort(tabs.begin(), tabs.end(), [](const Tab &a, const Tab &b) { return a.order < b.order; });
    for (const Tab &tab : tabs)
        addTab(tab.id, tab.create(), tab.icon, tab.label);
    v->addWidget(m_tabs, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    v->addWidget(buttons);
}

void SettingsDialog::addTab(const QString &id, QWidget *page, const QString &icon, const QString &label)
{
    m_tabs->addTab(page, Icons::get(icon), label);
    m_tabIds << id;
}

void SettingsDialog::showTab(const QString &tab)
{
    if (const int index = int(m_tabIds.indexOf(tab)); index >= 0)
        m_tabs->setCurrentIndex(index);
}

QWidget *SettingsDialog::buildPluginsTab()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(16, 16, 16, 16);
    v->setSpacing(12);
    v->addWidget(mutedLabel(tr("Plugins add features to Lumen Studio. Switching one on or off takes effect the next time "
                               "Lumen Studio starts; what a plugin saved in a project is kept while it is off."),
                            w));
    ext::Extensions &registry = ext::Extensions::instance();
    if (registry.all().isEmpty())
        v->addWidget(mutedLabel(tr("No plugins are installed."), w));
    for (const ext::Extension *e : registry.all()) {
        auto *row = card(w);
        auto *rv = new QVBoxLayout(row);
        auto *box = new QCheckBox(e->name(), row);
        box->setObjectName(QStringLiteral("plugin-%1").arg(e->id()));
        box->setChecked(registry.isEnabled(e->id()));
        rv->addWidget(box);
        rv->addWidget(mutedLabel(e->description(), row));
        auto *restart = mutedLabel(QString(), row);
        rv->addWidget(restart);
        auto sync = [restart, id = e->id()] {
            const ext::Extensions &r = ext::Extensions::instance();
            const bool pending = r.isEnabled(id) != r.isActive(id);
            restart->setText(pending ? (r.isEnabled(id) ? tr("Turns on when Lumen Studio starts again.")
                                                        : tr("Turns off when Lumen Studio starts again."))
                                     : QString());
            restart->setVisible(pending);
        };
        sync();
        connect(box, &QCheckBox::toggled, row, [id = e->id(), sync](bool on) {
            ext::Extensions::instance().setEnabled(id, on);
            sync();
        });
        v->addWidget(row);
    }
    v->addStretch();
    return w;
}

QWidget *SettingsDialog::buildProvidersTab()
{
    auto *w = new QWidget;
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(0, 8, 0, 0);
    auto *left = new QVBoxLayout;
    m_list = new QListWidget(w);
    m_list->setFixedWidth(270);
    m_list->setIconSize(QSize(18, 18));
    left->addWidget(m_list, 1);
    auto *bar = new QHBoxLayout;
    auto *add = new QPushButton(Icons::get(QStringLiteral("plus")), tr("Add provider"), w);
    Theme::setRole(add, QStringLiteral("primary"));
    auto *menu = new QMenu(add);
    auto addSection = [this, menu](const QString &title, ai::ProviderKind kind) {
        menu->addSection(title);
        for (const ai::ProviderType &t : m_studio->providers()->types(kind)) {
            QAction *a = menu->addAction(Icons::get(t.local ? QStringLiteral("server") : QStringLiteral("cloud")), t.name, this,
                                         [this, id = t.id] { addProvider(id); });
            a->setToolTip(t.description);
        }
    };
    addSection(tr("Image generation"), ai::ProviderKind::Image);
    addSection(tr("Language models"), ai::ProviderKind::Llm);
    menu->setToolTipsVisible(true);
    add->setMenu(menu);
    auto *detect = new QPushButton(Icons::get(QStringLiteral("target")), tr("Detect local"), w);
    detect->setToolTip(tr("Search for SD WebUI, ComfyUI, llama.cpp, Ollama, LM Studio and KoboldCpp on this computer"));
    connect(detect, &QPushButton::clicked, this, [this] {
        OnboardingDialog dlg(m_studio, this);
        dlg.exec();
        reloadProviders();
    });
    bar->addWidget(add);
    bar->addWidget(detect);
    left->addLayout(bar);
    auto *bar2 = new QHBoxLayout;
    auto *enable = new QCheckBox(tr("Enabled"), w);
    auto *remove = new QPushButton(Icons::get(QStringLiteral("trash")), tr("Remove"), w);
    Theme::setRole(remove, QStringLiteral("danger"));
    bar2->addWidget(enable);
    bar2->addStretch();
    bar2->addWidget(remove);
    left->addLayout(bar2);
    h->addLayout(left);
    auto *right = new QWidget(w);
    m_formHost = new QVBoxLayout(right);
    m_formHost->setContentsMargins(0, 0, 0, 0);
    h->addWidget(right, 1);

    connect(m_list, &QListWidget::currentItemChanged, this, [this, enable](QListWidgetItem *item) {
        const QString id = item ? item->data(Qt::UserRole).toString() : QString();
        QSignalBlocker b(enable);
        enable->setChecked(!id.isEmpty() && m_studio->providers()->isEnabled(id));
        showProvider(id);
    });
    connect(enable, &QCheckBox::toggled, this, [this](bool on) {
        if (QListWidgetItem *item = m_list->currentItem()) {
            const QString id = item->data(Qt::UserRole).toString();
            m_studio->providers()->setEnabled(id, on);
            reloadProviders(id);
        }
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        QListWidgetItem *item = m_list->currentItem();
        if (!item)
            return;
        const QString id = item->data(Qt::UserRole).toString();
        if (QMessageBox::question(this, tr("Remove provider"), tr("Remove '%1' and its stored API key?").arg(item->text())) == QMessageBox::Yes) {
            m_studio->providers()->removeProvider(id);
            reloadProviders();
        }
    });
    reloadProviders();
    return w;
}

void SettingsDialog::reloadProviders(const QString &select)
{
    const QString current = select.isEmpty() && m_list->currentItem() ? m_list->currentItem()->data(Qt::UserRole).toString() : select;
    QSignalBlocker b(m_list);
    m_list->clear();
    for (ai::Provider *p : m_studio->providers()->providers()) {
        auto *item = new QListWidgetItem(Icons::get(p->kind() == ai::ProviderKind::Image ? QStringLiteral("image") : QStringLiteral("chat")),
                                         p->displayName(), m_list);
        item->setData(Qt::UserRole, p->instanceId());
        item->setToolTip(p->type().name);
        if (!m_studio->providers()->isEnabled(p->instanceId()))
            item->setForeground(Theme::palette().textMuted);
        if (p->instanceId() == current)
            m_list->setCurrentItem(item);
    }
    b.unblock();
    if (!m_list->currentItem() && m_list->count() > 0)
        m_list->setCurrentRow(0);
    else
        showProvider(m_list->currentItem() ? m_list->currentItem()->data(Qt::UserRole).toString() : QString());
}

void SettingsDialog::showProvider(const QString &id)
{
    if (m_form)
        m_form->deleteLater();
    ai::Provider *p = m_studio->providers()->provider(id);
    if (!p) {
        auto *empty = new QWidget;
        auto *v = new QVBoxLayout(empty);
        v->addStretch();
        auto *title = new QLabel(tr("No provider selected"), empty);
        Theme::setRole(title, QStringLiteral("title"));
        title->setAlignment(Qt::AlignCenter);
        v->addWidget(title);
        // The core needs only an image engine; language models are for some plugins.
        auto *hint = mutedLabel(tr("Add an image engine to change pictures with words (Help → Set up AI… helps with that). A language "
                                   "model is only needed by some plugins."), empty);
        hint->setAlignment(Qt::AlignCenter);
        v->addWidget(hint);
        v->addStretch();
        m_form = empty;
    } else {
        auto *form = new ProviderForm(m_studio, p);
        connect(form, &ProviderForm::saved, this, [this, id] { reloadProviders(id); });
        m_form = form;
    }
    m_formHost->addWidget(m_form);
}

void SettingsDialog::addProvider(const QString &typeId)
{
    const auto type = m_studio->providers()->type(typeId);
    if (!type)
        return;
    ai::Provider *p = m_studio->providers()->addProvider(typeId, type->name, {});
    if (p)
        reloadProviders(p->instanceId());
}

QWidget *SettingsDialog::buildRolesTab()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(16, 16, 16, 16);
    v->setSpacing(12);
    v->addWidget(mutedLabel(tr("Choose which provider handles each task. Roles without an assignment fall back to the chat assistant."), w));
    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setVerticalSpacing(12);
    auto *image = new ProviderCombo(m_studio->providers(), ai::ProviderKind::Image, w);
    if (ai::ImageProvider *p = m_studio->providers()->defaultImageProvider())
        image->setProviderId(p->instanceId());
    connect(image, &ProviderCombo::providerChanged, this, [this](const QString &id) {
        if (!id.isEmpty())
            m_studio->providers()->setDefaultImageProvider(id);
    });
    form->addRow(tr("Default image generator"), image);
    for (ai::LlmRole role : ai::allLlmRoles()) {
        auto *row = new QWidget(w);
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        auto *combo = new ProviderCombo(m_studio->providers(), ai::ProviderKind::Llm, row);
        auto *model = new QComboBox(row);
        model->setEditable(true);
        model->setMinimumWidth(240);
        model->lineEdit()->setPlaceholderText(tr("provider default model"));
        const ai::RoleBinding binding = m_studio->providers()->role(role);
        if (!binding.providerId.isEmpty())
            combo->setProviderId(binding.providerId);
        model->setCurrentText(binding.model);
        auto *refresh = iconButton(QStringLiteral("refresh"), tr("Load models"), row);
        h->addWidget(combo, 1);
        h->addWidget(model, 1);
        h->addWidget(refresh);
        auto store = [this, role, combo, model] {
            m_studio->providers()->setRole(role, ai::RoleBinding{combo->providerId(), model->currentText().trimmed()});
        };
        connect(combo, &ProviderCombo::providerChanged, this, store);
        connect(model, &QComboBox::currentTextChanged, this, store);
        connect(refresh, &QToolButton::clicked, this, [this, combo, model] {
            if (ai::LlmProvider *p = combo->llmProvider()) {
                ai::gatedList(p, [p = QPointer<ai::LlmProvider>(p)] { return p ? p->listModels() : QtFuture::makeReadyValueFuture(QStringList()); }).then(this, [model](QStringList items) {
                    const QString cur = model->currentText();
                    model->clear();
                    model->addItem(QString());
                    model->addItems(items);
                    model->setCurrentText(cur);
                    model->showPopup();
                }).onFailed(this, [this](const std::exception &e) { m_studio->notify(describeException(e), QStringLiteral("error")); });
            }
        });
        form->addRow(ai::llmRoleLabel(role), row);
    }
    v->addLayout(form);
    v->addWidget(mutedLabel(tr("Tip: reference analysis needs a vision-capable model (e.g. Qwen2.5-VL / Gemma 3 in llama.cpp or Ollama, "
                               "GPT-4.1, Claude, Gemini). Shot planning benefits from a strong reasoning model."), w));
    v->addStretch();
    return w;
}

QWidget *SettingsDialog::buildGeneralTab()
{
    auto *w = new QWidget;
    auto *form = new QFormLayout(w);
    form->setContentsMargins(16, 16, 16, 16);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setVerticalSpacing(12);
    Settings &s = Settings::instance();
    auto *theme = new QComboBox(w);
    theme->addItem(tr("Automatic (high contrast when Windows uses it)"), QStringLiteral("auto"));
    theme->addItem(tr("Dark"), QStringLiteral("dark"));
    theme->addItem(tr("Light"), QStringLiteral("light"));
    theme->addItem(tr("High contrast"), QStringLiteral("high-contrast"));
    theme->setCurrentIndex(std::max(0, theme->findData(s.theme())));
    connect(theme, &QComboBox::currentIndexChanged, this, [theme] {
        Settings::instance().setValue(QStringLiteral("ui/theme"), theme->currentData().toString());
    });
    form->addRow(tr("Theme (restart)"), theme);
    auto *language = new QComboBox(w);
    language->addItem(tr("System default"), QString());
    language->addItem(QStringLiteral("English"), QStringLiteral("en"));
    for (const QString &file : QDir(QStringLiteral(":/i18n")).entryList({QStringLiteral("lumen_*.qm")})) {
        const QString code = file.mid(6, file.size() - 9); // lumen_<code>.qm
        if (code == QLatin1String("en"))
            continue; // English plural forms only
        const QLocale locale(code);
        QString name = locale.nativeLanguageName();
        if (!name.isEmpty())
            name[0] = name[0].toUpper();
        language->addItem(name.isEmpty() ? code : name, code);
    }
    language->setCurrentIndex(std::max(0, language->findData(s.language())));
    connect(language, &QComboBox::currentIndexChanged, this, [language] {
        Settings::instance().setValue(QStringLiteral("ui/language"), language->currentData().toString());
    });
    form->addRow(tr("Language (restart)"), language);
    auto *threads = new QSpinBox(w);
    threads->setRange(0, 64);
    threads->setSpecialValueText(tr("Automatic (%1)").arg(std::max(2, QThread::idealThreadCount() - 1)));
    threads->setValue(s.value(QStringLiteral("perf/workerThreads"), 0).toInt());
    connect(threads, &QSpinBox::valueChanged, this, [](int v) {
        Settings::instance().setValue(QStringLiteral("perf/workerThreads"), v);
        async::setMaxThreads(Settings::instance().workerThreads());
    });
    form->addRow(tr("Worker threads"), threads);
    auto *autosave = new QSpinBox(w);
    autosave->setRange(10, 3600);
    autosave->setSuffix(tr(" s"));
    autosave->setValue(s.autosaveIntervalSec());
    connect(autosave, &QSpinBox::valueChanged, this, [](int v) { Settings::instance().setValue(QStringLiteral("doc/autosaveSec"), v); });
    form->addRow(tr("Autosave interval"), autosave);
    auto *gpu = new QCheckBox(tr("Use the GPU for the canvas (restart)"), w);
    gpu->setChecked(s.value(QStringLiteral("perf/gpuCanvas"), true).toBool());
    connect(gpu, &QCheckBox::toggled, this, [](bool v) { Settings::instance().setValue(QStringLiteral("perf/gpuCanvas"), v); });
    form->addRow(QString(), gpu);
    auto *safe = new QCheckBox(tr("Safe for work: keep nudity and explicit content out of generated images"), w);
    safe->setToolTip(tr("Adds \"%1\" to the negative prompt of every image request where the model takes one. "
                        "Also in the status bar (shield).")
                         .arg(ai::GenerationService::safeNegativeTerms()));
    safe->setChecked(s.value(QStringLiteral("generation/safeContent"), false).toBool());
    connect(safe, &QCheckBox::toggled, this, [](bool v) { Settings::instance().setValue(QStringLiteral("generation/safeContent"), v); });
    form->addRow(QString(), safe);
    return w;
}

// ---- OnboardingDialog ----------------------------------------------------------------------------------------

OnboardingDialog::OnboardingDialog(Studio *studio, QWidget *parent)
    : QDialog(parent)
    , m_studio(studio)
{
    setWindowTitle(tr("Set up AI"));
    resize(620, 520);
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(24, 20, 24, 20);
    v->setSpacing(12);
    auto *logo = new QLabel(this);
    logo->setPixmap(Icons::pixmap(QStringLiteral("logo"), 44, Theme::palette().accent));
    v->addWidget(logo);
    auto *title = new QLabel(tr("Set up an image engine"), this);
    Theme::setRole(title, QStringLiteral("title"));
    v->addWidget(title);
    v->addWidget(mutedLabel(tr("Lumen changes pictures with an image engine: a model on this computer (Lumen can install one) or an "
                               "online service such as fal.ai, OpenAI or Gemini. Image servers already running here are found "
                               "automatically."), this));
    m_status = mutedLabel(tr("Searching..."), this);
    v->addWidget(m_status);
    auto *list = new QWidget(this);
    m_found = new QVBoxLayout(list);
    m_found->setContentsMargins(0, 0, 0, 0);
    v->addWidget(list);
    v->addStretch();
    auto *buttons = new QHBoxLayout;
    auto *again = new QPushButton(Icons::get(QStringLiteral("refresh")), tr("Search again"), this);
    auto *cloud = new QPushButton(Icons::get(QStringLiteral("cloud")), tr("Use an online service…"), this);
    cloud->setToolTip(tr("Add a service with your key: fal.ai, OpenAI, Gemini, Stability AI… (Settings → Providers)"));
    auto *local = new QPushButton(Icons::get(QStringLiteral("sparkles")), tr("Run on this computer…"), this);
    local->setToolTip(tr("Download free models and run them on this computer"));
    auto *done = new QPushButton(tr("Done"), this);
    Theme::setRole(done, QStringLiteral("primary"));
    buttons->addWidget(again);
    buttons->addWidget(cloud);
    buttons->addWidget(local);
    buttons->addStretch();
    buttons->addWidget(done);
    v->addLayout(buttons);
    connect(again, &QPushButton::clicked, this, &OnboardingDialog::detect);
    connect(local, &QPushButton::clicked, this, [this] {
        LocalSetupDialog dlg(m_studio, this);
        dlg.exec();
        detect();
    });
    connect(done, &QPushButton::clicked, this, &QDialog::accept);
    connect(cloud, &QPushButton::clicked, this, [this] {
        SettingsDialog dlg(m_studio, this);
        dlg.showTab(QStringLiteral("providers"));
        dlg.exec();
    });
    detect();
}

void OnboardingDialog::detect()
{
    while (QLayoutItem *item = m_found->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_status->setText(tr("Searching for local servers..."));
    QPointer<OnboardingDialog> self(this);
    m_studio->providers()->detectLocalServers().then(this, [self](QList<ai::DetectedServer> found) {
        if (!self)
            return;
        // Image servers only: the editor needs no language model (plugins add theirs in Settings).
        found.removeIf([&self](const ai::DetectedServer &s) {
            const auto type = self->m_studio->providers()->type(s.typeId);
            return !type || type->kind != ai::ProviderKind::Image;
        });
        if (found.isEmpty()) {
            self->m_status->setText(tr("No image server is running on this computer. Lumen can install one (Run on this computer…), "
                                       "or use an online service."));
            return;
        }
        self->m_status->setText(tr("Found %n server(s):", nullptr, int(found.size())));
        for (const ai::DetectedServer &s : found) {
            auto *row = card(self);
            auto *h = new QHBoxLayout(row);
            auto *label = new QLabel(QStringLiteral("<b>%1</b><br><span style='color:%4'>%2 %3</span>")
                                         .arg(s.name, s.url.toString(), s.detail, Theme::palette().textMuted.name()), row);
            auto *add = new QPushButton(tr("Add"), row);
            Theme::setRole(add, QStringLiteral("primary"));
            bool exists = false;
            for (ai::Provider *p : self->m_studio->providers()->providers()) {
                if (p->type().id == s.typeId && p->text(QStringLiteral("url")).startsWith(s.url.toString()))
                    exists = true;
            }
            if (exists) {
                add->setText(tr("Added"));
                add->setEnabled(false);
            }
            h->addWidget(label, 1);
            h->addWidget(add);
            self->m_found->addWidget(row);
            QObject::connect(add, &QPushButton::clicked, self, [self, s, add] {
                QVariantMap settings;
                QString url = s.url.toString();
                if (s.typeId == QLatin1String("llamacpp") || s.typeId == QLatin1String("ollama") || s.typeId == QLatin1String("lmstudio")
                    || s.typeId == QLatin1String("koboldcpp"))
                    url += QStringLiteral("/v1");
                settings.insert(QStringLiteral("url"), url);
                if (ai::Provider *p = self->m_studio->providers()->addProvider(s.typeId, s.name, settings)) {
                    add->setText(tr("Added"));
                    add->setEnabled(false);
                    self->m_studio->notify(tr("%1 added.").arg(p->displayName()), QStringLiteral("success"));
                }
            });
        }
    });
}

} // namespace lumen::ui
