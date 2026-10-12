#pragma once

#include "runtime/Hardware.h"
#include "runtime/ModelHub.h"

#include <QDialog>

#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;

namespace lumen::ui {

class Studio;

// Settings → Models → Download: searches Hugging Face and Civitai, shows a model's files with
// their precision, size and GPU fit, and downloads the chosen file (with the vision projector of a
// language model, and every part of a split GGUF) into Lumen's models folder through the Queue.
class ModelHubDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ModelHubDialog(Studio *studio, QWidget *parent = nullptr);

    void setSource(runtime::HubSource source);
    void setCategory(const QString &category);
    void setSearchText(const QString &text);
    void search(bool more = false);
    // Index of the file Lumen suggests: the primary Civitai file, or for GGUF models the
    // best-quality common quantisation that fits the GPU.
    static int recommendedFile(const QList<runtime::HubFile> &files, const std::optional<runtime::HardwareInfo> &hw);

private:
    runtime::HubSource source() const;
    void updateSourceControls();
    void showModel();
    void showVersion();
    void downloadSelected();
    void editAccounts();
    void loadPreview(const QUrl &url, const QString &cacheKey);
    const runtime::HubVersion *currentVersion() const;

    Studio *m_studio;
    std::optional<runtime::HardwareInfo> m_hw;
    QComboBox *m_source;
    QComboBox *m_category;
    QLineEdit *m_search;
    QCheckBox *m_nsfw;
    QTreeWidget *m_results;
    QPushButton *m_more;
    QLabel *m_status;
    QWidget *m_details;
    QLabel *m_title;
    QLabel *m_meta;
    QLabel *m_notice;
    QLabel *m_description;
    QLabel *m_preview;
    QComboBox *m_version;
    QTreeWidget *m_files;
    QCheckBox *m_projector;
    QCheckBox *m_companions;    // the model's VAE / text encoder (Civitai)
    void showCompanions();
    QPushButton *m_download;
    QList<runtime::HubModel> m_models;
    std::optional<runtime::HubModel> m_current;
    QString m_cursor;
    int m_searchSerial = 0;
    int m_detailSerial = 0;
};

// Access tokens for Hugging Face (gated repositories) and Civitai (most downloads), kept in the
// system credential store.
class HubAccountsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit HubAccountsDialog(QWidget *parent = nullptr);

private:
    void save();

    QLineEdit *m_hf;
    QLineEdit *m_civitai;
    bool m_forgetHf = false;
    bool m_forgetCivitai = false;
};

} // namespace lumen::ui
