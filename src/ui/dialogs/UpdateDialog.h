#pragma once

#include "runtime/AppUpdate.h"

#include <QDialog>
#include <QPointer>

class QLabel;
class QNetworkAccessManager;
class QProgressBar;
class QPushButton;

namespace lumen::runtime {
class FileDownload;
}

namespace lumen::ui {

// Says what a new version brings and gets it. Accepted when the installer is downloaded and its
// checksum is right: installerPath() then says where it is, and the caller closes Lumen and runs it.
class UpdateDialog : public QDialog
{
    Q_OBJECT
public:
    // Without `canInstall` (the portable version, a build folder, a release without an installer)
    // it offers the download page instead.
    UpdateDialog(const runtime::AppRelease &release, bool canInstall, QWidget *parent = nullptr);
    ~UpdateDialog() override;

    QString installerPath() const { return m_installerPath; }
    bool skipped() const { return m_skipped; }

protected:
    void reject() override;

private:
    void startDownload();

    runtime::AppRelease m_release;
    QLabel *m_status = nullptr;
    QProgressBar *m_progress = nullptr;
    QPushButton *m_update = nullptr;
    QPushButton *m_skip = nullptr;
    QNetworkAccessManager *m_network = nullptr;
    QPointer<runtime::FileDownload> m_download;
    QString m_installerPath;
    bool m_skipped = false;
};

} // namespace lumen::ui
