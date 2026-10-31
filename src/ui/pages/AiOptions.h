#pragma once

#include "ai/RunSettings.h"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

namespace lumen::ai {
class ImageProvider;
class ProviderRegistry;
struct LoraRef;
} // namespace lumen::ai

namespace lumen::ui {

// What Use my words as they are does, for its tooltip.
QString literalWordsHelp();

// A LoRA chosen in the editor for an image engine (plan M18 §18.3): what its server calls it, the
// file on this computer when Lumen knows it, how strongly it applies, whether it is used, and the
// words that call it up (added to the description when they are not in it).
struct ChosenLora
{
    QString name;           // the server's name for it (file name without extension)
    QString path;           // relative to the server's LoRA folder ("styles/ink.safetensors")
    QString file;           // full path on this computer; empty for a server elsewhere
    double weight = 0.8;
    bool on = true;
    QStringList triggerWords;

    QJsonObject toJson() const;
    static ChosenLora fromJson(const QJsonObject &o);
    bool operator==(const ChosenLora &o) const = default;
};

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
    // The image engine chosen in the editor for runs that change a picture (`changes`: the ⋯ menu
    // on a picture, the AI panel) or make a new one (the ⋯ menu on a blank canvas, New from
    // prompt); empty = the one Settings → AI names for that use (imageEngineInUse).
    QString providerId(bool changes) const { return m_providerId[changes ? 1 : 0]; }
    void setProviderId(bool changes, const QString &id);
    // The chosen engine while it is on, else the one in use for a run that changes the picture
    // (`changes`: the engine for changes) or makes a new one (the image engine); null without one.
    ai::ImageProvider *provider(const ai::ProviderRegistry *registry, bool changes) const;
    // The last choice wins: a new engine for a use in Settings → AI (or one the setup wizard sets
    // up) replaces the editor's choice for that use.
    void followSettings(const ai::ProviderRegistry *registry);
    // How much a selected area may change when it is inpainted or redrawn, and how much a variation of
    // the whole image may (the denoising strength). Kept apart: a variation wants far less.
    double regionStrength() const { return m_regionStrength; }
    void setRegionStrength(double strength);
    // How much an instruction edit may change, for engines that take it (1 = all of it, as the model
    // likes; lower keeps more of the picture). Remembered.
    double editStrength() const { return m_editStrength; }
    void setEditStrength(double strength);
    double variationStrength() const { return m_variationStrength; }
    void setVariationStrength(double strength);
    // The settings chosen for a model (steps, how closely to follow the words, sampler…), kept by
    // its RunSettingsSpec::key (the model preset, else the engine) for the next session; empty = the
    // model's own values.
    ai::RunSettings runSettings(const QString &key) const { return m_runSettings.value(key); }
    void setRunSettings(const QString &key, const ai::RunSettings &settings);
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
    // Use my words as they are (plan M18 §18.2): no quality words, no default negative prompt, and
    // an instruction is not turned into a description for inpainting. Remembered.
    bool literalWords() const { return m_literalWords; }
    void setLiteralWords(bool literal);
    // The region pipeline: the surroundings sent along with a selection, the width of the blended
    // edge (document pixels) and colour matching to the surroundings.
    int padding() const { return m_padding; }
    void setPadding(int pixels);
    int feather() const { return m_feather; }
    void setFeather(int pixels);
    bool colorMatch() const { return m_colorMatch; }
    void setColorMatch(bool on);
    // The LoRAs chosen for an engine, remembered for the next session. A request takes the ones that
    // are on (loraRefs) and their trigger words (withTriggerWords).
    QList<ChosenLora> loras(const QString &providerId) const { return m_loras.value(providerId); }
    void setLoras(const QString &providerId, const QList<ChosenLora> &loras);
    QList<ai::LoraRef> loraRefs(const QString &providerId) const;
    // `prompt` with the trigger words of the engine's LoRAs that are on and missing from it.
    QString withTriggerWords(const QString &providerId, const QString &prompt) const;

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
    QString m_providerId[2];   // new images, changes
    QString m_settingsEngine[2]; // what Settings → AI named for each use when last looked at
    double m_regionStrength = 0.75;
    double m_editStrength = 1.0;
    double m_variationStrength = 0.45;
    qint64 m_seed = -1;
    int m_variants = 2;
    QString m_negative;
    bool m_usePose = true;
    bool m_literalWords = false;
    int m_padding = 64;
    int m_feather = 16;
    bool m_colorMatch = true;
    QHash<QString, QList<ChosenLora>> m_loras;
    QHash<QString, ai::RunSettings> m_runSettings;
};

} // namespace lumen::ui
