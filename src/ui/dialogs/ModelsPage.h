#pragma once

#include "runtime/Hardware.h"
#include "runtime/ModelLibrary.h"
#include "runtime/ServerProfile.h"

#include <QWidget>

#include <optional>

class QComboBox;
class QLabel;
class QLineEdit;
class QMenu;
class QPushButton;
class QTreeWidget;
class QVBoxLayout;

namespace lumen::ui {

class ElidedLabel;
class Studio;

// Settings → Models: Lumen's models folder (open, move), and every model file Lumen knows (its own
// folder plus folders the user adds), with what each file is, whether it fits the GPU, Civitai
// details and which servers use it.
class ModelsPage : public QWidget
{
    Q_OBJECT
public:
    explicit ModelsPage(Studio *studio, QWidget *parent = nullptr);

    // Fit of a model on this computer's GPU: "fits", "partial", "too large" or "" when unknown.
    static QString fitKey(const runtime::ModelEntry &e, const std::optional<runtime::HardwareInfo> &hw);
    static QString fitText(const QString &key);
    static QColor fitColor(const QString &key);

    // Moves Lumen's models folder to `target` after asking (Move… picks the folder first).
    void moveFolder(const QString &target);

private:
    void showFolder();
    void reload();
    void showDetails();
    void buildFoldersMenu();
    QString selectedPath() const;
    QStringList serversUsing(const QString &path) const;
    QStringList usedFiles() const;
    void useWithServer(const runtime::ModelEntry &e);
    void warnMissingCompanions(const runtime::ModelEntry &e);

    Studio *m_studio;
    std::optional<runtime::VramTier> vramTier() const;
    std::optional<runtime::HardwareInfo> m_hardware;
    QLineEdit *m_search;
    QComboBox *m_kind;
    QTreeWidget *m_list;
    QLabel *m_status;
    QPushButton *m_folders;
    ElidedLabel *m_folder;
    QWidget *m_details;
    QVBoxLayout *m_detailsLayout;
};

// Model option keys of server settings → kinds of model that fit them.
QList<runtime::ModelKind> modelKindsForOption(const QString &serverKind, const QString &optionKey);
// A menu of library models of the given kinds; choosing one calls pick with its path. With `fits`,
// the files it accepts come first and the others follow under a line saying they may not fit.
void fillModelMenu(QMenu *menu, runtime::ModelLibrary *library, const QList<runtime::ModelKind> &kinds,
                   const std::function<void(const QString &)> &pick, const std::function<bool(const runtime::ModelEntry &)> &fits = {});

} // namespace lumen::ui
