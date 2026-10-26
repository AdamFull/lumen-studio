#pragma once

#include "ui/pages/PromptPlan.h"

#include <QFrame>
#include <QPointer>
#include <QTimer>

class QCheckBox;
class QFormLayout;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QToolButton;

namespace lumen::doc {
class Document;
}

namespace lumen::ui {

class AiOptions;
class AiPanel;
class AutoTextEdit;
class ElidedLabel;
class ProviderCombo;
class SliderField;
class Studio;
class VariantStrip;

// The editor's prompt bar, docked under the canvas while a document is open: a line saying what
// Generate does now (or why it cannot), the words, Generate, Remove (with a selection) and a ⋯ menu
// with what most people never need. What Generate does follows the selection, the canvas and the
// image engine (promptPlan). The requests go through the AI panel (AiPanel::submitPrompt) and the
// results to the variants strip above the bar; while they run, the bar shows the progress and
// Generate becomes Cancel.
class PromptBar : public QFrame
{
    Q_OBJECT
public:
    PromptBar(Studio *studio, AiOptions *options, AiPanel *ai, VariantStrip *strip, QWidget *parent = nullptr);

    void setDocument(doc::Document *doc);
    // What Generate does for the document now.
    PromptPlan plan() const;
    // The line above the words.
    QString hint() const;
    QString prompt() const;
    // "Reuse prompt": the words and the negative prompt of an image.
    void setPrompt(const QString &prompt, const QString &negative = QString());
    void focusPrompt();

    // Generate (Enter in the words), Remove, "Make a variation" (what the main button and Enter do
    // when the engine cannot change the whole image from words) and Cancel. Without an engine the
    // main button and Enter ask for one (setUpAiRequested).
    void generate();
    void remove();
    void variation();
    void cancel();

    // The ⋯ menu: how much may change (when the request uses it), results per run, the negative
    // prompt (models that take one), reference images, the control pose (with pose control), the
    // engine, steps and seed under "Advanced", and "All AI options…".
    QWidget *optionsPopup() const { return m_popup; }
    void showOptions();

signals:
    // "All AI options…" and "Reference images…": the AI panel.
    void allOptionsRequested();
    // "Select an area": the rectangle selection tool.
    void selectAreaRequested();
    // "Set up AI…" while there is no image engine.
    void setUpAiRequested();

private:
    void buildOptions();
    void scheduleUpdate();
    void updateUi();
    void updateOptions();
    void updateGenerate();
    bool blank() const;
    // The engine cannot change the whole image from words but can make a variation of it.
    static bool offersVariation(const PromptPlan &p);
    void submit(const PromptPlan &plan, ai::GenMode mode, const QString &words, double strength, bool withCharacter, const QString &name);

    Studio *m_studio;
    AiOptions *m_options;
    AiPanel *m_ai;
    VariantStrip *m_strip;
    QPointer<doc::Document> m_doc;
    PromptPlan m_plan; // as of the last update
    QTimer m_update;
    // Whether the document was blank at its last revision (scanning the layers once per change).
    mutable QPointer<doc::Document> m_blankDoc;
    mutable quint64 m_blankRevision = 0;
    mutable bool m_blank = false;

    QLabel *m_icon;
    ElidedLabel *m_hint;
    QProgressBar *m_progress;
    QPushButton *m_selectArea;
    AutoTextEdit *m_prompt;
    QPushButton *m_generate;
    QPushButton *m_remove;
    QToolButton *m_more;

    QFrame *m_popup = nullptr;
    QFormLayout *m_form = nullptr;
    SliderField *m_strength = nullptr;
    QSpinBox *m_steps = nullptr;
    QWidget *m_seedRow = nullptr;
    QSpinBox *m_seed = nullptr;
    QCheckBox *m_randomSeed = nullptr;
    QSpinBox *m_variants = nullptr;
    AutoTextEdit *m_negative = nullptr;
    QPushButton *m_references = nullptr;
    QCheckBox *m_pose = nullptr;
    ProviderCombo *m_engine = nullptr;
    QToolButton *m_advanced = nullptr;
    QPushButton *m_allOptions = nullptr;
};

} // namespace lumen::ui
