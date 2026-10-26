#pragma once

#include "ui/LoraCatalog.h"
#include "ui/pages/AiOptions.h"

#include <QPointer>
#include <QWidget>

class QLabel;
class QPushButton;
class QToolButton;
class QVBoxLayout;

namespace lumen::ai {
class ImageProvider;
}

namespace lumen::ui {

class Studio;

// The LoRAs a run uses, chosen by hand instead of typed into the description (plan M18 §18.3): one
// row per chosen LoRA with a tick box, its weight and a remove button; "Add a LoRA…" lists the
// engine's LoRAs, those made for its model first, the others greyed out with what they were made
// for. A LoRA that does not fit the engine's model is never used, and its row says why. The choice
// is kept per engine in AiOptions.
class LoraPicker : public QWidget
{
    Q_OBJECT
public:
    LoraPicker(Studio *studio, AiOptions *options, QWidget *parent = nullptr);

    // The engine runs go to; its LoRAs are listed again when it changes. Null = none.
    void setEngine(ai::ImageProvider *provider);
    ai::ImageProvider *engine() const { return m_engine; }
    // Lists the engine's LoRAs again (a file added to the folder, a new server setting).
    void refresh();

    const LoraListing &listing() const { return m_listing; }
    QToolButton *addButton() const { return m_add; }

signals:
    // A listing arrived and the rows show it.
    void listed();

private:
    void rebuild();
    void fillMenu();
    void add(const LoraChoice &choice);
    // The listing's entry for a chosen LoRA (by its path, else its name); null if not listed.
    const LoraChoice *find(const ChosenLora &l) const;
    // Turns off chosen LoRAs that the listing says do not fit.
    void enforceFit();

    Studio *m_studio;
    AiOptions *m_options;
    QPointer<ai::ImageProvider> m_engine;
    QString m_engineId;
    LoraListing m_listing;
    bool m_haveListing = false;
    int m_request = 0;
    QVBoxLayout *m_rows;
    QLabel *m_status;
    QToolButton *m_add;
    QPushButton *m_settings;
};

} // namespace lumen::ui
