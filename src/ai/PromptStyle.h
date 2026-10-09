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

    QString describeForLlm() const;
    QString apply(const QString &prompt) const;
};

// Guesses the prompt style from the provider type and the checkpoint/model name.
PromptStyle promptStyleFor(const ImageProvider *provider, const QString &model = QString());

// A1111-compatible "parameters" info text (stored in PNG metadata).
QString buildInfoText(const QString &prompt, const QString &negative, int steps, const QString &sampler, const QString &scheduler,
                      double cfg, qint64 seed, int width, int height, const QString &model, double denoise = -1.0);

} // namespace lumen::ai
