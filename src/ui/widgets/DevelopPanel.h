#pragma once

#include "gpu/Effects.h"
#include "ui/Grading.h"

#include <QHash>
#include <QPointer>
#include <QTimer>
#include <QUuid>
#include <QWidget>

class QComboBox;
class QMenu;
class QLabel;
class QPushButton;
class QToolButton;

namespace lumen::doc {
class Document;
struct Layer;
}

namespace lumen::ui {

class CanvasView;
class ColorWheel;
class CurveEditor;
class SliderField;
class Studio;

// The Develop tab (plan M20 steps 3 and 4): the active layer's grading on the graphics card, in
// groups that open and close: the basics (white balance, tone, presence), a tone curve, a colour
// mixer, colour grading wheels and a LUT, each an effect whose controls come from its shader. While
// a control moves, the canvas shows the picture at screen size; when it is let go the layer is
// graded at full size in the background, as one undo step. The layer keeps its pixels from before
// the grading, so the grading can be changed or reset later; Bake makes it part of the pixels.
class DevelopPanel : public QWidget
{
    Q_OBJECT
public:
    DevelopPanel(Studio *studio, CanvasView *canvas, QWidget *parent = nullptr);
    ~DevelopPanel() override;

    // Every effect's values now, by parameter name (a name two effects use: the first one's in the
    // tab, such as Develop's Shadows before Colour grading's).
    QVariantMap values() const;
    grading::State state() const { return m_state; }
    // What a moved control does, from code: a slider, a colour wheel (hue°, saturation %), a curve
    // ("rgb", "r", "g", "b": points "x y, …" from 0 to 255), the LUT (a name in the LUTs folder;
    // empty for none). A control's name is its parameter's, or "effect/name" when two effects share
    // it ("grading/shadows").
    void setValue(const QString &name, double value);
    void setWheel(const QString &name, double hue, double saturation);
    void setCurve(const QString &channel, const QString &points);
    void setLut(const QString &name);
    // Opens or closes every group.
    void expandAll(bool open);
    // Grades the layer with the controls as they are (what letting go of one does).
    void commit();
    bool isBusy() const { return m_committing; }
    void showBefore(bool before);

    // Presets and settings to copy (plan M20 step 8): a grading put on every control and the layer
    // (one undo step), a preset by its shown name, the settings to and from the clipboard, saved as
    // one of your presets.
    void applyGrading(const QJsonObject &grading);
    bool applyPreset(const QString &name);
    void copySettings();
    bool pasteSettings();
    bool saveAsPreset(const QString &name, QString *error = nullptr);

signals:
    // A full-size grading was put on the layer, or failed (the reason).
    void committed(bool ok, const QString &message);

protected:
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;

private:
    void ensureBuilt();
    QWidget *buildGroup(const gpu::Effect &effect, bool open);
    void setDocument(doc::Document *doc);
    void loadLayer();
    void showState();
    void updateState();
    void changed();
    void schedulePreview();
    void renderPreview();
    void clearPreview();
    void fillLuts();
    void fillPresetMenu(QMenu *menu);
    QImage scaledLayer(const doc::Layer &layer, const QImage &pixels, const QSize &size);
    bool differsFromLayer() const;
    QString keyFor(const QString &name, const QList<QString> &keys) const;

    Studio *m_studio;
    CanvasView *m_canvas;
    QPointer<doc::Document> m_doc;
    QUuid m_layer;
    QHash<QString, gpu::Effect> m_effects;
    grading::State m_state;
    QString m_unavailable;
    QHash<QString, SliderField *> m_fields;
    QHash<QString, ColorWheel *> m_wheels;
    QHash<QString, QToolButton *> m_groups;
    CurveEditor *m_curve = nullptr;
    QComboBox *m_curveChannel = nullptr;
    QComboBox *m_lut = nullptr;
    QLabel *m_status = nullptr;
    QWidget *m_controls = nullptr;
    QPushButton *m_before = nullptr;
    QPushButton *m_reset = nullptr;
    QPushButton *m_bake = nullptr;
    QPushButton *m_presets = nullptr;
    QPushButton *m_copy = nullptr;
    QPushButton *m_paste = nullptr;
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
