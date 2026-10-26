#include "ui/DocumentFiles.h"

#include "core/Async.h"
#include "core/Error.h"
#include "core/Paths.h"
#include "core/Settings.h"
#include "ui/Studio.h"

#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>

#include <memory>

namespace lumen::ui {

namespace {

const QString kLastFolderKey = QStringLiteral("files/lastFolder");

QFuture<bool> ready(bool value)
{
    return QtFuture::makeReadyValueFuture(value);
}

} // namespace

QMessageBox *saveOverBox(const SaveOverQuestion &question, QWidget *parent)
{
    auto *box = new QMessageBox(QMessageBox::Question, DocumentFiles::tr("Save over the picture"),
                                DocumentFiles::tr("Save over %1?").arg(question.fileName), QMessageBox::NoButton, parent);
    box->setObjectName(QStringLiteral("saveOverQuestion"));
    box->setInformativeText(question.reasons.join(QLatin1Char('\n')));
    QPushButton *over = box->addButton(DocumentFiles::tr("Save over"), QMessageBox::AcceptRole);
    over->setObjectName(QStringLiteral("saveOver"));
    QPushButton *as = box->addButton(DocumentFiles::tr("Save as…"), QMessageBox::ActionRole);
    as->setObjectName(QStringLiteral("saveAs"));
    box->addButton(QMessageBox::Cancel);
    box->setDefaultButton(over);
    box->setEscapeButton(QMessageBox::Cancel);
    auto *again = new QCheckBox(DocumentFiles::tr("Don't ask again for this picture"), box);
    again->setObjectName(QStringLiteral("dontAskAgain"));
    box->setCheckBox(again);
    return box;
}

QMessageBox *unsavedChangesBox(const QStringList &fileNames, bool quitting, QWidget *parent)
{
    const int n = int(fileNames.size());
    const QString text = n == 1 ? (quitting ? DocumentFiles::tr("Save the changes to %1 before quitting?").arg(fileNames.first())
                                            : DocumentFiles::tr("Save the changes to %1?").arg(fileNames.first()))
                                : DocumentFiles::tr("Save the changes to %n picture(s) before quitting?", nullptr, n);
    auto *box = new QMessageBox(QMessageBox::Warning, quitting ? DocumentFiles::tr("Quit") : DocumentFiles::tr("Close the picture"),
                                text, QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, parent);
    box->setObjectName(QStringLiteral("unsavedChangesQuestion"));
    QString more = n == 1 ? DocumentFiles::tr("If you don't save, the file stays as it was. Your project keeps the changes.")
                          : DocumentFiles::tr("If you don't save, the files stay as they were. Your project keeps the changes.");
    if (n > 1)
        more = fileNames.join(QLatin1Char('\n')) + QStringLiteral("\n\n") + more;
    box->setInformativeText(more);
    box->button(QMessageBox::Discard)->setText(DocumentFiles::tr("Don't save"));
    box->setDefaultButton(QMessageBox::Save);
    box->setEscapeButton(QMessageBox::Cancel);
    return box;
}

DocumentFiles::DocumentFiles(Studio *studio, QWidget *window)
    : QObject(window)
    , m_studio(studio)
    , m_window(window)
{
    askSaveOver = [this](const SaveOverQuestion &question) {
        const std::unique_ptr<QMessageBox> box(saveOverBox(question, m_window));
        box->exec();
        SaveOverReply reply;
        const QString clicked = box->clickedButton() ? box->clickedButton()->objectName() : QString();
        reply.answer = clicked == QLatin1String("saveOver") ? SaveOverReply::SaveOver
                     : clicked == QLatin1String("saveAs")   ? SaveOverReply::SaveAs
                                                            : SaveOverReply::Cancel;
        reply.dontAskAgain = reply.answer == SaveOverReply::SaveOver && box->checkBox()->isChecked();
        return reply;
    };
    askUnsaved = [this](const QStringList &fileNames, bool quitting) {
        const std::unique_ptr<QMessageBox> box(unsavedChangesBox(fileNames, quitting, m_window));
        box->exec();
        return box->standardButton(box->clickedButton());
    };
    askPath = [this](const QString &title, const QString &suggested, const QString &filters, QString *selectedFilter) {
        return QFileDialog::getSaveFileName(m_window, title, suggested, filters, selectedFilter);
    };
}

QList<doc::Document *> DocumentFiles::unsavedDocuments() const
{
    QList<doc::Document *> out;
    for (doc::Document *d : m_studio->documents()) {
        if (d->isFileModified())
            out << d;
    }
    return out;
}

QStringList DocumentFiles::saveOverReasons(const doc::Document *doc, const doc::ImageFormat &format,
                                           const doc::SaveEffects &effects) const
{
    QStringList reasons;
    if (effects.mergesLayers)
        reasons << tr("The layers are merged in the file; your project keeps them.");
    if (effects.dropsTransparency)
        reasons << tr("%1 has no transparency: the transparent parts become white.").arg(format.name);
    if (effects.lossy) {
        const int quality = doc->file().quality;
        reasons << (quality > 0 ? tr("%1 loses a little quality each time it is saved; Lumen keeps the quality the file had (%2 of 100).")
                                      .arg(format.name)
                                      .arg(quality)
                                : tr("%1 loses a little quality each time it is saved.").arg(format.name));
    }
    reasons << doc->file().losses;
    return reasons;
}

QString DocumentFiles::suggestedPath(const doc::Document *doc, const doc::ImageFormat &format) const
{
    QString folder = doc->hasFile() ? QFileInfo(doc->file().path).absolutePath() : Settings::instance().value(kLastFolderKey).toString();
    if (folder.isEmpty() || !QFileInfo(folder).isDir())
        folder = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    // A name from a prompt may hold characters a file name cannot.
    QString name = doc->hasFile() ? QFileInfo(doc->file().path).completeBaseName() : doc->name();
    name.remove(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|]+)")));
    name = name.simplified();
    if (name.isEmpty())
        name = tr("Untitled");
    return QDir(folder).filePath(name + QLatin1Char('.') + format.suffixes.first());
}

QFuture<bool> DocumentFiles::save(doc::Document *doc)
{
    if (!doc)
        return ready(false);
    doc::FileLink link = doc->file();
    const std::optional<doc::ImageFormat> format = link.isEmpty() ? std::nullopt : doc::imageFormatById(link.format);
    if (!format) {
        // A new picture, or one in a format Lumen reads but cannot write (GIF).
        if (!link.isEmpty())
            m_studio->notify(tr("Lumen cannot write %1 files: choose a file to save the picture as.").arg(QString::fromLatin1(link.format).toUpper()));
        return saveAs(doc);
    }
    const int quality = link.quality > 0 ? link.quality : format->defaultQuality;
    if (link.dontAsk || m_confirmed.contains(doc))
        return write(doc, link.path, *format, quality, false);
    // Asked the first time per document when it changes the picture in the file.
    QPointer<doc::Document> guard(doc);
    const doc::DocumentSnapshot layers = doc->snapshot();
    const QSize size = doc->size();
    const doc::ImageFormat f = *format;
    return async::run([layers, size, f] { return doc::saveEffects(layers, size, f); })
        .then(this, [this, guard, f, quality](const doc::SaveEffects &effects) -> QFuture<bool> {
            if (!guard)
                return ready(false);
            const QStringList reasons = saveOverReasons(guard, f, effects);
            if (reasons.isEmpty())
                return write(guard, guard->file().path, f, quality, false);
            const SaveOverReply reply = askSaveOver(SaveOverQuestion{guard->file().fileName(), reasons});
            if (!guard || reply.answer == SaveOverReply::Cancel)
                return ready(false);
            if (reply.answer == SaveOverReply::SaveAs)
                return saveAs(guard);
            doc::Document *d = guard;
            if (!m_confirmed.contains(d)) {
                m_confirmed.insert(d);
                connect(d, &QObject::destroyed, this, [this, d] { m_confirmed.remove(d); });
            }
            if (reply.dontAskAgain) {
                doc::FileLink link = d->file();
                link.dontAsk = true;
                d->updateFile(link);
                d->setModified(true); // the project remembers it
            }
            return write(d, d->file().path, f, quality, false);
        })
        .unwrap();
}

QFuture<bool> DocumentFiles::saveAs(doc::Document *doc, const QString &chosen)
{
    if (!doc)
        return ready(false);
    const QList<doc::ImageFormat> formats = doc::writableImageFormats();
    const doc::FileLink link = doc->file();
    // The format the picture has, else PNG (it keeps transparency and loses nothing).
    const doc::ImageFormat preferred = doc::imageFormatById(link.format).value_or(formats.first());
    QString path = chosen;
    if (path.isEmpty()) {
        QString filter = preferred.filter();
        path = askPath(tr("Save as"), suggestedPath(doc, preferred), doc::imageFileFilter(formats), &filter);
        if (path.isEmpty())
            return ready(false);
        if (QFileInfo(path).suffix().isEmpty()) {
            for (const doc::ImageFormat &f : formats) {
                if (f.filter() == filter)
                    path += QLatin1Char('.') + f.suffixes.first();
            }
        }
    }
    const std::optional<doc::ImageFormat> format = doc::imageFormatFor(path);
    if (!format) {
        m_studio->notify(tr("Lumen cannot write %1 files. Choose one of the formats in the list.").arg(QFileInfo(path).suffix().toUpper()),
                         QStringLiteral("warning"));
        return ready(false);
    }
    const int quality = format->id == link.format && link.quality > 0 ? link.quality : format->defaultQuality;
    return write(doc, QDir::cleanPath(QFileInfo(path).absoluteFilePath()), *format, quality, true);
}

QFuture<bool> DocumentFiles::exportCopy(doc::Document *doc, const QString &chosen)
{
    if (!doc)
        return ready(false);
    const QList<doc::ImageFormat> formats = doc::writableImageFormats();
    QString path = chosen;
    if (path.isEmpty()) {
        QString filter = formats.first().filter();
        path = askPath(tr("Export a copy"), suggestedPath(doc, formats.first()), doc::imageFileFilter(formats), &filter);
        if (path.isEmpty())
            return ready(false);
        if (QFileInfo(path).suffix().isEmpty()) {
            for (const doc::ImageFormat &f : formats) {
                if (f.filter() == filter)
                    path += QLatin1Char('.') + f.suffixes.first();
            }
        }
    }
    const std::optional<doc::ImageFormat> format = doc::imageFormatFor(path);
    if (!format) {
        m_studio->notify(tr("Lumen cannot write %1 files. Choose one of the formats in the list.").arg(QFileInfo(path).suffix().toUpper()),
                         QStringLiteral("warning"));
        return ready(false);
    }
    const doc::DocumentSnapshot layers = doc->snapshot();
    const QSize size = doc->size();
    const doc::ImageFormat f = *format;
    const QString name = QFileInfo(path).fileName();
    Settings::instance().setValue(kLastFolderKey, QFileInfo(path).absolutePath());
    return async::run([path, f, layers, size] { doc::writeImageFile(path, f, layers, size, f.defaultQuality); })
        .then(this, [this, name] {
            m_studio->notify(tr("Exported a copy as %1.").arg(name), QStringLiteral("success"));
            return true;
        })
        .onFailed(this, [this](const std::exception &e) {
            m_studio->notify(describeException(e), QStringLiteral("error"));
            return false;
        });
}

QFuture<bool> DocumentFiles::write(doc::Document *doc, const QString &path, const doc::ImageFormat &format, int quality, bool link)
{
    QPointer<doc::Document> guard(doc);
    const doc::DocumentSnapshot layers = doc->snapshot();
    const QSize size = doc->size();
    // A change made while the file is written is not in it: the document stays modified then.
    const int step = doc->undoStack()->index();
    const QString name = QFileInfo(path).fileName();
    return async::run([path, format, layers, size, quality] { doc::writeImageFile(path, format, layers, size, quality); })
        .then(this, [this, guard, path, format, quality, link, step, name] {
            Settings::instance().addRecentFile(path);
            if (link)
                Settings::instance().setValue(kLastFolderKey, QFileInfo(path).absolutePath());
            if (guard) {
                doc::FileLink file = link ? doc::FileLink{} : guard->file();
                file.path = path;
                file.format = format.id;
                file.quality = quality;
                file.losses.clear(); // the file now has what the document has
                file.stamp();
                const bool changed = guard->undoStack()->index() != step;
                if (link) {
                    // Saved as a file chosen knowing what its format keeps: Save does not ask
                    // about it again in this session.
                    guard->setFile(file);
                    guard->setName(QFileInfo(path).completeBaseName());
                    doc::Document *d = guard;
                    if (!m_confirmed.contains(d)) {
                        m_confirmed.insert(d);
                        connect(d, &QObject::destroyed, this, [this, d] { m_confirmed.remove(d); });
                    }
                } else {
                    guard->updateFile(file);
                    guard->markFileSaved();
                }
                if (changed)
                    guard->setFileModified(true);
                guard->setModified(true); // the project remembers the file and that it is saved
            }
            m_studio->notify(tr("Saved %1.").arg(name), QStringLiteral("success"));
            emit saved(guard, path);
            return true;
        })
        .onFailed(this, [this, name](const std::exception &e) {
            m_studio->notify(tr("%1 was not saved, the file is as it was: %2").arg(name, describeException(e)), QStringLiteral("error"));
            return false;
        });
}

QFuture<bool> DocumentFiles::close(doc::Document *doc)
{
    if (!doc)
        return ready(false);
    if (!doc->isFileModified()) {
        m_studio->closeDocument(doc);
        return ready(true);
    }
    const QMessageBox::StandardButton answer = askUnsaved({doc->file().fileName()}, false);
    if (answer == QMessageBox::Discard) {
        m_studio->closeDocument(doc);
        return ready(true);
    }
    if (answer != QMessageBox::Save)
        return ready(false);
    QPointer<doc::Document> guard(doc);
    return save(doc).then(this, [this, guard](bool saved) {
        if (!saved || !guard)
            return false;
        m_studio->closeDocument(guard);
        return true;
    });
}

QFuture<bool> DocumentFiles::confirmQuit()
{
    const QList<doc::Document *> unsaved = unsavedDocuments();
    if (unsaved.isEmpty())
        return ready(true);
    QStringList names;
    QList<QPointer<doc::Document>> docs;
    for (doc::Document *d : unsaved) {
        names << d->file().fileName();
        docs << d;
    }
    const QMessageBox::StandardButton answer = askUnsaved(names, true);
    if (answer == QMessageBox::Save)
        return saveEach(docs);
    if (answer != QMessageBox::Discard)
        return ready(false);
    // Not reopened at the next start: the files stay as they were, the project keeps the documents.
    QList<QFuture<void>> closing;
    for (const QPointer<doc::Document> &d : std::as_const(docs))
        closing << m_studio->closeDocument(d);
    return QtFuture::whenAll(closing.begin(), closing.end()).then([](const QList<QFuture<void>> &) { return true; });
}

QFuture<bool> DocumentFiles::saveEach(QList<QPointer<doc::Document>> docs)
{
    docs.removeIf([](const QPointer<doc::Document> &d) { return d.isNull(); });
    if (docs.isEmpty())
        return ready(true);
    const QPointer<doc::Document> first = docs.takeFirst();
    return save(first)
        .then(this, [this, docs](bool saved) -> QFuture<bool> { return saved ? saveEach(docs) : ready(false); })
        .unwrap();
}

} // namespace lumen::ui
