#pragma once

#include <QObject>
#include <QString>

namespace lumen::ai {
class ImageProvider;
class ProviderRegistry;
} // namespace lumen::ai

namespace lumen::ui {

// The editor's AI settings, shared by the prompt bar's ⋯ menu and the AI panel: one value each,
// whichever of the two changes it. The prompt bar's words are here too, for the AI panel's actions
// that take a description (variation, expand canvas).
class AiOptions : public QObject
{
    Q_OBJECT
public:
    explicit AiOptions(QObject *parent = nullptr);

    static constexpr int kMaxVariants = 4;

    QString prompt() const { return m_prompt; }
    void setPrompt(const QString &prompt);
    // The image engine chosen in the editor (the ⋯ menu or the AI panel), for every run; empty = the
    // one Settings → AI names (imageEngineInUse).
    QString providerId() const { return m_providerId; }
    void setProviderId(const QString &id);
    // The chosen engine while it is on, else the one in use for a run that changes the picture
    // (`changes`: the engine for changes) or makes a new one (the image engine); null without one.
    ai::ImageProvider *provider(const ai::ProviderRegistry *registry, bool changes) const;
    // How much a selected area may change when it is inpainted or redrawn, and how much a variation of
    // the whole image may (the denoising strength). Kept apart: a variation wants far less.
    double regionStrength() const { return m_regionStrength; }
    void setRegionStrength(double strength);
    double variationStrength() const { return m_variationStrength; }
    void setVariationStrength(double strength);
    // 0 = the model's (its preset, else the provider's).
    int steps() const { return m_steps; }
    void setSteps(int steps);
    // -1 = a new random seed for every run.
    qint64 seed() const { return m_seed; }
    void setSeed(qint64 seed);
    // 1 … kMaxVariants; remembered for the next session.
    int variants() const { return m_variants; }
    void setVariants(int variants);
    // Only sent to models that take a negative prompt.
    QString negative() const { return m_negative; }
    void setNegative(const QString &negative);
    // The document's control pose goes along when the engine has pose control.
    bool usePose() const { return m_usePose; }
    void setUsePose(bool use);
    // The region pipeline: the surroundings sent along with a selection, the width of the blended
    // edge (document pixels) and colour matching to the surroundings.
    int padding() const { return m_padding; }
    void setPadding(int pixels);
    int feather() const { return m_feather; }
    void setFeather(int pixels);
    bool colorMatch() const { return m_colorMatch; }
    void setColorMatch(bool on);

signals:
    // An option changed (not the words, which change with every key).
    void changed();
    void promptChanged();

private:
    template<typename T>
    void set(T &member, const T &value)
    {
        if (member == value)
            return;
        member = value;
        emit changed();
    }

    QString m_prompt;
    QString m_providerId;
    double m_regionStrength = 0.75;
    double m_variationStrength = 0.45;
    int m_steps = 0;
    qint64 m_seed = -1;
    int m_variants = 2;
    QString m_negative;
    bool m_usePose = true;
    int m_padding = 64;
    int m_feather = 16;
    bool m_colorMatch = true;
};

} // namespace lumen::ui
