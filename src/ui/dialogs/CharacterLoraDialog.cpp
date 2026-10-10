#include "ui/dialogs/CharacterLoraDialog.h"

#include "ai/Provider.h"
#include "ai/ProviderRegistry.h"
#include "runtime/ModelFormats.h"
#include "runtime/ModelLibrary.h"
#include "runtime/ServerProfile.h"
#include "runtime/Supervisor.h"
#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/widgets/Controls.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace lumen::ui {

namespace {

std::optional<runtime::ServerProfile> managedSdServer(Studio *studio, ai::ImageProvider *provider)
{
    if (!provider)
        return std::nullopt;
    const QString id = provider->text(QStringLiteral("managed_server"));
    if (id.isEmpty() || studio->servers()->kindOf(id) != QLatin1String("sd.cpp"))
        return std::nullopt;
    return studio->servers()->profile(id);
}

bool insideFolder(const QString &file, const QString &folder)
{
    if (folder.isEmpty())
        return false;
    const QString f = QDir::cleanPath(QFileInfo(file).absoluteFilePath());
    const QString d = QDir::cleanPath(QFileInfo(folder).absoluteFilePath());
    return f.startsWith(d + QLatin1Char('/'), Qt::CaseInsensitive);
}

std::optional<runtime::ModelEntry> libraryEntry(Studio *studio, const QString &file)
{
    if (auto e = studio->models()->entry(QDir::cleanPath(QDir::fromNativeSeparators(file))))
        return e;
    return std::nullopt;
}

} // namespace

QString modelFamily(Studio *studio, const QString &file)
{
    if (file.isEmpty())
        return {};
    if (const auto e = libraryEntry(studio, file))
        return e->info.family;
    return QFileInfo::exists(file) ? runtime::inspectModel(file).family : QString();
}

QString checkpointFamily(Studio *studio, ai::ImageProvider *provider)
{
    if (!provider)
        return {};
    if (const auto p = managedSdServer(studio, provider)) {
        const QString model = p->option(QStringLiteral("model"), QStringLiteral("sd.cpp")).toString();
        if (!model.isEmpty())
            return modelFamily(studio, model);
        return modelFamily(studio, p->option(QStringLiteral("diffusion_model"), QStringLiteral("sd.cpp")).toString());
    }
    // Other servers name their checkpoint; look for a file of that name in the library.
    const QString name = QFileInfo(QDir::fromNativeSeparators(provider->text(QStringLiteral("model")))).fileName();
    if (name.isEmpty())
        return {};
    for (const runtime::ModelEntry &e : studio->models()->entries()) {
        if (QFileInfo(e.path).fileName().compare(name, Qt::CaseInsensitive) == 0)
            return e.info.family;
    }
    return {};
}

QString suggestedTrigger(Studio *studio, const QString &file)
{
    if (const auto e = libraryEntry(studio, file)) {
        if (!e->info.triggerWords.isEmpty())
            return e->info.triggerWords.first().trimmed();
        const QJsonArray trained = e->civitai.value(QStringLiteral("trainedWords")).toArray();
        if (!trained.isEmpty())
            return trained.first().toString().trimmed();
    }
    if (QFileInfo::exists(file)) {
        const runtime::ModelInfo info = runtime::inspectModel(file);
        if (!info.triggerWords.isEmpty())
            return info.triggerWords.first().trimmed();
    }
    return {};
}

LoraCheck checkCharacterLora(Studio *studio, ai::ImageProvider *provider, const ai::CharacterLora &lora)
{
    LoraCheck c;
    if (!lora.isValid() || !provider)
        return c;
    if (!provider->capabilities().loras) {
        c.problem = CharacterLoraDialog::tr("%1 cannot use LoRAs; only the references keep the character.").arg(provider->displayName());
        return c;
    }
    const bool local = QFileInfo::exists(lora.file);
    const auto server = managedSdServer(studio, provider);
    if (!local && server) {
        c.problem = CharacterLoraDialog::tr("The LoRA file is missing: %1").arg(QDir::toNativeSeparators(lora.file));
        return c;
    }
    const QString family = lora.family.isEmpty() ? modelFamily(studio, lora.file) : lora.family;
    const QString checkpoint = checkpointFamily(studio, provider);
    if (!family.isEmpty() && !checkpoint.isEmpty() && family != checkpoint) {
        c.problem = CharacterLoraDialog::tr("The LoRA is for %1 models, but %2 uses a %3 model; it will not work there.")
                        .arg(family, provider->displayName(), checkpoint);
        return c;
    }
    if (server) {
        const QString folder = server->option(QStringLiteral("lora_dir"), QStringLiteral("sd.cpp")).toString();
        if (!insideFolder(lora.file, folder)) {
            c.problem = folder.isEmpty() ? CharacterLoraDialog::tr("The server %1 has no LoRA folder yet.").arg(server->name)
                                         : CharacterLoraDialog::tr("The LoRA is not in the LoRA folder of %1 (%2).")
                                               .arg(server->name, QDir::toNativeSeparators(folder));
            c.fixableFolder = true;
        }
    }
    return c;
}

bool useLoraFolder(Studio *studio, ai::ImageProvider *provider, const QString &loraFile)
{
    auto server = managedSdServer(studio, provider);
    if (!server || loraFile.isEmpty())
        return false;
    server->options.insert(QStringLiteral("lora_dir"), QDir::toNativeSeparators(QFileInfo(loraFile).absolutePath()));
    studio->servers()->updateProfile(*server);
    if (studio->servers()->state(server->id) != runtime::ServerState::Stopped)
        studio->servers()->restart(server->id);
    return true;
}

// ---- CharacterLoraDialog ----------------------------------------------------------------------------

CharacterLoraDialog::CharacterLoraDialog(Studio *studio, ai::ImageProvider *provider, const ai::CharacterLora &current, QWidget *parent)
    : QDialog(parent)
    , m_studio(studio)
    , m_provider(provider)
{
    setWindowTitle(tr("Character LoRA"));
    resize(560, 300);
    auto *v = new QVBoxLayout(this);
    v->addWidget(mutedLabel(tr("A LoRA trained on this character keeps its face, hair, outfit and colours the same in every image. "
                               "Its trigger word goes first in every prompt."),
                            this));
    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto *fileRow = new QHBoxLayout;
    m_file = new QComboBox(this);
    m_file->setMinimumWidth(320);
    for (const runtime::ModelEntry &e : studio->models()->entries(runtime::ModelKind::Lora)) {
        QString label = e.name();
        if (!e.info.family.isEmpty())
            label += QStringLiteral("  —  ") + e.info.family;
        m_file->addItem(label, QDir::toNativeSeparators(e.path));
        m_file->setItemData(m_file->count() - 1, QDir::toNativeSeparators(e.path), Qt::ToolTipRole);
    }
    if (current.isValid() && m_file->findData(QDir::toNativeSeparators(current.file)) < 0)
        m_file->addItem(QFileInfo(current.file).completeBaseName(), QDir::toNativeSeparators(current.file));
    auto *browse = new QPushButton(Icons::get(QStringLiteral("folder")), tr("Browse…"), this);
    fileRow->addWidget(m_file, 1);
    fileRow->addWidget(browse);
    form->addRow(tr("LoRA file"), fileRow);
    m_trigger = new QLineEdit(this);
    m_trigger->setPlaceholderText(tr("e.g. lmn_akane"));
    form->addRow(tr("Trigger word"), m_trigger);
    m_weight = new QDoubleSpinBox(this);
    m_weight->setRange(0.05, 2.0);
    m_weight->setSingleStep(0.05);
    m_weight->setDecimals(2);
    m_weight->setValue(current.isValid() ? current.weight : 0.8);
    m_weight->setToolTip(tr("How strongly the LoRA applies; 0.6–1.0 usually works. Lower it when images look stiff or copy the "
                            "training pictures."));
    form->addRow(tr("Weight"), m_weight);
    m_family = mutedLabel(QString(), this);
    form->addRow(tr("Made for"), m_family);
    v->addLayout(form);
    m_check = new QLabel(this);
    m_check->setWordWrap(true);
    m_check->setTextFormat(Qt::RichText);
    v->addWidget(m_check);
    v->addStretch();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    QPushButton *attach = buttons->addButton(current.isValid() ? tr("Save") : tr("Attach"), QDialogButtonBox::AcceptRole);
    Theme::setRole(attach, QStringLiteral("primary"));
    if (current.isValid()) {
        QPushButton *detach = buttons->addButton(tr("Detach"), QDialogButtonBox::DestructiveRole);
        connect(detach, &QPushButton::clicked, this, [this] {
            m_detached = true;
            accept();
        });
    }
    v->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(this, tr("Choose a LoRA"), QFileInfo(m_file->currentData().toString()).absolutePath(),
                                                          tr("LoRA files (*.safetensors)"));
        if (file.isEmpty())
            return;
        const QString native = QDir::toNativeSeparators(file);
        int index = m_file->findData(native);
        if (index < 0) {
            m_file->addItem(QFileInfo(file).completeBaseName(), native);
            index = m_file->count() - 1;
        }
        m_file->setCurrentIndex(index);
    });
    connect(m_file, &QComboBox::currentIndexChanged, this, &CharacterLoraDialog::fileChanged);
    connect(m_weight, &QDoubleSpinBox::valueChanged, this, &CharacterLoraDialog::updateCheck);
    connect(m_check, &QLabel::linkActivated, this, [this](const QString &) {
        if (useLoraFolder(m_studio, m_provider, lora().file))
            updateCheck();
    });

    if (current.isValid())
        m_file->setCurrentIndex(std::max(0, m_file->findData(QDir::toNativeSeparators(current.file))));
    attach->setEnabled(m_file->count() > 0);
    connect(m_file, &QComboBox::currentIndexChanged, attach, [this, attach] { attach->setEnabled(m_file->currentIndex() >= 0); });
    fileChanged();
    if (current.isValid())
        m_trigger->setText(current.trigger);
    if (m_file->count() == 0)
        m_check->setText(tr("No LoRAs in the model library yet. Download one under Settings → Models, or browse to a file."));
}

ai::CharacterLora CharacterLoraDialog::lora() const
{
    ai::CharacterLora l;
    l.file = QDir::fromNativeSeparators(m_file->currentData().toString());
    l.trigger = m_trigger->text().trimmed();
    l.weight = m_weight->value();
    l.family = modelFamily(m_studio, l.file);
    return l;
}

void CharacterLoraDialog::fileChanged()
{
    const QString file = QDir::fromNativeSeparators(m_file->currentData().toString());
    const QString family = modelFamily(m_studio, file);
    m_family->setText(family.isEmpty() ? tr("unknown model family") : family);
    // Replace the trigger word unless the user typed their own.
    const QString suggested = suggestedTrigger(m_studio, file);
    if (m_trigger->text().isEmpty() || m_trigger->text() == m_lastSuggested)
        m_trigger->setText(suggested);
    m_lastSuggested = suggested;
    updateCheck();
}

void CharacterLoraDialog::updateCheck()
{
    if (m_file->count() == 0)
        return;
    const LoraCheck c = checkCharacterLora(m_studio, m_provider, lora());
    if (c.problem.isEmpty()) {
        m_check->setText(m_provider ? QStringLiteral("<span style='color:%1'>%2</span>")
                                          .arg(Theme::palette().success.name(), tr("Works with %1.").arg(m_provider->displayName().toHtmlEscaped()))
                                    : QString());
        return;
    }
    QString text = QStringLiteral("<span style='color:%1'>%2</span>").arg(Theme::palette().warning.name(), c.problem.toHtmlEscaped());
    if (c.fixableFolder)
        text += QStringLiteral(" <a href='folder'>%1</a>").arg(tr("Use the LoRA's folder"));
    m_check->setText(text);
}

} // namespace lumen::ui
