#include "ui/dialogs/UpdateDialog.h"

#include "runtime/Download.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QProgressBar>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace lumen::ui {

UpdateDialog::UpdateDialog(const runtime::AppRelease &release, bool canInstall, QWidget *parent)
    : QDialog(parent)
    , m_release(release)
{
    setWindowTitle(tr("Update Lumen Studio"));
    setMinimumSize(480, 380);
    auto *v = new QVBoxLayout(this);
    auto *title = new QLabel(tr("<b>Lumen Studio %1 is available.</b> You have %2.").arg(release.version, QApplication::applicationVersion()), this);
    title->setWordWrap(true);
    v->addWidget(title);

    auto *notes = new QTextBrowser(this);
    notes->setObjectName(QStringLiteral("updateNotes"));
    notes->setAccessibleName(tr("What is new"));
    notes->setOpenExternalLinks(true);
    notes->setMarkdown(release.notes.isEmpty() ? tr("This release has no notes.") : release.notes);
    v->addWidget(notes, 1);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("updateStatus"));
    m_status->setWordWrap(true);
    m_status->setProperty("role", QStringLiteral("muted"));
    m_status->setText(canInstall ? tr("Lumen Studio closes to install it, after asking about anything not saved. Your projects, settings and models stay.")
                                 : tr("Download it from the release page. Your projects, settings and models stay."));
    v->addWidget(m_status);

    m_progress = new QProgressBar(this);
    m_progress->setObjectName(QStringLiteral("updateProgress"));
    m_progress->setAccessibleName(tr("Download"));
    m_progress->hide();
    v->addWidget(m_progress);

    auto *buttons = new QDialogButtonBox(this);
    m_update = buttons->addButton(canInstall ? tr("Update now") : tr("Open download page"), QDialogButtonBox::AcceptRole);
    m_update->setObjectName(QStringLiteral("updateNow"));
    m_update->setDefault(true);
    m_skip = buttons->addButton(tr("Skip this version"), QDialogButtonBox::ActionRole);
    m_skip->setObjectName(QStringLiteral("updateSkip"));
    m_skip->setToolTip(tr("No more reminders about %1. Help → Check for updates still finds it.").arg(release.version));
    auto *later = buttons->addButton(tr("Later"), QDialogButtonBox::RejectRole);
    later->setObjectName(QStringLiteral("updateLater"));
    v->addWidget(buttons);

    connect(m_update, &QPushButton::clicked, this, [this, canInstall] {
        if (canInstall) {
            startDownload();
            return;
        }
        QDesktopServices::openUrl(m_release.page);
        QDialog::reject();
    });
    connect(m_skip, &QPushButton::clicked, this, [this] {
        m_skipped = true;
        QDialog::reject();
    });
    connect(later, &QPushButton::clicked, this, &UpdateDialog::reject);
}

UpdateDialog::~UpdateDialog() = default;

void UpdateDialog::reject()
{
    // Later or Esc during the download stops it; what came so far is not kept for the next time.
    if (m_download) {
        m_download->cancel();
        QFile::remove(runtime::FileDownload::partPath(m_download->spec().destination));
    }
    QDialog::reject();
}

void UpdateDialog::startDownload()
{
    const QDir folder(runtime::appUpdateFolder());
    QDir().mkpath(folder.path());
    runtime::DownloadSpec spec;
    spec.url = m_release.installer;
    spec.destination = folder.filePath(QStringLiteral("LumenStudio-%1-win64.exe").arg(m_release.version));
    spec.sha256 = m_release.installerSha256;
    spec.size = m_release.installerSize;
    // Only a file checked just now is run.
    QFile::remove(spec.destination);

    if (!m_network)
        m_network = new QNetworkAccessManager(this);
    m_update->setEnabled(false);
    m_skip->setEnabled(false);
    m_progress->setRange(0, 0);
    m_progress->show();
    m_status->setText(tr("Downloading…"));

    auto *download = new runtime::FileDownload(m_network, spec, this);
    m_download = download;
    connect(download, &runtime::FileDownload::progress, this, [this](qint64 received, qint64 total) {
        if (total > 0) {
            m_progress->setRange(0, 1000);
            m_progress->setValue(int(received * 1000 / total));
        }
    });
    connect(download, &runtime::FileDownload::finished, this, [this, download] {
        download->deleteLater();
        if (download->ok()) {
            m_installerPath = download->spec().destination;
            accept();
            return;
        }
        m_progress->hide();
        m_update->setEnabled(true);
        m_skip->setEnabled(true);
        m_status->setText(tr("The download did not work: %1").arg(download->errorString()));
    });
    download->start();
}

} // namespace lumen::ui
