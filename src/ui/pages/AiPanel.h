#pragma once

#include "ai/CharacterContext.h"
#include "ai/GenerationService.h"
#include "doc/Document.h"

#include <QPointer>
#include <QSet>
#include <QWidget>

#include <functional>
#include <optional>

class QAbstractButton;
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QVBoxLayout;

namespace lumen::ui {

class AiOptions;
class AutoTextEdit;
class RunSettingsEditor;
class CharacterChip;
class ProviderCombo;
class ReferenceStrip;
class SliderField;
class Studio;

// The editor's advanced AI panel (opened from the prompt bar's ⋯ menu, "All AI options…"): the
// image engine (the engine for changes of Settings → AI unless one is chosen), the project's character, the actions that are not "describe a change" (variation,
// expand canvas, upscale, remove background, detect pose; also in the AI menu) and every option of
// the prompt bar (AiOptions). It also runs the prompt bar's requests (submitPrompt), so both go
// through the same character handling and region pipeline.
class AiPanel : public QWidget
{
    Q_OBJECT
public:
    // Inpaint and Edit are no longer the panel's own actions (the prompt bar picks them); the values
    // stay, since images record the action they were made with.
    enum class Mode { Inpaint, Variation, Edit, Expand, Upscale, RemoveBackground, DetectPose };

    // A request of the prompt bar (or of the panel's Variation). The other values (steps, seed,
    // variants, negative prompt, control pose, the region's context and edge) come from the
    // options, the references from the panel.
    struct PromptRun
    {
        // The engine the bar planned the run for; empty: the options' engine for this kind of run
        // (the engine for new images for TextToImage, else the engine for changes).
        QString providerId;
        ai::GenMode mode = ai::GenMode::Edit; // Edit, Inpaint, ImageToImage or TextToImage
        bool region = false;                  // through the selection (the region pipeline)
        QString prompt;                       // the user's words
        double strength = 0.75;               // inpainting and img2img only
        bool withCharacter = true;            // false when removing an object
        QString name;                         // of the result layer
        std::optional<Mode> action;           // recorded with the images
        // A plan's words (plan M18 §18.5) go as they are, with its negative prompt.
        bool literal = false;
        std::optional<QString> negative;
    };
    // Where the jobs of a prompt run go (the variants strip of the prompt bar), with the number of
    // results they make; called once per run, with no jobs when the run could not be submitted.
    // Without a handler their results become layers, the first visible.
    using ResultsHandler = std::function<void(doc::Document *doc, const QList<Job *> &jobs, const QString &name, int results)>;
    // Told when a run starts being prepared (the character and the picture are read first), before
    // the results handler gets its jobs.
    using PreparingHandler = std::function<void(doc::Document *doc)>;

    AiPanel(Studio *studio, AiOptions *options, QWidget *parent = nullptr);
    void setDocument(doc::Document *doc);
    void setMode(Mode mode);
    Mode mode() const { return m_mode; }
    // Runs one of the panel's actions with the panel's settings (the AI menu and the panel's button).
    void run(Mode mode);
    // The utility an action runs (Upscale, Remove background, Detect pose), which not every image
    // engine has; none for the others.
    static std::optional<ai::Utility> utilityOf(Mode mode);
    // The AI menu's Upscale 2× / 3× / 4×.
    void upscale(double factor);
    // Submits a prompt run on the panel's document, with the project's character unless the run or
    // the character button says otherwise. The jobs go to the results handler.
    void submitPrompt(const PromptRun &run);
    void setResultsHandler(ResultsHandler handler, PreparingHandler preparing = {})
    {
        m_results = std::move(handler);
        m_preparing = std::move(preparing);
    }
    ReferenceStrip *references() const { return m_refs; }
    AiOptions *options() const { return m_options; }

    // Adds generated images to a document as new layers (scaled to its size) in one undo step;
    // only the first stays visible, the others are variants to compare.
    static void addResultLayers(Studio *studio, doc::Document *doc, const QList<QImage> &images, const QString &name,
                                const QJsonObject &meta);

private:
    void updateUi();
    void syncFromOptions();
    // The picture of the panel's document (or of `doc`), flattened on a worker thread.
    void submitWithComposite(std::function<void(QImage composite)> fn);
    void submitWithComposite(doc::Document *doc, std::function<void(QImage composite)> fn);
    // Adds the page's character (if any) to the request for this use on `provider`, then continues
    // with the request and the metadata of what went in (empty without a character).
    void withCharacter(ai::ImageRequest request, ai::CharacterUse use, bool skip, ai::ImageProvider *provider,
                       std::function<void(ai::ImageRequest, QJsonObject)> then);
    void trackJob(Job *job, std::function<void(doc::Document *, const ai::GenerationOutput &)> apply);
    void addResultLayers(doc::Document *doc, const QList<QImage> &images, const QString &name, const QJsonObject &meta)
    {
        addResultLayers(m_studio, doc, images, name, meta);
    }
    // The request values shared by every prompt run: words, negative prompt, steps, seed, variants,
    // references and the control pose.
    // The request for the words, fitted to the engine's model (plan M18 §18.2): `area` for a change
    // of the selection, whose instruction an inpainting model gets as a description of the result.
    // `literal` and `negative` override Use my words as they are and the negative prompt of the options.
    ai::ImageRequest baseRequest(const ai::ImageProvider *provider, const QString &prompt, ai::GenMode mode, bool area,
                                 bool literal = false, const std::optional<QString> &negative = std::nullopt) const;
    bool takesNegative(const ai::ImageProvider *provider) const;

    Studio *m_studio;
    AiOptions *m_options;
    QPointer<doc::Document> m_doc;
    Mode m_mode = Mode::Variation;
    ResultsHandler m_results;
    PreparingHandler m_preparing;
    QWidget *m_characterRow = nullptr;
    bool m_charactersOn = false; // the Characters plugin is on
    QAbstractButton *m_detectPose = nullptr;
    ProviderCombo *m_provider;
    QButtonGroup *m_modes;
    QLabel *m_modeHint;
    SliderField *m_strength;
    QWidget *m_expandBox;
    QCheckBox *m_expandLeft, *m_expandRight, *m_expandTop, *m_expandBottom;
    QSpinBox *m_expandAmount;
    QComboBox *m_upscaleFactor;
    QWidget *m_upscaleBox;
    ReferenceStrip *m_refs;
    QWidget *m_refsBox;
    QCheckBox *m_usePose;
    QCheckBox *m_literal;
    QSpinBox *m_variants;
    RunSettingsEditor *m_settings;
    QSpinBox *m_seed;
    QCheckBox *m_randomSeed;
    QWidget *m_negativeBox;
    AutoTextEdit *m_negative;
    SliderField *m_areaStrength;
    SliderField *m_padding;
    SliderField *m_feather;
    QCheckBox *m_colorMatch;
    QPushButton *m_run;
    QVBoxLayout *m_jobs;
    QLabel *m_capsWarning;
    CharacterChip *m_character;
    QSet<QString> m_notes;
};

} // namespace lumen::ui
