#pragma once

#include <QString>
#include <QVariantMap>

namespace lumen::ai {

class Provider;

// What a model preset (runtime/ModelPresets.h) tells the image back-end that runs the model: its
// sampling defaults and what it can do. The studio hands it to the provider of a server Lumen runs
// as the provider's "model_preset" setting; 0 / empty values mean "the back-end's default".
struct ModelDefaults
{
    QString id;              // "flux2-klein-4b"; empty = no preset known
    QString name;            // "FLUX.2 [klein] 4B"
    double cfg = 0;
    double guidance = 0;     // distilled guidance (FLUX.1 dev)
    double imageCfg = 0;     // image guidance of edit models
    double flowShift = 0;
    int steps = 0;
    QString sampler;
    QString scheduler;
    QString dialect;         // "natural" or "tags"
    int nativeResolution = 0;
    bool generate = true;
    bool edit = false;       // instruction edits with reference images
    int references = 0;      // reference images the model takes (an edit's source counts)
    bool negativePrompt = true;

    bool isValid() const { return !id.isEmpty(); }
    QVariantMap toVariant() const;
    static ModelDefaults fromVariant(const QVariantMap &map);
};

// The preset of the model a provider's server runs (its "model_preset" setting).
ModelDefaults modelDefaultsOf(const Provider *provider);

} // namespace lumen::ai
