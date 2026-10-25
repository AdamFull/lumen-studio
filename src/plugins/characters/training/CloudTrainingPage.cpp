#include "plugins/characters/training/CloudTrainingPage.h"

#include "core/Error.h"
#include "core/SecretStore.h"
#include "core/Settings.h"
#include "runtime/RunPod.h"
#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/widgets/Controls.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace lumen::ui {

namespace {

constexpr int kMinGpuMemoryGb = runtime::kCloudTrainingMinGpuGb;

} // namespace

CloudTrainingPage::CloudTrainingPage(Studio *studio, QWidget *parent)
    : QWidget(parent)
    , m_studio(studio)
{
    auto *v = new QVBoxLayout(this);
    auto *intro = mutedLabel(tr("Train character LoRAs on a rented RunPod GPU: about half an hour instead of hours on a laptop. Lumen starts "
                                "the GPU, uploads the training set, trains with the same settings as on this computer, downloads the LoRA "
                                "and shuts the GPU down. RunPod bills your account by the minute. Create an API key at "
                                "<a href='https://www.runpod.io/console/user/settings'>runpod.io → Settings → API Keys</a>."),
                             this);
    intro->setOpenExternalLinks(true);
    intro->setTextFormat(Qt::RichText);
    v->addWidget(intro);

    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto *keyRow = new QHBoxLayout;
    m_key = new QLineEdit(this);
    m_key->setEchoMode(QLineEdit::Password);
    m_saveKey = new QPushButton(tr("Save"), this);
    m_forgetKey = new QPushButton(Icons::get(QStringLiteral("trash")), QString(), this);
    m_forgetKey->setToolTip(tr("Forget the saved key"));
    auto *test = new QPushButton(tr("Test"), this);
    test->setToolTip(tr("Ask RunPod for its GPUs with this key"));
    keyRow->addWidget(m_key, 1);
    keyRow->addWidget(m_saveKey);
    keyRow->addWidget(m_forgetKey);
    keyRow->addWidget(test);
    form->addRow(tr("RunPod API key"), keyRow);
    form->addRow(QString(), mutedLabel(SecretStore::isNativeBackend()
                                           ? tr("Kept in the Windows Credential Manager and only sent to RunPod.")
                                           : tr("Kept in a private file in Lumen's data folder and only sent to RunPod."),
                                       this));

    m_cloud = new QComboBox(this);
    m_cloud->addItem(tr("Secure cloud (data centres, reliable)"), false);
    m_cloud->addItem(tr("Community cloud (cheaper, less reliable)"), true);
    m_cloud->setCurrentIndex(Settings::instance().value(QStringLiteral("cloud/runpod/community"), false).toBool() ? 1 : 0);
    form->addRow(tr("Cloud"), m_cloud);

    auto *gpuRow = new QHBoxLayout;
    m_gpu = new QComboBox(this);
    m_gpu->setMinimumWidth(320);
    auto *refresh = new QToolButton(this);
    refresh->setIcon(Icons::get(QStringLiteral("refresh")));
    refresh->setToolTip(tr("Load GPUs and prices from RunPod"));
    gpuRow->addWidget(m_gpu, 1);
    gpuRow->addWidget(refresh);
    form->addRow(tr("GPU"), gpuRow);
    v->addLayout(form);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::RichText);
    v->addWidget(m_status);
    v->addStretch();

    connect(m_saveKey, &QPushButton::clicked, this, &CloudTrainingPage::saveKey);
    connect(m_key, &QLineEdit::returnPressed, this, &CloudTrainingPage::saveKey);
    connect(m_forgetKey, &QPushButton::clicked, this, &CloudTrainingPage::forgetKey);
    connect(test, &QPushButton::clicked, this, &CloudTrainingPage::refreshGpus);
    connect(refresh, &QToolButton::clicked, this, &CloudTrainingPage::refreshGpus);
    connect(m_cloud, &QComboBox::currentIndexChanged, this, [this] {
        Settings::instance().setValue(QStringLiteral("cloud/runpod/community"), m_cloud->currentData().toBool());
        refreshGpus();
    });
    connect(m_gpu, &QComboBox::activated, this, [this] {
        Settings::instance().setValue(QStringLiteral("cloud/runpod/gpu"), m_gpu->currentData().toString());
    });
    updateKeyState();
    // Without a catalogue yet, the saved choice is still shown.
    const QString saved = Settings::instance().value(QStringLiteral("cloud/runpod/gpu")).toString();
    if (!saved.isEmpty())
        m_gpu->addItem(saved, saved);
    if (m_studio->runPod()->hasKey())
        refreshGpus();
}

void CloudTrainingPage::updateKeyState()
{
    const bool saved = m_studio->runPod()->hasKey();
    m_key->setPlaceholderText(saved ? tr("Saved — type a new one to replace it") : tr("Not set"));
    m_forgetKey->setEnabled(saved);
}

void CloudTrainingPage::saveKey()
{
    const QString key = m_key->text().trimmed();
    if (key.isEmpty())
        return;
    if (!SecretStore::write(runtime::RunPodClient::apiKeyName(), key)) {
        QMessageBox::warning(this, tr("Cloud training"), tr("Cannot save the key: %1").arg(SecretStore::lastError()));
        return;
    }
    m_key->clear();
    updateKeyState();
    refreshGpus();
}

void CloudTrainingPage::forgetKey()
{
    SecretStore::remove(runtime::RunPodClient::apiKeyName());
    updateKeyState();
    m_status->setText(tr("The key is forgotten."));
}

void CloudTrainingPage::refreshGpus()
{
    const bool community = m_cloud->currentData().toBool();
    m_status->setText(tr("Asking RunPod…"));
    QPointer<CloudTrainingPage> self(this);
    m_studio->runPod()
        ->gpus()
        .then(this,
              [self, community](const QList<runtime::RunPodGpu> &gpus) {
                  if (!self)
                      return;
                  const QString saved = Settings::instance().value(QStringLiteral("cloud/runpod/gpu")).toString();
                  self->m_gpu->clear();
                  for (const runtime::RunPodGpu &g : gpus) {
                      if (g.memoryGb < kMinGpuMemoryGb || !g.offered(community) || !g.runsTrainer())
                          continue;
                      self->m_gpu->addItem(tr("%1 · %2 GB · $%3/h").arg(g.name).arg(g.memoryGb).arg(g.price(community), 0, 'f', 2), g.id);
                  }
                  if (self->m_gpu->count() == 0) {
                      self->m_status->setText(tr("RunPod offers no GPU with %1 GB or more in this cloud right now.").arg(kMinGpuMemoryGb));
                      return;
                  }
                  const int index = self->m_gpu->findData(saved);
                  self->m_gpu->setCurrentIndex(std::max(0, index));
                  if (index < 0)
                      Settings::instance().setValue(QStringLiteral("cloud/runpod/gpu"), self->m_gpu->currentData().toString());
                  self->m_status->setText(QStringLiteral("<span style='color:%1'>%2</span>")
                                              .arg(Theme::palette().success.name(),
                                                   tr("Connected: %n GPU type(s) with %1 GB or more.", nullptr, self->m_gpu->count())
                                                       .arg(kMinGpuMemoryGb)
                                                       .toHtmlEscaped()));
              })
        .onFailed(this, [self](const std::exception &e) {
            if (self)
                self->m_status->setText(QStringLiteral("<span style='color:%1'>%2</span>")
                                            .arg(Theme::palette().danger.name(), QString::fromUtf8(e.what()).toHtmlEscaped()));
        });
}

} // namespace lumen::ui
