#pragma once

#include "doc/Document.h"
#include "filters/Evaluator.h"
#include "filters/Graph.h"

#include <QFuture>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUuid>

#include <functional>
#include <memory>
#include <optional>

namespace lumen::ui {

class FilterPreviewCache;

// Runs a filter on the active layer of a document (M17 §17.6), for the Filters panel:
// * a preview of the whole picture at reduced resolution while the filter's settings change, for
//   the canvas (previewChanged); a filter with AI steps previews only after run();
// * apply(): the filter at full resolution on the active layer, through the selection (also the
//   graph's Selection input), onto a new layer above it or the layer itself, as one undo step.
// Evaluation and compositing run on worker threads; apply() and run() can be cancelled and report
// their progress. One evaluator serves the preview and apply(), so what a preview computed (an AI
// step after run()) is not computed again when the filter is applied.
//
// Only the selected area changes; the graph's own mask (the Output's "Where to apply") narrows it
// further. A result of another size than the layer is stretched to the layer, except one of the
// same proportions (an upscale, a resize) applied with nothing selected: that scales the whole
// picture to the result's size, in the same undo step. A result that takes opacity away from the
// layer (a cut-out, "Remove background") goes onto the new layer whole and the layer is hidden, so
// the transparency shows. GUI-thread object.
class FilterRunner : public QObject
{
    Q_OBJECT
public:
    enum class Target { NewLayer, Layer };

    explicit FilterRunner(QObject *parent = nullptr);
    ~FilterRunner() override;

    void setDocument(doc::Document *doc);
    doc::Document *document() const { return m_doc; }
    // The filter with the values its settings have now (nullopt: none). `name` names the undo step
    // and the new layer.
    void setGraph(const std::optional<filters::Graph> &graph, const QString &name = {});
    const std::optional<filters::Graph> &graph() const { return m_graph; }
    QString name() const { return m_name; }
    // One setting of the filter (a published slider moved).
    bool setParam(const QString &node, const QString &param, const QVariant &value);

    // Where apply() puts the result, and what the preview shows.
    void setTarget(Target target);
    Target target() const { return m_target; }

    void setPreviewEnabled(bool on);
    bool isPreviewEnabled() const { return m_previewEnabled; }
    // The longest side of the preview in pixels (the picture's own size when it is smaller).
    void setPreviewSize(int longestSide);
    int previewSize() const { return m_previewSize; }
    // The last preview: the document's picture with the filter, at the preview's size. Null when
    // there is none (no filter, previews off, after apply() until a setting changes).
    QImage previewImage() const { return m_preview; }
    // The filter has an AI step whose result is not there yet: the preview waits for run().
    bool needsRun() const { return m_needsRun; }

    // Previews the filter with its AI steps (they run at full resolution, so apply() reuses them).
    bool run(QString *why = nullptr);
    // Applies the filter to the active layer. False when it cannot start (no document, no filter,
    // an invalid filter, a locked layer for Target::Layer, already busy), with the reason.
    bool apply(Target target, QString *why = nullptr);
    bool apply(QString *why = nullptr) { return apply(m_target, why); }
    bool isBusy() const { return m_busy != Busy::No; }
    // What else the last apply() did, in plain words ("The picture is now 1280 × 960 pixels."); empty
    // when it only added or changed the layer.
    QString appliedNote() const { return m_appliedNote; }
    bool isApplying() const { return m_busy == Busy::Applying; }
    // Stops apply() or run(); nothing changes.
    void cancel();

signals:
    void previewChanged(const QImage &image); // null: show the document as it is
    void previewFailed(const QString &message);
    void needsRunChanged(bool needsRun);
    void busyChanged(bool busy);
    // While apply() or run() works: the steps done, the steps in all, the step that runs now.
    void progress(int done, int total, const QString &step);
    // How far the step that runs now is, when it says (an AI step's job): a fraction in [0, 1]
    // (negative: not known) and its status in plain words.
    void stepProgress(double fraction, const QString &status);
    // apply() or run() ended: ok, else what happened in plain words (cancelled, failed).
    void finished(bool ok, const QString &message);
    // apply() changed the document: the new layer, or the layer that changed.
    void applied(const QUuid &layer);

private:
    enum class Busy { No, Running, Applying };
    struct ProgressSink;

    void schedulePreview();
    void startPreview(bool runExpensive);
    void clearPreview();
    void setNeedsRun(bool needsRun);
    void setBusy(Busy busy);
    std::function<void(int, int, const QString &)> progressRelay() const;
    std::function<void(double, const QString &)> stepProgressRelay() const;

    QPointer<doc::Document> m_doc;
    QList<QMetaObject::Connection> m_docConnections;
    std::optional<filters::Graph> m_graph;
    QString m_name;
    QString m_appliedNote;
    Target m_target = Target::NewLayer;
    bool m_previewEnabled = true;
    bool m_suspended = false; // after apply(), until the filter or a setting changes
    int m_previewSize = 1280;
    QImage m_preview;
    bool m_needsRun = false;
    Busy m_busy = Busy::No;
    bool m_previewPending = false; // a change while busy: preview when done
    int m_generation = 0;
    QFuture<filters::EvalResult> m_previewFuture;
    QTimer m_previewTimer;
    std::unique_ptr<filters::Evaluator> m_evaluator;
    std::shared_ptr<FilterPreviewCache> m_scaled;
    std::shared_ptr<ProgressSink> m_sink;
    // False once the destructor runs: continuations that are cancelled by it run while QObject's
    // destructor runs, after the members are gone.
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

// What applying a filter result to a layer gives (worker threads; the tests check it directly).
// `where` is the selection times the graph's mask (null: everywhere); `result` is stretched to
// the layer. For a new layer the result is masked by `where` (transparent elsewhere); for the
// layer itself the result is mixed in through `where`, and `rect` covers only what changes. A
// result that takes opacity away from the layer within `where` (a cut-out) gives a new layer that
// holds the whole layer with the result mixed in, and `hidesLayer`: the layer is to be hidden.
struct FilterComposite
{
    QRect rect;
    QImage pixels; // rect-sized
    bool hidesLayer = false;
};
FilterComposite composeFilterResult(const QImage &layer, const QImage &result, const QImage &where,
                                    FilterRunner::Target target, bool keepAlpha);
// The product of two masks stretched to `size`; a null mask counts as everywhere, so two give null.
QImage combinedMask(const QImage &a, const QImage &b, const QSize &size);
// Whether a result of size `result` applied to a layer of size `layer` with nothing selected
// scales the picture: another size with the same proportions (within rounding), as an upscale or a
// resize gives.
bool resultScalesPicture(const QSize &layer, const QSize &result);

} // namespace lumen::ui
