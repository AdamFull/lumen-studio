#pragma once

#include "ai/RunSettings.h"

#include <QPointer>
#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QPushButton;
class QSpinBox;

namespace lumen::ai {
class ImageProvider;
}

namespace lumen::ui {

class AiOptions;

// The settings of a run that the engine's model takes (plan M18 §18.4), each starting at "Model's
// choice" with the model's own value shown, in plain words with the technical name in the tooltip:
// steps, how closely to follow the words (CFG; a note instead for a distilled model), FLUX.1 dev's
// guidance, image guidance, flow shift, sampler and scheduler, CLIP skip. Settings the model does not
// take are not shown. The values are kept per model type in AiOptions, so the prompt bar's ⋯ and the
// AI tab show the same ones.
class RunSettingsEditor : public QWidget
{
    Q_OBJECT
public:
    RunSettingsEditor(AiOptions *options, QWidget *parent = nullptr);

    void setEngine(ai::ImageProvider *provider);
    const ai::RunSettingsSpec &spec() const { return m_spec; }
    // Whether the engine's model has anything to set (or a note to show).
    bool hasSettings() const { return m_spec.any(); }

private:
    void showValues();
    void store();
    void fillLists();

    AiOptions *m_options;
    QPointer<ai::ImageProvider> m_engine;
    ai::RunSettingsSpec m_spec;
    QFormLayout *m_form;
    QLabel *m_model;
    QSpinBox *m_steps;
    QDoubleSpinBox *m_cfg;
    QLabel *m_distilled;
    QDoubleSpinBox *m_guidance;
    QDoubleSpinBox *m_imageCfg;
    QDoubleSpinBox *m_flowShift;
    QComboBox *m_sampler;
    QComboBox *m_scheduler;
    QSpinBox *m_clipSkip;
    QPushButton *m_reset;
    int m_listRequest = 0;
};

} // namespace lumen::ui
