#pragma once

#include "pose/Skeleton.h"

#include <QPointer>
#include <QSet>
#include <QWidget>

#include <optional>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;
class QVBoxLayout;

namespace lumen::ai {
struct GenerationSpec;
}

namespace lumen::ui {

class AutoTextEdit;
class CharacterChip;
class ProviderCombo;
class ReferenceStrip;
class SliderField;
class Studio;

// Classic generation workspace: prompt + parameters + references + pose → results grid.
class GeneratePage : public QWidget
{
    Q_OBJECT
public:
    explicit GeneratePage(Studio *studio, QWidget *parent = nullptr);

    void setPose(const std::optional<pose::Pose> &pose);

private:
    void generate();
    void enhancePrompt();
    void providerChanged();
    // The CFG slider takes the recommended value when the model family changes.
    void followRecommendedCfg();
    void updateSizeLabel();
    QSize requestedSize() const;
    void rerun(const QString &assetId);
    void addResult(const QString &assetId, const QImage &image, const QString &prompt);
    void submit(const ai::GenerationSpec &spec, const QString &prompt);
    // Tells once per provider and character what replaced a LoRA that could not be used.
    void noteFallback(const QString &note);
    void setInitImage(const QImage &image, const QString &assetId = QString());

    Studio *m_studio;
    ProviderCombo *m_provider;
    QComboBox *m_model;
    QString m_cfgFamily;        // the prompt style (and model preset) the CFG slider was last set for
    bool m_stepsFromPreset = false; // the steps slider shows a model preset's value
    AutoTextEdit *m_prompt;
    AutoTextEdit *m_negative;
    QButtonGroup *m_aspects;
    QSpinBox *m_width;
    QSpinBox *m_height;
    QCheckBox *m_customSize;
    QLabel *m_sizeLabel;
    QSpinBox *m_batch;
    QSpinBox *m_seed;
    QCheckBox *m_randomSeed;
    SliderField *m_steps;
    SliderField *m_cfg;
    QComboBox *m_sampler;
    QComboBox *m_scheduler;
    QComboBox *m_quality;
    QCheckBox *m_transparent;
    ReferenceStrip *m_refs;
    QLabel *m_poseThumb;
    SliderField *m_poseWeight;
    std::optional<pose::Pose> m_pose;
    QLabel *m_initThumb;
    QImage m_init;
    SliderField *m_denoise;
    QWidget *m_samplingBox;
    QPushButton *m_go;
    QVBoxLayout *m_jobs;
    QListWidget *m_results;
    QLabel *m_capsLabel;
    CharacterChip *m_character;
    QSet<QString> m_notes;
};

} // namespace lumen::ui
