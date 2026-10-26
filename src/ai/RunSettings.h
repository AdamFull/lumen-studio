#pragma once

#include <QJsonObject>
#include <QString>

namespace lumen::ai {

class ImageProvider;
struct ImageRequest;

// Which settings of a run an engine's model takes, and the model's own values (plan M18 §18.4), from
// its preset (ModelDefaults), its prompt style and the provider's capabilities. A setting the model
// does not take is not offered; a distilled model (CFG 1) is told apart, since raising its CFG only
// makes it slower and burns the image.
struct RunSettingsSpec
{
    QString key;              // what the user's choice is kept by: the model preset, else the engine
    QString modelName;        // the preset's name, if known ("FLUX.2 [klein] 4B")
    bool steps = false;
    int modelSteps = 0;       // 0 = the server's own
    bool cfg = false;         // how closely to follow the words can be set
    bool distilled = false;   // the model runs at CFG 1 (FLUX.1 dev / schnell, FLUX.2 klein)
    double modelCfg = 0;
    bool guidance = false;    // distilled guidance (FLUX.1 dev)
    double modelGuidance = 0;
    bool imageCfg = false;    // how closely an edit keeps the picture (edit models with a preset value)
    double modelImageCfg = 0;
    bool flowShift = false;   // flow models whose preset sets one (Qwen-Image)
    double modelFlowShift = 0;
    bool sampler = false;     // the server's samplers and schedulers
    QString modelSampler;
    QString modelScheduler;
    bool clipSkip = false;    // SD 1.5 and SDXL (Pony and Illustrious often want 2)

    bool any() const { return steps || cfg || distilled || guidance || imageCfg || flowShift || sampler || clipSkip; }
};
RunSettingsSpec runSettingsFor(const ImageProvider *provider);

// The values the user chose for a model; 0 / empty = the model's.
struct RunSettings
{
    int steps = 0;
    double cfg = 0;
    double guidance = 0;
    double imageCfg = 0;
    double flowShift = 0;
    QString sampler;
    QString scheduler;
    int clipSkip = 0;

    bool isEmpty() const { return *this == RunSettings(); }
    QJsonObject toJson() const;
    static RunSettings fromJson(const QJsonObject &o);
    bool operator==(const RunSettings &o) const = default;
};

// Puts the chosen values the model takes into a request (marking steps and CFG as chosen, so the
// model's recommendation does not replace them).
void applyRunSettings(ImageRequest &request, const RunSettings &settings, const RunSettingsSpec &spec);

} // namespace lumen::ai
