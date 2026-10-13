#include "ui/widgets/CharacterChip.h"

#include "ai/Types.h"
#include "core/Settings.h"
#include "doc/CharacterLibrary.h"
#include "ui/Icons.h"
#include "ui/Studio.h"

#include <QActionGroup>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>

namespace lumen::ui {

namespace {

QString settingKey(const QString &page)
{
    return QStringLiteral("character/use/") + page;
}

// A round thumbnail for the button and the menu.
QIcon roundIcon(const QImage &thumbnail)
{
    if (thumbnail.isNull())
        return Icons::get(QStringLiteral("person"));
    const int size = 64;
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addEllipse(0, 0, size, size);
    p.setClipPath(clip);
    const QImage scaled = thumbnail.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    p.drawImage((size - scaled.width()) / 2, 0, scaled);
    return QIcon(pm);
}

// Two round thumbnails, the second overlapping the first.
QIcon pairIcon(const QImage &first, const QImage &second)
{
    const int size = 64;
    QPixmap pm(size * 3 / 2, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.drawPixmap(size / 2, 0, roundIcon(second).pixmap(size, size));
    p.drawPixmap(0, 0, roundIcon(first).pixmap(size, size));
    return QIcon(pm);
}

} // namespace

CharacterChip::CharacterChip(Studio *studio, const QString &pageKey, QWidget *parent)
    : QToolButton(parent)
    , m_studio(studio)
    , m_key(pageKey)
    , m_menu(new QMenu(this))
{
    m_used = Settings::instance().value(settingKey(m_key), true).toBool();
    setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    setPopupMode(QToolButton::InstantPopup);
    setIconSize(QSize(20, 20));
    setMenu(m_menu);
    connect(m_menu, &QMenu::aboutToShow, this, &CharacterChip::rebuildMenu);
    connect(studio, &Studio::activeCharacterChanged, this, [this] {
        refresh();
        emit characterChanged();
    });
    // A new LoRA, name or thumbnail of the active character.
    connect(studio->characters(), &doc::CharacterLibrary::changed, this, [this] {
        refresh();
        if (!m_studio->activeCharacter().isEmpty())
            emit characterChanged();
    });
    refresh();
}

QString CharacterChip::characterId() const
{
    return m_used ? m_studio->activeCharacter() : QString();
}

QStringList CharacterChip::castIds() const
{
    return m_used ? m_studio->activeCast() : QStringList();
}

bool CharacterChip::isUsedHere() const
{
    return m_used;
}

void CharacterChip::setUsedHere(bool used)
{
    if (used == m_used)
        return;
    m_used = used;
    Settings::instance().setValue(settingKey(m_key), used);
    refresh();
    emit characterChanged();
}

void CharacterChip::refresh()
{
    const QString id = m_studio->activeCharacter();
    const auto entry = id.isEmpty() ? std::nullopt : m_studio->characters()->entry(id);
    const QStringList cast = m_studio->activeCast();
    const auto second = cast.size() > 1 ? m_studio->characters()->entry(cast[1]) : std::nullopt;
    QFont f = font();
    f.setStrikeOut(entry && !m_used);
    setFont(f);
    if (entry && second) {
        auto withLora = [](const doc::CharacterEntry &e) {
            return ai::CharacterLora::fromJson(e.lora).isValid() ? tr("%1 (LoRA)").arg(e.name) : e.name;
        };
        setIcon(pairIcon(entry->thumbnail, second->thumbnail));
        setIconSize(QSize(30, 20));
        setText(tr("%1 & %2").arg(entry->name, second->name));
        setToolTip(m_used ? tr("The cast: %1 on the left, %2 on the right").arg(withLora(*entry), withLora(*second))
                          : tr("%1 and %2 are the project's cast, but switched off on this page").arg(entry->name, second->name));
        return;
    }
    setIconSize(QSize(20, 20));
    if (!entry) {
        setIcon(Icons::get(QStringLiteral("person")));
        setText(tr("No character"));
        setToolTip(tr("Choose a character from the character library; image tools then draw it without repeating it in every prompt"));
        return;
    }
    const bool lora = ai::CharacterLora::fromJson(entry->lora).isValid();
    setIcon(roundIcon(entry->thumbnail));
    setText(lora ? tr("%1 · LoRA").arg(entry->name) : entry->name);
    setToolTip(m_used ? (lora ? tr("Images here draw %1 with the character's LoRA when the image generator can load it, else with "
                                   "the character's reference image or description")
                                    .arg(entry->name)
                              : tr("Images here draw %1 with the character's reference image or description").arg(entry->name))
                      : tr("%1 is the project's character, but switched off on this page").arg(entry->name));
}

void CharacterChip::rebuildMenu()
{
    m_menu->clear();
    const QString active = m_studio->activeCharacter();
    const QStringList cast = m_studio->activeCast();
    auto *group = new QActionGroup(m_menu);
    group->setExclusive(cast.size() < 2); // both members of a cast of two show a tick
    const QList<doc::CharacterEntry> entries = m_studio->characters()->entries();
    auto label =[](const doc::CharacterEntry &e) {
        return ai::CharacterLora::fromJson(e.lora).isValid() ? tr("%1 · LoRA").arg(e.name) : e.name;
    };
    for (const doc::CharacterEntry &e : entries) {
        QAction *a = m_menu->addAction(roundIcon(e.thumbnail), label(e));
        a->setCheckable(true);
        a->setChecked(cast.contains(e.id));
        group->addAction(a);
        const QString id = e.id;
        connect(a, &QAction::triggered, this, [this, id] { m_studio->setActiveCharacter(id); });
    }
    if (entries.isEmpty()) {
        QAction *none = m_menu->addAction(tr("No characters in the library yet. Save one on the Create page."));
        none->setEnabled(false);
    }
    // A second character (M16): added on the right; the two can swap sides or leave.
    if (!cast.isEmpty()) {
        m_menu->addSeparator();
        QMenu *add = m_menu->addMenu(Icons::get(QStringLiteral("plus")), cast.size() < 2 ? tr("Add a second character") : tr("Replace the second character"));
        for (const doc::CharacterEntry &e : entries) {
            if (e.id == cast.first())
                continue;
            QAction *a = add->addAction(roundIcon(e.thumbnail), label(e));
            const QString id = e.id;
            connect(a, &QAction::triggered, this, [this, id] { m_studio->setActiveCast({m_studio->activeCharacter(), id}); });
        }
        add->setEnabled(!add->actions().isEmpty());
        if (cast.size() > 1) {
            QAction *swap = m_menu->addAction(tr("Swap left and right"));
            connect(swap, &QAction::triggered, this, [this] {
                const QStringList c = m_studio->activeCast();
                m_studio->setActiveCast({c.value(1), c.value(0)});
            });
            for (const QString &id : cast) {
                const auto e = m_studio->characters()->entry(id);
                QAction *remove = m_menu->addAction(Icons::get(QStringLiteral("minus")), tr("Remove %1").arg(e ? e->name : id));
                connect(remove, &QAction::triggered, this, [this, id] {
                    QStringList c = m_studio->activeCast();
                    c.removeAll(id);
                    m_studio->setActiveCast(c);
                });
            }
        }
    }
    m_menu->addSeparator();
    QAction *none = m_menu->addAction(tr("No character"));
    none->setCheckable(true);
    none->setChecked(active.isEmpty());
    group->addAction(none);
    connect(none, &QAction::triggered, this, [this] { m_studio->setActiveCharacter(QString()); });
    m_menu->addSeparator();
    QAction *use = m_menu->addAction(cast.size() > 1 ? tr("Use the characters on this page") : tr("Use the character on this page"));
    use->setCheckable(true);
    use->setChecked(m_used);
    use->setEnabled(!active.isEmpty());
    connect(use, &QAction::toggled, this, &CharacterChip::setUsedHere);
}

} // namespace lumen::ui
