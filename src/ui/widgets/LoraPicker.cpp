#include "ui/widgets/LoraPicker.h"

#include "ai/Provider.h"
#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/pages/AiOptions.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace lumen::ui {

LoraPicker::LoraPicker(Studio *studio, AiOptions *options, QWidget *parent)
    : QWidget(parent)
    , m_studio(studio)
    , m_options(options)
{
    setObjectName(QStringLiteral("loraPicker"));
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);
    m_rows = new QVBoxLayout;
    m_rows->setContentsMargins(0, 0, 0, 0);
    m_rows->setSpacing(2);
    v->addLayout(m_rows);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("loraStatus"));
    m_status->setWordWrap(true);
    m_status->setProperty("role", QStringLiteral("muted"));
    v->addWidget(m_status);
    auto *buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 0, 0, 0);
    m_add = new QToolButton(this);
    m_add->setObjectName(QStringLiteral("loraAdd"));
    m_add->setText(tr("Add a LoRA…"));
    m_add->setIcon(Icons::get(QStringLiteral("plus")));
    m_add->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_add->setPopupMode(QToolButton::InstantPopup);
    m_add->setToolTip(tr("Small add-ons to the model for a style, a character or a look. Only those made for the engine's model can "
                         "be used."));
    m_add->setMenu(new QMenu(m_add));
    connect(m_add->menu(), &QMenu::aboutToShow, this, &LoraPicker::fillMenu);
    m_settings = new QPushButton(tr("Server settings…"), this);
    m_settings->setObjectName(QStringLiteral("loraServerSettings"));
    m_settings->setToolTip(tr("Settings → Local servers, where the server's LoRA folder is chosen"));
    connect(m_settings, &QPushButton::clicked, this, [this] { emit m_studio->settingsRequested(QStringLiteral("servers")); });
    buttons->addWidget(m_add);
    buttons->addWidget(m_settings);
    buttons->addStretch(1);
    v->addLayout(buttons);
    connect(m_options, &AiOptions::changed, this, &LoraPicker::rebuild);
    rebuild();
}

void LoraPicker::setEngine(ai::ImageProvider *provider)
{
    const QString id = provider ? provider->instanceId() : QString();
    if (provider == m_engine && id == m_engineId && m_haveListing)
        return;
    m_engine = provider;
    m_engineId = id;
    refresh();
}

void LoraPicker::refresh()
{
    m_haveListing = false;
    m_listing = {};
    const int request = ++m_request;
    rebuild();
    QPointer<LoraPicker> self(this);
    listLoras(m_studio, m_engine).then(this, [self, request](const LoraListing &listing) {
        if (!self || request != self->m_request)
            return;
        self->m_listing = listing;
        self->m_haveListing = true;
        self->enforceFit();
        self->rebuild();
        emit self->listed();
    });
}

void LoraPicker::setWords(const QString &words)
{
    const QString simplified = words.simplified();
    if (simplified == m_words)
        return;
    m_words = simplified;
    rebuild();
}

const LoraChoice *LoraPicker::find(const ChosenLora &l) const
{
    for (const LoraChoice &c : m_listing.loras) {
        if (!l.path.isEmpty() && c.path.compare(l.path, Qt::CaseInsensitive) == 0)
            return &c;
    }
    for (const LoraChoice &c : m_listing.loras) {
        if (c.name.compare(l.name, Qt::CaseInsensitive) == 0)
            return &c;
    }
    return nullptr;
}

void LoraPicker::enforceFit()
{
    if (m_engineId.isEmpty())
        return;
    QList<ChosenLora> chosen = m_options->loras(m_engineId);
    bool changed = false;
    for (ChosenLora &l : chosen) {
        const LoraChoice *c = find(l);
        if (l.on && c && !c->mismatch.isEmpty()) {
            l.on = false;
            changed = true;
        }
    }
    if (changed)
        m_options->setLoras(m_engineId, chosen);
}

void LoraPicker::rebuild()
{
    // A row's box or field usually sent the change that rebuilds them: it is still in its signal
    // (a click goes on after the toggle), so the rows go later, out of sight and out of findChild.
    while (QLayoutItem *item = m_rows->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            w->hide();
            w->setParent(nullptr);
            w->deleteLater();
        }
        delete item;
    }
    const QList<ChosenLora> chosen = m_engineId.isEmpty() ? QList<ChosenLora>() : m_options->loras(m_engineId);
    for (int i = 0; i < chosen.size(); ++i) {
        const ChosenLora &l = chosen.at(i);
        const LoraChoice *listed = m_haveListing ? find(l) : nullptr;
        // What keeps it from being used: made for another model, or gone from the folder.
        QString reason;
        if (listed && !listed->mismatch.isEmpty())
            reason = listed->mismatch;
        else if (m_haveListing && !listed && m_listing.problem.isEmpty())
            reason = tr("It is not in the server's LoRAs any more.");
        auto *row = new QWidget(this);
        row->setObjectName(QStringLiteral("loraRow"));
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        auto *on = new QCheckBox(l.name, row);
        on->setObjectName(QStringLiteral("loraOn"));
        on->setChecked(l.on && reason.isEmpty());
        on->setEnabled(reason.isEmpty());
        QString tip = reason.isEmpty() ? tr("Used in the next runs while ticked.") : reason;
        if (!l.triggerWords.isEmpty())
            tip += QLatin1Char('\n') + tr("Words that call it up (added to the description): %1").arg(l.triggerWords.join(QStringLiteral(", ")));
        on->setToolTip(tip);
        auto *weight = new QDoubleSpinBox(row);
        weight->setObjectName(QStringLiteral("loraWeight"));
        weight->setAccessibleName(tr("Strength of %1").arg(l.name));
        weight->setRange(-2.0, 2.0);
        weight->setSingleStep(0.05);
        weight->setDecimals(2);
        weight->setValue(l.weight);
        weight->setToolTip(tr("How strongly it applies: 1 is full strength; 0.6 to 1 suits most LoRAs."));
        auto *remove = new QToolButton(row);
        remove->setObjectName(QStringLiteral("loraRemove"));
        remove->setIcon(Icons::get(QStringLiteral("close")));
        remove->setAutoRaise(true);
        remove->setToolTip(tr("Remove %1").arg(l.name));
        remove->setAccessibleName(tr("Remove %1").arg(l.name));
        h->addWidget(on, 1);
        h->addWidget(weight);
        h->addWidget(remove);
        m_rows->addWidget(row);
        if (!reason.isEmpty()) {
            auto *why = new QLabel(reason, this);
            why->setObjectName(QStringLiteral("loraReason"));
            why->setWordWrap(true);
            why->setProperty("role", QStringLiteral("error"));
            m_rows->addWidget(why);
        }
        const QString id = m_engineId;
        connect(on, &QCheckBox::toggled, this, [this, id, i](bool checked) {
            QList<ChosenLora> list = m_options->loras(id);
            if (i < list.size()) {
                list[i].on = checked;
                m_options->setLoras(id, list);
            }
        });
        // The value, not every keystroke, so the rows are not rebuilt under the cursor.
        connect(weight, &QDoubleSpinBox::editingFinished, this, [this, id, i, weight] {
            QList<ChosenLora> list = m_options->loras(id);
            if (i < list.size() && !qFuzzyCompare(list[i].weight, weight->value())) {
                list[i].weight = weight->value();
                m_options->setLoras(id, list);
            }
        });
        connect(remove, &QToolButton::clicked, this, [this, id, i] {
            QList<ChosenLora> list = m_options->loras(id);
            if (i < list.size()) {
                list.removeAt(i);
                // Later: the rows are rebuilt from the options, and this button is in one of them.
                QMetaObject::invokeMethod(this, [this, id, list] { m_options->setLoras(id, list); }, Qt::QueuedConnection);
            }
        });
    }

    // Suggested for the words (plan M18 §18.3): never used until ticked.
    if (m_haveListing && !m_words.isEmpty()) {
        QList<LoraChoice> suggested;
        for (const LoraChoice &c : suggestedLoras(m_listing.loras, m_words, 6)) {
            bool taken = false;
            for (const ChosenLora &l : chosen)
                taken = taken || (!l.path.isEmpty() && l.path.compare(c.path, Qt::CaseInsensitive) == 0)
                        || l.name.compare(c.name, Qt::CaseInsensitive) == 0;
            if (!taken && suggested.size() < 3)
                suggested << c;
        }
        if (!suggested.isEmpty()) {
            auto *title = new QLabel(tr("Suggested for your words:"), this);
            title->setObjectName(QStringLiteral("loraSuggestedTitle"));
            title->setProperty("role", QStringLiteral("muted"));
            m_rows->addWidget(title);
        }
        for (const LoraChoice &c : std::as_const(suggested)) {
            auto *box = new QCheckBox(c.name, this);
            box->setObjectName(QStringLiteral("loraSuggestion"));
            QString tip = tr("Its name matches your words. Tick it to use it.");
            if (!c.info.triggerWords.isEmpty())
                tip += QLatin1Char('\n') + tr("Words that call it up (added to the description): %1").arg(c.info.triggerWords.join(QStringLiteral(", ")));
            box->setToolTip(tip);
            m_rows->addWidget(box);
            // Later: the rows are rebuilt from the options, and this box is one of them.
            connect(box, &QCheckBox::toggled, this, [this, c](bool on) {
                if (on)
                    QMetaObject::invokeMethod(this, [this, c] { add(c); }, Qt::QueuedConnection);
            });
        }
    }

    QString status;
    if (!m_engine)
        status = tr("There is no image engine to use LoRAs with.");
    else if (!m_haveListing)
        status = tr("Looking at the engine's LoRAs…");
    else if (!m_listing.problem.isEmpty())
        status = m_listing.problem;
    else if (chosen.isEmpty())
        status = m_listing.model.isEmpty() ? tr("None chosen.") : tr("None chosen. The engine runs %1.").arg(m_listing.model);
    m_status->setText(status);
    m_status->setVisible(!status.isEmpty());
    m_add->setEnabled(m_haveListing && !m_listing.loras.isEmpty());
    m_settings->setVisible(m_haveListing && !m_listing.profileId.isEmpty() && m_listing.loras.isEmpty());
}

void LoraPicker::fillMenu()
{
    QMenu *menu = m_add->menu();
    menu->clear();
    menu->setToolTipsVisible(true);
    const QList<ChosenLora> chosen = m_options->loras(m_engineId);
    auto isChosen = [&chosen](const LoraChoice &c) {
        for (const ChosenLora &l : chosen) {
            if ((!l.path.isEmpty() && l.path.compare(c.path, Qt::CaseInsensitive) == 0) || l.name.compare(c.name, Qt::CaseInsensitive) == 0)
                return true;
        }
        return false;
    };
    bool others = false;
    for (const LoraChoice &c : std::as_const(m_listing.loras)) {
        if (!c.mismatch.isEmpty() && !others) {
            others = true;
            menu->addSection(tr("Made for another model"));
        }
        QString text = c.name;
        const QString made = runtime::modelFamilyName(c.info.family, c.info.variant);
        if (!c.mismatch.isEmpty() && !made.isEmpty())
            text += QStringLiteral("  ·  ") + made;
        QAction *a = menu->addAction(text);
        a->setEnabled(c.mismatch.isEmpty() && !isChosen(c));
        QString tip = c.mismatch.isEmpty() ? (made.isEmpty() ? c.path : tr("Made for %1").arg(made)) : c.mismatch;
        if (!c.info.triggerWords.isEmpty())
            tip += QLatin1Char('\n') + tr("Words that call it up: %1").arg(c.info.triggerWords.join(QStringLiteral(", ")));
        a->setToolTip(tip);
        const LoraChoice choice = c;
        connect(a, &QAction::triggered, this, [this, choice] { add(choice); });
    }
}

void LoraPicker::add(const LoraChoice &choice)
{
    if (m_engineId.isEmpty() || !choice.mismatch.isEmpty())
        return;
    QList<ChosenLora> list = m_options->loras(m_engineId);
    ChosenLora l;
    l.name = choice.name;
    l.path = choice.path;
    l.file = choice.file;
    l.triggerWords = choice.info.triggerWords;
    list << l;
    m_options->setLoras(m_engineId, list);
}

} // namespace lumen::ui
