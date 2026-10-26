#pragma once

#include "ai/CharacterContext.h"
#include "ai/GenerationService.h"
#include "doc/Document.h"

#include <QPointer>
#include <QSet>
#include <QWidget>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QVBoxLayout;

namespace lumen::ui {

class AutoTextEdit;
class CharacterChip;
class ProviderCombo;
class ReferenceStrip;
class SliderField;
class Studio;

// Generative editing panel of the editor: inpaint selection, variations, instruction edits,
// outpainting, upscaling, background removal and pose detection on the active document.
class AiPanel : public QWidget
{
    Q_OBJECT
public:
    enum class Mode { Inpaint, Variation, Edit, Expand, Upscale, RemoveBackground, DetectPose };

    explicit AiPanel(Studio *studio, QWidget *parent = nullptr);
    void setDocument(doc::Document *doc);
    void setMode(Mode mode);
    // Runs an action with this prompt (the contextual canvas toolbar); the panel's other options apply.
    // `withCharacter` false keeps the project's character out (removing an object).
    void runFromCanvas(Mode mode, const QString &prompt, bool withCharacter = true);
    QString prompt() const;
    int runningJobs() const { return m_running; }
    ReferenceStrip *references() const { return m_refs; }

    // Adds generated images to a document as new layers (scaled to its size) in one undo step;
    // only the first stays visible, the others are variants to compare.
    static void addResultLayers(Studio *studio, doc::Document *doc, const QList<QImage> &images, const QString &name,
                                const QJsonObject &meta);

signals:
    void runningJobsChanged(int count);

private:
    void run();
    void updateUi();
    void submitWithComposite(std::function<void(QImage composite)> fn);
    // Adds the page's character (if any) to the request for this use, then continues with the
    // request and the metadata of what went in (empty without a character).
    void withCharacter(ai::ImageRequest request, ai::CharacterUse use, std::function<void(ai::ImageRequest, QJsonObject)> then);
    void trackJob(Job *job, std::function<void(doc::Document *, const ai::GenerationOutput &)> apply);
    void addResultLayers(doc::Document *doc, const QList<QImage> &images, const QString &name, const QJsonObject &meta)
    {
        addResultLayers(m_studio, doc, images, name, meta);
    }
    ai::ImageRequest baseRequest() const;

    Studio *m_studio;
    QPointer<doc::Document> m_doc;
    Mode m_mode = Mode::Inpaint;
    ProviderCombo *m_provider;
    QButtonGroup *m_modes;
    QLabel *m_modeHint;
    AutoTextEdit *m_prompt;
    AutoTextEdit *m_negative;
    QWidget *m_promptBox;
    SliderField *m_strength;
    QWidget *m_expandBox;
    QCheckBox *m_expandLeft, *m_expandRight, *m_expandTop, *m_expandBottom;
    QSpinBox *m_expandAmount;
    QComboBox *m_upscaleFactor;
    QWidget *m_upscaleBox;
    ReferenceStrip *m_refs;
    QWidget *m_refsBox;
    QCheckBox *m_usePose;
    QSpinBox *m_variants;
    SliderField *m_padding;
    SliderField *m_feather;
    QCheckBox *m_colorMatch;
    QPushButton *m_run;
    QVBoxLayout *m_jobs;
    QLabel *m_capsWarning;
    int m_running = 0;
    CharacterChip *m_character;
    QSet<QString> m_notes;
    bool m_skipCharacter = false;
};

} // namespace lumen::ui
