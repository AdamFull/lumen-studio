#include "ui/assistant/AssistantPanel.h"

#include "ai/ProviderRegistry.h"
#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/assistant/AssistantTools.h"
#include "ui/widgets/Controls.h"
#include "ui/widgets/ThumbnailCache.h"

#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace lumen::ui {

namespace {

// Readable one-line summary of tool arguments.
QString argumentSummary(const QJsonObject &args)
{
    QStringList parts;
    for (auto it = args.begin(); it != args.end(); ++it) {
        QString v = it.value().isString() ? it.value().toString()
                                          : QString::fromUtf8(QJsonDocument(QJsonArray{it.value()}).toJson(QJsonDocument::Compact)).mid(1).chopped(1);
        if (v.size() > 60)
            v = v.left(57) + QStringLiteral("…");
        parts << QStringLiteral("%1: %2").arg(it.key(), v);
    }
    return parts.join(QStringLiteral(" · "));
}

QString toolLabel(const QString &name)
{
    static const QHash<QString, const char *> labels = {
        {QStringLiteral("get_state"), QT_TRANSLATE_NOOP("lumen::ui::AssistantPanel", "Looking at the project")},
        {QStringLiteral("list_assets"), QT_TRANSLATE_NOOP("lumen::ui::AssistantPanel", "Browsing the library")},
        {QStringLiteral("generate_image"), QT_TRANSLATE_NOOP("lumen::ui::AssistantPanel", "Generating")},
        {QStringLiteral("edit_image"), QT_TRANSLATE_NOOP("lumen::ui::AssistantPanel", "Editing an image")},
        {QStringLiteral("inpaint_region"), QT_TRANSLATE_NOOP("lumen::ui::AssistantPanel", "Repainting a region")},
        {QStringLiteral("remove_background"), QT_TRANSLATE_NOOP("lumen::ui::AssistantPanel", "Removing the background")},
        {QStringLiteral("upscale"), QT_TRANSLATE_NOOP("lumen::ui::AssistantPanel", "Upscaling")},
        {QStringLiteral("open_in_editor"), QT_TRANSLATE_NOOP("lumen::ui::AssistantPanel", "Opening in the editor")},
        {QStringLiteral("analyze_references"), QT_TRANSLATE_NOOP("lumen::ui::AssistantPanel", "Analysing the references")},
        {QStringLiteral("plan_shots"), QT_TRANSLATE_NOOP("lumen::ui::AssistantPanel", "Planning shots")},
        {QStringLiteral("revise_pose"), QT_TRANSLATE_NOOP("lumen::ui::AssistantPanel", "Adjusting a pose")},
        {QStringLiteral("generate_shots"), QT_TRANSLATE_NOOP("lumen::ui::AssistantPanel", "Generating shots")},
    };
    const char *label = labels.value(name);
    return label ? AssistantPanel::tr(label) : name;
}

} // namespace

AssistantPanel::AssistantPanel(Studio *studio, QWidget *parent)
    : QFrame(parent)
    , m_studio(studio)
    , m_thumbs(new ThumbnailCache(this))
{
    setProperty("card", QStringLiteral("raised"));
    setAccessibleName(tr("Assistant"));
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(12, 12, 12, 12);
    v->setSpacing(8);

    auto *head = new QHBoxLayout;
    auto *title = new QLabel(tr("Assistant"), this);
    Theme::setRole(title, QStringLiteral("heading"));
    head->addWidget(title);
    m_model = mutedLabel(QString(), this);
    m_model->setWordWrap(false);
    head->addWidget(m_model, 1);
    auto *fresh = iconButton(QStringLiteral("plus"), tr("New chat"), this);
    auto *close = iconButton(QStringLiteral("close"), tr("Close"), this);
    head->addWidget(fresh);
    head->addWidget(close);
    v->addLayout(head);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget(m_scroll);
    m_messages = new QVBoxLayout(content);
    m_messages->setContentsMargins(0, 0, 6, 0);
    m_messages->setSpacing(8);
    m_suggestions = new QWidget(content);
    auto *sv = new QVBoxLayout(m_suggestions);
    sv->setContentsMargins(0, 8, 0, 0);
    sv->addWidget(mutedLabel(tr("Ask in your own words. The assistant plans, generates, edits and poses for you; every image "
                                "lands in the library."), m_suggestions));
    for (const QString &example : {tr("Plan three Live2D source images of my character from the reference images, then generate them"),
                                   tr("Generate a portrait of a fox knight in silver armour, 2:3"),
                                   tr("Remove the background of the newest image"),
                                   tr("Make the latest image look like a rainy night")}) {
        auto *b = new QPushButton(example, m_suggestions);
        b->setStyleSheet(QStringLiteral("QPushButton { text-align: left; padding: 8px 10px; }"));
        connect(b, &QPushButton::clicked, this, [this, example] { send(example); });
        sv->addWidget(b);
    }
    m_messages->addWidget(m_suggestions);
    m_messages->addStretch();
    m_scroll->setWidget(content);
    v->addWidget(m_scroll, 1);

    auto *composer = new QHBoxLayout;
    m_input = new AutoTextEdit(this, 2, 6);
    m_input->setPlaceholderText(tr("What should I do? (Enter sends, Shift+Enter adds a line)"));
    m_input->setSubmitOnEnter(true);
    m_input->setAccessibleName(tr("Message to the assistant"));
    m_send = new QPushButton(Icons::get(QStringLiteral("send")), tr("Send"), this);
    Theme::setRole(m_send, QStringLiteral("primary"));
    composer->addWidget(m_input, 1);
    composer->addWidget(m_send, 0, Qt::AlignBottom);
    v->addLayout(composer);

    connect(m_send, &QPushButton::clicked, this, [this] {
        if (isRunning())
            stop();
        else
            send(m_input->text());
    });
    connect(m_input, &AutoTextEdit::submitted, this, [this] {
        if (!isRunning())
            send(m_input->text());
    });
    connect(fresh, &QToolButton::clicked, this, &AssistantPanel::newChat);
    connect(close, &QToolButton::clicked, this, &AssistantPanel::closeRequested);
    connect(m_studio->providers(), &ai::ProviderRegistry::rolesChanged, this, &AssistantPanel::updateModelLabel);
    connect(m_studio->providers(), &ai::ProviderRegistry::providersChanged, this, &AssistantPanel::updateModelLabel);
    connect(m_studio, &Studio::projectOpened, this, [this] { m_thumbs->setProject(m_studio->project()); });
    m_thumbs->setProject(m_studio->project());
    updateModelLabel();
    newChat();
}

void AssistantPanel::updateModelLabel()
{
    QString model;
    ai::LlmProvider *provider = m_studio->providers()->llmFor(ai::LlmRole::Assistant, &model);
    m_model->setText(provider ? (model.isEmpty() ? provider->displayName() : QStringLiteral("%1 · %2").arg(provider->displayName(), model))
                              : tr("no language model"));
}

void AssistantPanel::newChat()
{
    stop();
    while (m_messages->count() > 2) {
        QLayoutItem *item = m_messages->takeAt(1);
        delete item->widget();
        delete item;
    }
    m_chips.clear();
    m_history = {ai::ChatMessage::system(assistantSystemPrompt())};
    m_lastReply.clear();
    showSuggestions(true);
}

void AssistantPanel::showSuggestions(bool show)
{
    m_suggestions->setVisible(show);
}

void AssistantPanel::scrollToEnd()
{
    QTimer::singleShot(20, this, [this] { m_scroll->verticalScrollBar()->setValue(m_scroll->verticalScrollBar()->maximum()); });
}

QLabel *AssistantPanel::addBubble(const QString &text, bool user)
{
    auto *bubble = new QLabel(text);
    bubble->setWordWrap(true);
    bubble->setTextFormat(user ? Qt::PlainText : Qt::MarkdownText);
    bubble->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    bubble->setOpenExternalLinks(true);
    const Palette &p = Theme::palette();
    bubble->setStyleSheet(QStringLiteral("QLabel { background: %1; border: 1px solid %2; border-radius: 10px; padding: 8px 10px; }")
                              .arg((user ? p.surface : p.panel).name(), p.border.name()));
    auto *row = new QWidget;
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(user ? 40 : 0, 0, user ? 0 : 40, 0);
    h->addWidget(bubble);
    m_messages->insertWidget(m_messages->count() - 1, row);
    scrollToEnd();
    return bubble;
}

QWidget *AssistantPanel::addToolChip(const QString &callId, const QString &name, const QJsonObject &arguments)
{
    auto *chip = new QFrame;
    chip->setProperty("card", QStringLiteral("true"));
    auto *v = new QVBoxLayout(chip);
    v->setContentsMargins(10, 6, 10, 6);
    v->setSpacing(4);
    auto *title = new QLabel(QStringLiteral("⚙ %1…").arg(toolLabel(name)), chip);
    title->setObjectName(QStringLiteral("title"));
    v->addWidget(title);
    const QString summary = argumentSummary(arguments);
    if (!summary.isEmpty()) {
        auto *args = mutedLabel(summary, chip);
        args->setToolTip(QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Indented)));
        v->addWidget(args);
    }
    m_messages->insertWidget(m_messages->count() - 1, chip);
    m_chips.insert(callId, chip);
    scrollToEnd();
    return chip;
}

void AssistantPanel::finishToolChip(const QString &callId, const ai::ToolResult &result)
{
    QWidget *chip = m_chips.value(callId);
    if (!chip)
        return;
    auto *title = chip->findChild<QLabel *>(QStringLiteral("title"));
    QString text = title ? title->text() : QString();
    if (text.endsWith(QStringLiteral("…")))
        text.chop(1);
    if (!result.error.isEmpty()) {
        text = QStringLiteral("⚠ %1 — %2").arg(text.mid(2), result.error.left(200));
    } else if (!result.assetIds.isEmpty()) {
        text = QStringLiteral("✓ %1 — %2").arg(text.mid(2), tr("%n image(s)", nullptr, int(result.assetIds.size())));
    } else {
        text = QStringLiteral("✓ %1").arg(text.mid(2));
    }
    if (title) {
        title->setText(text);
        title->setWordWrap(true);
    }
    if (!result.assetIds.isEmpty()) {
        auto *row = new QWidget(chip);
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 2, 0, 0);
        h->setSpacing(6);
        for (const QString &id : result.assetIds.mid(0, 8)) {
            auto *b = new QToolButton(row);
            b->setFixedSize(72, 72);
            b->setIconSize(QSize(68, 68));
            b->setToolTip(tr("Open in the editor"));
            b->setAccessibleName(tr("Result image %1").arg(id));
            const QPixmap pm = m_thumbs->get(id);
            if (!pm.isNull())
                b->setIcon(QIcon(pm));
            connect(m_thumbs, &ThumbnailCache::ready, b, [this, b, id](const QString &ready) {
                if (ready == id)
                    b->setIcon(QIcon(m_thumbs->get(id)));
            });
            connect(b, &QToolButton::clicked, this, [this, id] { m_studio->openAssetInEditor(id); });
            h->addWidget(b);
        }
        h->addStretch();
        chip->layout()->addWidget(row);
    }
    scrollToEnd();
}

void AssistantPanel::setRunning(bool running)
{
    m_send->setText(running ? tr("Stop") : tr("Send"));
    m_send->setIcon(Icons::get(running ? QStringLiteral("stop") : QStringLiteral("send")));
    Theme::setRole(m_send, running ? QStringLiteral("danger") : QStringLiteral("primary"));
}

void AssistantPanel::send(const QString &rawText)
{
    const QString text = rawText.trimmed();
    if (text.isEmpty() || isRunning())
        return;
    QString model;
    ai::LlmProvider *provider = m_studio->providers()->llmFor(ai::LlmRole::Assistant, &model);
    showSuggestions(false);
    addBubble(text, true);
    m_input->clear();
    if (!provider) {
        addBubble(tr("Add a language model in **Settings → Providers** (llama.cpp, Ollama, LM Studio, OpenAI, Anthropic, Gemini…) "
                     "and assign it to the *Assistant* role."),
                  false);
        return;
    }
    m_history.append(ai::ChatMessage::user(text));
    auto *run = new ai::AgentRun(provider, model, assistantTools(m_studio), m_history, 12, this);
    m_run = run;
    m_streamText.clear();
    m_streaming = nullptr;
    setRunning(true);
    connect(run, &ai::AgentRun::textDelta, this, [this](const QString &delta) {
        m_streamText += delta;
        if (!m_streaming)
            m_streaming = addBubble(QString(), false);
        m_streaming->setText(m_streamText);
        scrollToEnd();
    });
    connect(run, &ai::AgentRun::toolStarted, this, [this](const QString &id, const QString &name, const QJsonObject &args) {
        // Text before a tool call stays in its own bubble; the next text starts a new one.
        m_streaming = nullptr;
        m_streamText.clear();
        addToolChip(id, name, args);
    });
    connect(run, &ai::AgentRun::toolFinished, this,
            [this](const QString &id, const QString &, const ai::ToolResult &result) { finishToolChip(id, result); });
    connect(run, &ai::Operation::finished, this, [this, run] {
        setRunning(false);
        if (run->state() == ai::Operation::State::Succeeded) {
            m_history = run->messages();
            m_lastReply = run->reply();
            if (!m_streaming && !run->reply().isEmpty())
                addBubble(run->reply(), false);
        } else if (run->state() == ai::Operation::State::Failed) {
            // An interrupted tool exchange is not kept (providers reject unanswered tool calls).
            addBubble(tr("⚠ %1").arg(run->errorString()), false);
        } else {
            addBubble(tr("Stopped."), false);
        }
        m_streaming = nullptr;
        m_run = nullptr;
        emit runFinished();
    });
    run->start();
}

void AssistantPanel::stop()
{
    if (m_run)
        m_run->cancel();
}

} // namespace lumen::ui
