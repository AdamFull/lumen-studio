#pragma once

#include <QToolButton>

class QAction;
class QMenu;

namespace lumen::ui {

class Studio;

// The project's cast on an image page (M14 §14.2, M16 §16.3): thumbnails, names and a LoRA badge.
// Its menu makes a library character active (or none), adds a second character, swaps or removes
// one, and switches the cast off for this page only; the switch is remembered per page.
class CharacterChip : public QToolButton
{
    Q_OBJECT
public:
    // `pageKey` names the page for its switch ("generate", "editor", "parts").
    CharacterChip(Studio *studio, const QString &pageKey, QWidget *parent = nullptr);

    // The character this page draws: the first of the cast unless switched off here; empty otherwise.
    QString characterId() const;
    // The whole cast this page draws (one or two), unless switched off here.
    QStringList castIds() const;
    bool isUsedHere() const;
    void setUsedHere(bool used);

signals:
    // The active character, its LoRA or this page's switch changed.
    void characterChanged();

private:
    void refresh();
    void rebuildMenu();

    Studio *m_studio;
    QString m_key;
    QMenu *m_menu;
    bool m_used = true;
};

} // namespace lumen::ui
