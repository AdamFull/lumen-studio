#pragma once

#include "doc/Document.h"

#include <QFrame>
#include <QHash>
#include <QJsonObject>
#include <QPointer>

class QButtonGroup;
class QHBoxLayout;
class QPushButton;

namespace lumen {
class Job;
}

namespace lumen::ui {

class CanvasView;
class ElidedLabel;
class Studio;

// The results of a prompt run, above the prompt bar. The canvas shows the current one over the
// document without changing it; ←/→ or a click on a thumbnail picks another, Enter or Keep adds it as
// a new layer named after the prompt (one undo step; a result of a selection is already masked to
// it), and Esc or Discard drops them all. Each document has its own results. A new run replaces the
// results on show, or joins a run that is still going.
class VariantStrip : public QFrame
{
    Q_OBJECT
public:
    VariantStrip(Studio *studio, CanvasView *canvas, QWidget *parent = nullptr);

    // The document whose results are shown (the editor's active one).
    void setDocument(doc::Document *doc);
    // A run on `doc` is being prepared (its jobs come with track()): it counts as running from now
    // on, so that a second Generate does not start another one, and the results on show go.
    void expect(doc::Document *doc);
    // A run's jobs on `doc`, which make `results` images in all: the images of each one become
    // results when it finishes. No jobs: a run that was expected could not be submitted. A job
    // retried from the Queue after it failed or was cancelled joins the document's results again.
    void track(doc::Document *doc, const QList<Job *> &jobs, const QString &name, int results);
    // Results to choose from, scaled to the document (`thumbs`: what the thumbnails show, else the
    // images). Also for tests and screenshots.
    void addVariants(doc::Document *doc, const QList<QImage> &images, const QString &name, const QJsonObject &meta = {},
                     const QList<QImage> &thumbs = {});

    // Of the shown document.
    int count() const;
    int current() const;
    void setCurrent(int index);
    QImage currentImage() const;
    QString currentName() const;
    bool isRunning() const;
    // The running jobs' progress (0 … 1, or -1 when unknown) and the first one's status.
    double progress() const;
    QString status() const;
    // How many results the running jobs are still making.
    int pending() const;
    // The line next to the thumbnails.
    QString label() const;

    // Adds the current result as a layer on top and drops the others (and stops what still runs).
    void keep();
    // Drops every result of the shown document and stops its running jobs.
    void discard();
    // Stops the shown document's running jobs; the results so far stay.
    void stop();

    // ←/→, Enter and Esc for the results; false when there are none or the key is another one. The
    // canvas hands over the keys its tool does not use.
    bool handleKey(QKeyEvent *e);

signals:
    // The results or the jobs of the shown document changed, or the current result.
    void stateChanged();
    // The first results of a run arrived.
    void resultsArrived();

protected:
    void keyPressEvent(QKeyEvent *e) override;

private:
    struct Variant
    {
        QImage image; // document-sized
        QImage thumb;
        QString name;
        QJsonObject meta;
    };
    struct Tracked
    {
        QPointer<Job> job;
        int results = 1; // images it makes
    };
    struct Run
    {
        QList<Variant> variants;
        QList<Tracked> jobs;
        int current = 0;
        int submitting = 0; // runs expected whose jobs have not come yet
        bool stopped = false; // Cancel while one was being prepared: its jobs stop when they come
        bool has(const Job *job) const;
    };
    // The run of a document, made (and its document watched) when it has none.
    Run &runFor(doc::Document *doc);
    static bool running(const Run &run);
    Run *shown();
    const Run *shown() const;
    void drop(doc::Document *doc);
    // The document closes: its run goes and what still runs is cancelled.
    void forget(doc::Document *doc);
    void watch(doc::Document *doc, Job *job, const QString &name, int results);
    void rejoin(doc::Document *doc, Job *job, int results);
    void finished(doc::Document *doc, Job *job, const QString &name);
    void removeIfEmpty(doc::Document *doc);
    void rebuild();
    void refresh();
    void updateLabel();

    Studio *m_studio;
    CanvasView *m_canvas;
    QPointer<doc::Document> m_doc;
    QHash<doc::Document *, Run> m_runs;
    ElidedLabel *m_label;
    QHBoxLayout *m_thumbs;
    QButtonGroup *m_group;
    QPushButton *m_keep;
    QPushButton *m_discard;
};

} // namespace lumen::ui
