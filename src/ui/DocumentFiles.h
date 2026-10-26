#pragma once

#include "doc/Document.h"
#include "doc/ImageFile.h"

#include <QFuture>
#include <QMessageBox>
#include <QObject>
#include <QPointer>
#include <QSet>

#include <functional>

namespace lumen::ui {

class Studio;

// The question before File → Save writes over a picture in a way that changes it (askSaveOver).
struct SaveOverQuestion
{
    QString fileName;    // "photo.jpg"
    QStringList reasons; // what saving over it changes, in plain words
};
struct SaveOverReply
{
    enum Answer { SaveOver, SaveAs, Cancel };
    Answer answer = Cancel;
    bool dontAskAgain = false; // for this picture, also in later sessions
};

// The questions as message boxes, not shown yet (the layout audit shows them). Save over / Save
// as… / Cancel with "Don't ask again for this picture"; Save / Don't save / Cancel for changes not
// saved to their files when a document closes (`quitting`: when Lumen quits).
QMessageBox *saveOverBox(const SaveOverQuestion &question, QWidget *parent);
QMessageBox *unsavedChangesBox(const QStringList &fileNames, bool quitting, QWidget *parent);

// Files as in an image editor (M17): File → Save writes a document's picture back to the file it
// was opened from or last saved as, in that file's format; Save as… chooses a file (PNG, JPEG,
// WebP, TIFF where Qt can write them, layered PSD) that the document remembers from then on; Export
// a copy… writes one it does not. The first time per document that Save would change the picture
// in the file (the layers merged, transparency or quality lost, parts of the file Lumen does not
// keep), it asks first. A document that cannot be written back (none, or a format Lumen cannot
// write) goes to Save as…. Closing a document or quitting with changes not saved to its file asks
// Save / Don't save / Cancel. The project keeps every document with its layers whatever is chosen.
// Writing goes through a temporary file, so a failed save leaves the picture as it was.
class DocumentFiles : public QObject
{
    Q_OBJECT
public:
    // `window` is the parent of the questions and the file dialogs.
    DocumentFiles(Studio *studio, QWidget *window);

    // File → Save. Resolves to whether the picture was written.
    QFuture<bool> save(doc::Document *doc);
    // File → Save as…: to `path`, or to a file chosen in the dialog when empty.
    QFuture<bool> saveAs(doc::Document *doc, const QString &path = {});
    // File → Export a copy…: the document's own file stays as it is.
    QFuture<bool> exportCopy(doc::Document *doc, const QString &path = {});
    // Closes the document, asking first when it has changes not saved to its file. Resolves to
    // whether it closed.
    QFuture<bool> close(doc::Document *doc);
    // Before quitting: one question about every document with changes not saved to its file.
    // Resolves to whether Lumen may quit; those not saved are closed, so the next start does not
    // bring them back.
    QFuture<bool> confirmQuit();

    // The documents with changes not saved to their files.
    QList<doc::Document *> unsavedDocuments() const;

    // The questions; tests answer them without dialogs.
    std::function<SaveOverReply(const SaveOverQuestion &)> askSaveOver;
    std::function<QMessageBox::StandardButton(const QStringList &fileNames, bool quitting)> askUnsaved;
    // A file to write; empty: cancelled. `selectedFilter` is the dialog's filter in and out.
    std::function<QString(const QString &title, const QString &suggested, const QString &filters, QString *selectedFilter)> askPath;

signals:
    // The document's picture was written to `path` (Save, Save as…).
    void saved(lumen::doc::Document *doc, const QString &path);

private:
    // What saving over the document's file changes, in plain words (empty: nothing).
    QStringList saveOverReasons(const doc::Document *doc, const doc::ImageFormat &format, const doc::SaveEffects &effects) const;
    QFuture<bool> write(doc::Document *doc, const QString &path, const doc::ImageFormat &format, int quality, bool link);
    QString suggestedPath(const doc::Document *doc, const doc::ImageFormat &format) const;
    QFuture<bool> saveEach(QList<QPointer<doc::Document>> docs);

    Studio *m_studio;
    QPointer<QWidget> m_window;
    // Documents whose save-over question was answered in this session ("the first time").
    QSet<const doc::Document *> m_confirmed;
};

} // namespace lumen::ui
