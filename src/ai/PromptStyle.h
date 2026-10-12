#pragma once

#include <QString>

namespace lumen::ai {

class ImageProvider;

// How prompts should be written for a given image back-end / model family.
struct PromptStyle
{
    enum class Dialect { Tags, Natural };
    Dialect dialect = Dialect::Natural;
    QString family;          // "pony", "illustrious", "sdxl", "sd15", "flux", "cloud"...
    QString qualityPrefix;   // prepended to positive prompts
    QString defaultNegative; // used when the user gives none
    bool supportsNegative = true;
    // A rating tag the model understands, added with "Safe for work" (Anima: "safe").
    QString safeTag;

    QString describeForLlm() const;
    QString apply(const QString &prompt) const;
    // The prompt with the safe rating tag after the quality tags, unless it has a rating already.
    QString withSafeTag(const QString &prompt) const;
};

// Guesses the prompt style from the provider type and the checkpoint/model name (and, for servers
// Lumen runs, the model family read from the file: "model_family").
PromptStyle promptStyleFor(const ImageProvider *provider, const QString &model = QString());

// A1111-compatible "parameters" info text (stored in PNG metadata).
QString buildInfoText(const QString &prompt, const QString &negative, int steps, const QString &sampler, const QString &scheduler,
                      double cfg, qint64 seed, int width, int height, const QString &model, double denoise = -1.0);

} // namespace lumen::ai
