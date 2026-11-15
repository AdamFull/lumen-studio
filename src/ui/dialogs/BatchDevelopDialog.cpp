#include "ui/dialogs/BatchDevelopDialog.h"

#include "core/Async.h"
#include "core/Job.h"
#include "core/JobQueue.h"
#include "doc/ImageFile.h"
#include "ui/Grading.h"
#include "ui/Studio.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include <atomic>

namespace lumen::ui {

BatchDevelopDialog::BatchDevelopDialog(Studio *studio, const QJsonObject &current, QWidget *parent)
    : QDialog(parent)
    , m_studio(studio)
    , m_current(current)
{
    setObjectName(QStringLiteral("batchDevelopDialog"));
    setWindowTitle(tr("Apply to picture files"));
    resize(560, 460);
    auto *v = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("Each picture is graded at full size and saved as a copy in its own format; the files you choose "
                                "stay as they are. The camera's details (Exif) are not copied."),
                             this);
    intro->setWordWrap(true);
    v->addWidget(intro);

    auto *form = new QFormLayout;
    m_settings = new QComboBox(this);
    m_settings->setObjectName(QStringLiteral("batchSettings"));
    m_settings->addItem(tr("The Develop tab's settings now"), QString());
    for (const grading::Preset &p : grading::presets())
        m_settings->addItem(p.name, p.name);
    form->addRow(tr("Settings"), m_settings);
    v->addLayout(form);

    auto *filesLabel = new QLabel(tr("Pictures"), this);
    v->addWidget(filesLabel);
    m_files = new QListWidget(this);
    m_files->setObjectName(QStringLiteral("batchFiles"));
    m_files->setSelectionMode(QAbstractItemView::ExtendedSelection);
    filesLabel->setBuddy(m_files);
    v->addWidget(m_files, 1);
    auto *fileButtons = new QHBoxLayout;
    auto *add = new QPushButton(tr("Add pictures…"), this);
    m_remove = new QPushButton(tr("Remove"), this);
    fileButtons->addWidget(add);
    fileButtons->addWidget(m_remove);
    fileButtons->addStretch();
    v->addLayout(fileButtons);

    auto *folderRow = new QHBoxLayout;
    m_folder = new QLineEdit(this);
    m_folder->setObjectName(QStringLiteral("batchFolder"));
    auto *choose = new QPushButton(tr("Choose…"), this);
    folderRow->addWidget(m_folder, 1);
    folderRow->addWidget(choose);
    auto *folderForm = new QFormLayout;
    folderForm->addRow(tr("Save the copies in"), folderRow);
    v->addLayout(folderForm);

    auto *box = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_apply = box->addButton(tr("Apply"), QDialogButtonBox::AcceptRole);
    m_apply->setObjectName(QStringLiteral("batchApply"));
    v->addWidget(box);

    connect(add, &QPushButton::clicked, this, [this] {
        QStringList patterns;
        for (const QByteArray &f : QImageReader::supportedImageFormats())
            patterns << QStringLiteral("*.%1").arg(QString::fromLatin1(f));
        const QStringList picked = QFileDialog::getOpenFileNames(this, tr("Add pictures"), QString(),
                                                                 tr("Pictures (%1)").arg(patterns.join(u' ')));
        addFiles(picked);
    });
    connect(m_remove, &QPushButton::clicked, this, [this] {
        qDeleteAll(m_files->selectedItems());
        updateState();
    });
    connect(choose, &QPushButton::clicked, this, [this] {
        const QString folder = QFileDialog::getExistingDirectory(this, tr("Save the copies in"), m_folder->text());
        if (!folder.isEmpty())
            setFolder(folder);
    });
    connect(m_folder, &QLineEdit::textEdited, this, [this] {
        m_folderChosen = true;
        updateState();
    });
    connect(m_files, &QListWidget::itemSelectionChanged, this, &BatchDevelopDialog::updateState);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_apply, &QPushButton::clicked, this, [this] {
        if (apply())
            accept();
    });
    updateState();
}

void BatchDevelopDialog::addFiles(const QStringList &files)
{
    for (const QString &file : files) {
        const QString native = QDir::toNativeSeparators(QFileInfo(file).absoluteFilePath());
        if (!m_files->findItems(native, Qt::MatchExactly).isEmpty())
            continue;
        m_files->addItem(native);
    }
    // A folder of their own next to the first picture, until one is chosen.
    if (!m_folderChosen && m_files->count() > 0)
        m_folder->setText(QDir::toNativeSeparators(QFileInfo(m_files->item(0)->text()).absolutePath() + QStringLiteral("/") + tr("Graded")));
    updateState();
}

void BatchDevelopDialog::setFolder(const QString &folder)
{
    m_folderChosen = true;
    m_folder->setText(QDir::toNativeSeparators(folder));
    updateState();
}

void BatchDevelopDialog::choosePreset(const QString &name)
{
    m_settings->setCurrentIndex(std::max(0, m_settings->findData(name)));
}

QJsonObject BatchDevelopDialog::grading() const
{
    const QString name = m_settings->currentData().toString();
    if (name.isEmpty())
        return m_current;
    const std::optional<grading::Preset> p = grading::preset(name);
    return p ? p->grading : QJsonObject();
}

QStringList BatchDevelopDialog::files() const
{
    QStringList out;
    for (int i = 0; i < m_files->count(); ++i)
        out << QDir::fromNativeSeparators(m_files->item(i)->text());
    return out;
}

QString BatchDevelopDialog::folder() const
{
    return QDir::fromNativeSeparators(m_folder->text().trimmed());
}

void BatchDevelopDialog::updateState()
{
    m_remove->setEnabled(!m_files->selectedItems().isEmpty());
    m_apply->setEnabled(m_files->count() > 0 && !folder().isEmpty());
}

Job *BatchDevelopDialog::apply()
{
    if (files().isEmpty() || folder().isEmpty())
        return nullptr;
    return start(m_studio, grading(), files(), folder());
}

Job *BatchDevelopDialog::start(Studio *studio, const QJsonObject &grading, const QStringList &files, const QString &folder)
{
    if (files.isEmpty())
        return nullptr;
    const QString title = tr("Grade %n picture(s)", nullptr, int(files.size()));
    QPointer<Studio> owner(studio);
    return studio->jobs()->submit(title, QStringLiteral("develop"), [owner, grading, files, folder](Job *job) {
        auto cancelled = std::make_shared<std::atomic_bool>(false);
        job->setCancelHandler([cancelled] { cancelled->store(true); });
        const QPointer<Job> guard(job);
        const int total = int(files.size());
        job->setProgress(0.0, tr("Starting…"));
        async::run([grading, files, folder, cancelled, guard, total] {
            QStringList failed;
            const QStringList written = grading::gradeFiles(
                grading, files, folder, &failed,
                [guard, total](int done, const QString &file) {
                    const QString name = QFileInfo(file).fileName();
                    QMetaObject::invokeMethod(
                        qApp,
                        [guard, done, total, name] {
                            if (guard)
                                guard->setProgress(double(done) / total, tr("%1 of %2: %3").arg(done + 1).arg(total).arg(name));
                        },
                        Qt::QueuedConnection);
                },
                cancelled.get());
            return std::pair{written, failed};
        }).then(job, [job, owner, cancelled, folder](const std::pair<QStringList, QStringList> &result) {
            const auto &[written, failed] = result;
            if (cancelled->load()) {
                job->reportCancelled();
                return;
            }
            if (written.isEmpty() && !failed.isEmpty()) {
                job->fail(failed.join(u'\n'));
                return;
            }
            QString message = tr("%n graded copy(ies) saved in %1.", nullptr, int(written.size())).arg(QDir::toNativeSeparators(folder));
            if (!failed.isEmpty())
                message += u' ' + tr("Not graded: %1").arg(failed.join(QStringLiteral("; ")));
            job->setStatus(message);
            job->succeed(folder);
            if (owner)
                owner->notify(message, failed.isEmpty() ? QStringLiteral("info") : QStringLiteral("warning"));
        });
    });
}

} // namespace lumen::ui
