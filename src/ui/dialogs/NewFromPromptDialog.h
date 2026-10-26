#pragma once

#include "pose/Skeleton.h"

#include <QDialog>

#include <optional>

class QButtonGroup;
class QCheckBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QToolButton;

namespace lumen {
class Job;
}

namespace lumen::doc {
class Document;
struct AssetInfo;
} // namespace lumen::doc

namespace lumen::ui {

class AutoTextEdit;
class ProviderCombo;
class Studio;

// File → New from prompt: a new document of the chosen size, drawn from a description by the image
// engine. The results are chosen in the editor's variants strip (generationStarted); without anyone
// listening they become layers of the new document (all variants but the first hidden).
class NewFromPromptDialog : public QDialog
{
    Q_OBJECT
public:
    explicit NewFromPromptDialog(Studio *studio, QWidget *parent = nullptr);

    void setPrompt(const QString &prompt, const QString &negative = QString());
    // The prompt, negative prompt, size and seed an image was made with ("Re-run").
    void fillFrom(const doc::AssetInfo &asset);
    // The new document's control pose ("New image with this pose"); the engine follows it when it
    // can. The size switches to the aspect ratio closest to the pose's canvas.
    void setPose(const std::optional<pose::Pose> &pose);
    QSize requestedSize() const;
    // Makes the document and starts the generation into it. Null when nothing can be made (no
    // image engine, no prompt). Create does this and closes the dialog.
    doc::Document *create();

signals:
    // The new document and the job drawing `results` images into it; `name` is the result layer's.
    void generationStarted(lumen::doc::Document *doc, lumen::Job *job, const QString &name, int results);

private:
    void updateUi();
    void updateSize();
    void showMore(bool on);
    bool takesNegative() const;

    Studio *m_studio;
    AutoTextEdit *m_prompt;
    QButtonGroup *m_aspects;
    QCheckBox *m_custom;
    QSpinBox *m_width;
    QSpinBox *m_height;
    QLabel *m_sizeLabel;
    QSpinBox *m_variants;
    ProviderCombo *m_engine;
    QLabel *m_engineNote;
    QToolButton *m_moreButton;
    QWidget *m_more;
    QWidget *m_negativeBox;
    AutoTextEdit *m_negative;
    QSpinBox *m_seed;
    QCheckBox *m_randomSeed;
    QWidget *m_poseBox;
    QLabel *m_poseThumb;
    QLabel *m_poseNote;
    std::optional<pose::Pose> m_pose;
    QPushButton *m_create;
};

} // namespace lumen::ui
