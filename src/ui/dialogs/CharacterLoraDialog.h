#pragma once

#include "ai/Types.h"

#include <QDialog>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;

namespace lumen::ai {
class ImageProvider;
}

namespace lumen::ui {

class Studio;

// What stops a character LoRA from working with an image provider: the provider cannot use
// LoRAs, the file is gone, the LoRA belongs to another model family than the provider's
// checkpoint, or a Lumen-run sd.cpp server does not look in the LoRA's folder. Empty when fine.
struct LoraCheck
{
    QString problem;         // shown to the user
    bool fixableFolder = false; // the server's LoRA folder can be pointed at the file's folder
};
LoraCheck checkCharacterLora(Studio *studio, ai::ImageProvider *provider, const ai::CharacterLora &lora);
// Points the Lumen-run sd.cpp server behind `provider` at the LoRA's folder (restarts it if it runs).
bool useLoraFolder(Studio *studio, ai::ImageProvider *provider, const QString &loraFile);
// Base model family of a model file ("SDXL", "SD 1.5", …), from the library or its header.
QString modelFamily(Studio *studio, const QString &file);
// Family of the checkpoint an image provider generates with; empty when unknown.
QString checkpointFamily(Studio *studio, ai::ImageProvider *provider);
// The trigger word a LoRA was trained with (its metadata, Civitai or the download's notes).
QString suggestedTrigger(Studio *studio, const QString &file);

// Attaches a LoRA to a character: the file (from the model library or anywhere), its trigger word
// and weight. Shows whether it works with the current image provider.
class CharacterLoraDialog : public QDialog
{
    Q_OBJECT
public:
    CharacterLoraDialog(Studio *studio, ai::ImageProvider *provider, const ai::CharacterLora &current, QWidget *parent = nullptr);

    ai::CharacterLora lora() const;
    bool detached() const { return m_detached; }

private:
    void fileChanged();
    void updateCheck();

    Studio *m_studio;
    ai::ImageProvider *m_provider;
    QComboBox *m_file;
    QLineEdit *m_trigger;
    QDoubleSpinBox *m_weight;
    QLabel *m_family;
    QLabel *m_check;
    QString m_lastSuggested;
    bool m_detached = false;
};

} // namespace lumen::ui
