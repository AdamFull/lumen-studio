#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QStringList>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace lumen {
class Job;
}

namespace lumen::ui {

class Studio;

// Develop settings applied to picture files (plan M20 step 8): the settings in the Develop tab or a
// preset, the files, and the folder the graded copies go to. Each picture is graded at full size on
// the graphics card and saved as a copy in its own format; the files chosen stay as they are. Runs
// as a Queue job.
class BatchDevelopDialog : public QDialog
{
    Q_OBJECT
public:
    BatchDevelopDialog(Studio *studio, const QJsonObject &current, QWidget *parent = nullptr);

    void addFiles(const QStringList &files);
    void setFolder(const QString &folder);
    // "" for the Develop tab's settings, else a preset's shown name.
    void choosePreset(const QString &name);
    QJsonObject grading() const;
    QStringList files() const;
    QString folder() const;
    // Starts the job (what Apply does); null when there is nothing to do.
    Job *apply();

    // The job itself, for others (and tests).
    static Job *start(Studio *studio, const QJsonObject &grading, const QStringList &files, const QString &folder);

private:
    void updateState();

    Studio *m_studio;
    QJsonObject m_current;
    QComboBox *m_settings = nullptr;
    QListWidget *m_files = nullptr;
    QLineEdit *m_folder = nullptr;
    QPushButton *m_apply = nullptr;
    QPushButton *m_remove = nullptr;
    bool m_folderChosen = false;
};

} // namespace lumen::ui
