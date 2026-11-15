#pragma once

#include "gpu/Effects.h"

#include <QHash>
#include <QPointer>
#include <QTimer>
#include <QUuid>
#include <QWidget>

class QLabel;
class QPushButton;

namespace lumen::doc {
class Document;
struct Layer;
}

namespace lumen::ui {

class CanvasView;
class SliderField;
class Studio;

// The Develop tab (plan M20 step 3): white balance, tone and presence for the active layer, on the
// graphics card. While a slider moves the canvas shows the picture at screen size; when it is let go
// the layer is graded at full size in the background, as one undo step. The layer keeps its pixels
// from before the grading, so the grading can be changed or reset later; Bake makes it part of the
// pixels. Its sliders come from the Develop effect's shader.
class DevelopPanel : public QWidget
{
    Q_OBJECT
public:
    DevelopPanel(Studio *studio, CanvasView *canvas, QWidget *parent = nullptr);
    ~DevelopPanel() override;

    // The sliders' values now (the effect's parameter names).
    QVariantMap values() const { return m_values; }
    void setValue(const QString &name, double value);
    // Grades the layer with the values now (what letting go of a slider does).
    void commit();
    // Whether a full-size grading is being made.
    bool isBusy() const { return m_committing; }
    void showBefore(bool before);

signals:
    // A full-size grading was put on the layer, or failed (the reason).
    void committed(bool ok, const QString &message);

protected:
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;

private:
    void ensureBuilt();
    void setDocument(doc::Document *doc);
    void loadLayer();
    void updateState();
    void schedulePreview();
    void renderPreview();
    void clearPreview();
    QImage scaledLayer(const doc::Layer &layer, const QImage &pixels, const QSize &size);
    QJsonObject gradingFor(const QVariantMap &values) const;
    bool isNeutral(const QVariantMap &values) const;
    bool differsFromLayer() const;

    Studio *m_studio;
    CanvasView *m_canvas;
    QPointer<doc::Document> m_doc;
    QUuid m_layer;
    gpu::Effect m_effect;
    QString m_unavailable;
    QHash<QString, SliderField *> m_fields;
    QVariantMap m_values;
    QLabel *m_status = nullptr;
    QWidget *m_sliders = nullptr;
    QPushButton *m_before = nullptr;
    QPushButton *m_reset = nullptr;
    QPushButton *m_bake = nullptr;
    gpu::Chain m_previewChain;
    gpu::Chain m_fullChain;
    QTimer m_previewTimer;
    bool m_built = false;
    bool m_shown = false;
    bool m_previewing = false;   // the canvas shows this panel's preview
    bool m_committing = false;
    bool m_commitAgain = false;
    bool m_applying = false;     // our own grading going onto the layer: not a change to load
    struct Scaled
    {
        quint64 revision = 0;
        qint64 key = 0;
        QImage image;
    };
    QHash<QString, Scaled> m_scaled; // layer pictures at the preview's size
};

} // namespace lumen::ui
