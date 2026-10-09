#include "ui/pages/CreatePage.h"

#include "ai/Director.h"
#include "ai/GenerationService.h"
#include "ai/PromptStyle.h"
#include "core/Error.h"
#include "core/JsonUtil.h"
#include "core/JobQueue.h"
#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/director/ShotCard.h"
#include "ui/widgets/Controls.h"
#include "ui/widgets/ProviderCombo.h"
#include "ui/widgets/ReferenceStrip.h"
#include "ui/widgets/ThumbnailCache.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace lumen::ui {

CreatePage::CreatePage(Studio *studio, QWidget *parent)
    : QWidget(parent)
    , m_studio(studio)
    , m_thumbs(new ThumbnailCache(this))
{
    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    root->addWidget(splitter);

    // ---------------- Chat column ----------------
    auto *chatCol = new QWidget(splitter);
    chatCol->setObjectName(QStringLiteral("Panel"));
    chatCol->setMinimumWidth(380);
    auto *cv = new QVBoxLayout(chatCol);
    cv->setContentsMargins(14, 14, 14, 14);
    cv->setSpacing(10);
    auto *head = new QHBoxLayout;
    auto *title = new QLabel(tr("Director"), chatCol);
    Theme::setRole(title, QStringLiteral("title"));
    head->addWidget(title);
    head->addStretch();
    m_sessions = new QComboBox(chatCol);
    m_sessions->setMinimumWidth(170);
    m_sessions->setToolTip(tr("Saved director sessions of this project"));
    head->addWidget(m_sessions);
    auto *newBtn = iconButton(QStringLiteral("plus"), tr("New session"), chatCol);
    head->addWidget(newBtn);
    cv->addLayout(head);

    m_chatScroll = new QScrollArea(chatCol);
    m_chatScroll->setWidgetResizable(true);
    m_chatScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *chatContent = new QWidget(m_chatScroll);
    m_chat = new QVBoxLayout(chatContent);
    m_chat->setContentsMargins(0, 0, 6, 0);
    m_chat->setSpacing(10);
    m_chat->addStretch();
    m_chatScroll->setWidget(chatContent);
    cv->addWidget(m_chatScroll, 1);

    auto *composer = card(chatCol);
    auto *comp = new QVBoxLayout(composer);
    comp->setContentsMargins(10, 10, 10, 10);
    comp->setSpacing(8);
    m_attachments = new ReferenceStrip(studio, composer);
    m_attachments->setCompact(true);
    comp->addWidget(m_attachments);
    m_input = new AutoTextEdit(composer, 3, 10);
    m_input->setSubmitOnEnter(true);
    m_input->setPlaceholderText(tr("Describe what you need, e.g. \"3 images of my character for a Live2D avatar: neutral front A-pose, "
                                   "eyes closed and mouth open variants\". Attach reference images above. Enter sends, Shift+Enter = new line."));
    m_input->setFrameShape(QFrame::NoFrame);
    comp->addWidget(m_input);
    auto *opts = new QHBoxLayout;
    m_purpose = new QComboBox(composer);
    for (const ai::PurposeProfile &p : ai::purposeProfiles())
        m_purpose->addItem(p.name, p.id);
    m_purpose->setToolTip(tr("What the images are for — shapes poses, framing and backgrounds"));
    m_count = new QSpinBox(composer);
    m_count->setRange(0, 12);
    m_count->setSpecialValueText(tr("Auto"));
    m_count->setPrefix(tr("Shots: "));
    m_count->setToolTip(tr("Number of shots (Auto = as described)"));
    m_send = new QPushButton(Icons::get(QStringLiteral("send")), tr("Send"), composer);
    Theme::setRole(m_send, QStringLiteral("primary"));
    opts->addWidget(m_purpose, 1);
    opts->addWidget(m_count);
    opts->addWidget(m_send);
    comp->addLayout(opts);
    cv->addWidget(composer);

    // ---------------- Board ----------------
    auto *board = new QWidget(splitter);
    auto *bv = new QVBoxLayout(board);
    bv->setContentsMargins(16, 14, 16, 14);
    bv->setSpacing(12);

    auto *charCard = card(board);
    auto *ch = new QVBoxLayout(charCard);
    ch->setContentsMargins(14, 12, 14, 12);
    auto *charHead = new QHBoxLayout;
    auto *charTitle = new QLabel(tr("Character"), charCard);
    Theme::setRole(charTitle, QStringLiteral("heading"));
    charHead->addWidget(charTitle);
    charHead->addStretch();
    auto *editSheet = new QPushButton(Icons::get(QStringLiteral("edit")), tr("Edit sheet"), charCard);
    m_analyze = new QPushButton(Icons::get(QStringLiteral("sparkles")), tr("Analyze references"), charCard);
    charHead->addWidget(editSheet);
    charHead->addWidget(m_analyze);
    ch->addLayout(charHead);
    auto *charBody = new QHBoxLayout;
    m_refRow = new QWidget(charCard);
    auto *refLayout = new QHBoxLayout(m_refRow);
    refLayout->setContentsMargins(0, 0, 0, 0);
    refLayout->setSpacing(6);
    m_refThumbs = nullptr;
    charBody->addWidget(m_refRow);
    m_characterSummary = mutedLabel(QString(), charCard);
    m_characterSummary->setTextFormat(Qt::RichText);
    charBody->addWidget(m_characterSummary, 1);
    ch->addLayout(charBody);
    bv->addWidget(charCard);

    auto *genBar = new QHBoxLayout;
    m_planTitle = new QLabel(tr("Shot plan"), board);
    Theme::setRole(m_planTitle, QStringLiteral("heading"));
    genBar->addWidget(m_planTitle, 1);
    genBar->addWidget(new QLabel(tr("Generator"), board));
    m_provider = new ProviderCombo(studio->providers(), ai::ProviderKind::Image, board);
    genBar->addWidget(m_provider);
    m_variants = new QSpinBox(board);
    m_variants->setRange(1, 8);
    m_variants->setValue(2);
    m_variants->setSuffix(tr(" per shot"));
    genBar->addWidget(m_variants);
    m_generateAll = new QPushButton(Icons::get(QStringLiteral("sparkles")), tr("Generate all"), board);
    Theme::setRole(m_generateAll, QStringLiteral("primary"));
    genBar->addWidget(m_generateAll);
    bv->addLayout(genBar);

    auto *shotsScroll = new QScrollArea(board);
    shotsScroll->setWidgetResizable(true);
    auto *shotsContent = new QWidget(shotsScroll);
    m_shots = new QVBoxLayout(shotsContent);
    m_shots->setContentsMargins(0, 0, 6, 0);
    m_shots->setSpacing(10);
    m_emptyHint = new QLabel(shotsContent);
    m_emptyHint->setWordWrap(true);
    m_emptyHint->setAlignment(Qt::AlignCenter);
    m_emptyHint->setTextFormat(Qt::RichText);
    m_emptyHint->setText(tr("<h2>Create a set of images with your character</h2>"
                            "<p style='color:%1'>1. Attach reference images of the character (drag & drop into the chat box).<br>"
                            "2. Describe what you need and pick the purpose (e.g. Live2D model, turnaround, stickers).<br>"
                            "3. The director analyses the references and plans every shot with an editable pose.<br>"
                            "4. Adjust poses and prompts, then generate — each result opens in the editor for inpainting.</p>")
                             .arg(Theme::palette().textMuted.name()));
    m_shots->addWidget(m_emptyHint);
    m_shots->addStretch();
    shotsScroll->setWidget(shotsContent);
    bv->addWidget(shotsScroll, 1);

    splitter->addWidget(chatCol);
    splitter->addWidget(board);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({460, 980});

    // ---------------- wiring ----------------
    connect(m_send, &QPushButton::clicked, this, &CreatePage::send);
    connect(m_input, &AutoTextEdit::submitted, this, &CreatePage::send);
    connect(newBtn, &QToolButton::clicked, this, &CreatePage::newSession);
    connect(m_sessions, &QComboBox::activated, this, [this](int index) {
        const QString id = m_sessions->itemData(index).toString();
        if (!id.isEmpty() && (!m_session || m_session->id() != id))
            openSession(id);
    });
    connect(m_purpose, &QComboBox::currentIndexChanged, this, [this] {
        if (m_session)
            m_session->setPurpose(m_purpose->currentData().toString());
    });
    connect(m_analyze, &QPushButton::clicked, this, [this] {
        if (!m_session || m_session->references().isEmpty()) {
            m_studio->notify(tr("Attach reference images first."), QStringLiteral("warning"));
            return;
        }
        analyze([] {});
    });
    connect(editSheet, &QPushButton::clicked, this, [this] {
        if (!m_session)
            return;
        QDialog dlg(this);
        dlg.setWindowTitle(tr("Character sheet"));
        dlg.resize(640, 640);
        auto *v = new QVBoxLayout(&dlg);
        v->addWidget(mutedLabel(tr("The sheet is sent to the director with every plan. Edit it to correct details."), &dlg));
        auto *edit = new QPlainTextEdit(&dlg);
        edit->setPlainText(QString::fromUtf8(QJsonDocument(m_session->character()).toJson(QJsonDocument::Indented)));
        v->addWidget(edit, 1);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
        v->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, &dlg, [&dlg, edit, this] {
            QJsonParseError err{};
            const QJsonDocument doc = QJsonDocument::fromJson(edit->toPlainText().toUtf8(), &err);
            if (err.error != QJsonParseError::NoError || !doc.isObject()) {
                QMessageBox::warning(&dlg, tr("Character sheet"), tr("Invalid JSON: %1").arg(err.errorString()));
                return;
            }
            m_session->setCharacter(doc.object());
            dlg.accept();
        });
        dlg.exec();
    });
    connect(m_generateAll, &QPushButton::clicked, this, &CreatePage::generateAll);
    connect(studio, &Studio::projectOpened, this, [this] {
        m_thumbs->setProject(m_studio->project());
        loadSessions();
    });
    connect(m_thumbs, &ThumbnailCache::ready, this, [this](const QString &id) {
        // Late-arriving thumbnails for chat attachments.
        for (QToolButton *b : m_chatScroll->widget()->findChildren<QToolButton *>()) {
            if (b->property("asset").toString() == id)
                b->setIcon(QIcon(m_thumbs->get(id)));
        }
    });
    m_thumbs->setProject(studio->project());
    if (studio->isProjectOpen())
        loadSessions();
    else
        newSession();
}

// ---- sessions --------------------------------------------------------------------------------------------

void CreatePage::loadSessions(const QString &select)
{
    doc::Project *project = m_studio->project();
    if (!project || !project->isOpen()) {
        newSession();
        return;
    }
    QPointer<CreatePage> self(this);
    project->records(QStringLiteral("director")).then(this, [self, select](QList<doc::Record> records) {
        if (!self)
            return;
        self->m_sessions->clear();
        for (const doc::Record &r : records)
            self->m_sessions->addItem(r.name, r.id);
        if (records.isEmpty()) {
            self->newSession();
            return;
        }
        const QString id = select.isEmpty() ? records.first().id : select;
        self->m_sessions->setCurrentIndex(std::max(0, self->m_sessions->findData(id)));
        auto *session = new DirectorSession(self);
        for (const doc::Record &r : records) {
            if (r.id == id)
                session->fromJson(r.id, r.json);
        }
        self->bindSession(session);
    });
}

void CreatePage::openSession(const QString &id)
{
    loadSessions(id);
}

void CreatePage::newSession()
{
    auto *session = new DirectorSession(this);
    session->setPurpose(m_purpose->currentData().toString());
    bindSession(session);
    m_sessions->insertItem(0, tr("New session"), QString());
    m_sessions->setCurrentIndex(0);
}

void CreatePage::bindSession(DirectorSession *session)
{
    if (m_session) {
        m_session->disconnect(this);
        m_session->deleteLater();
    }
    m_session = session;
    session->bind(m_studio->project());
    connect(session, &DirectorSession::messagesChanged, this, &CreatePage::rebuildChat);
    connect(session, &DirectorSession::planChanged, this, &CreatePage::rebuildShots);
    connect(session, &DirectorSession::characterChanged, this, &CreatePage::refreshCharacter);
    connect(session, &DirectorSession::referencesChanged, this, &CreatePage::refreshCharacter);
    const int purpose = m_purpose->findData(session->purpose());
    if (purpose >= 0) {
        QSignalBlocker b(m_purpose);
        m_purpose->setCurrentIndex(purpose);
    }
    rebuildChat();
    rebuildShots();
    refreshCharacter();
}

// ---- chat ---------------------------------------------------------------------------------------------------

QWidget *CreatePage::bubble(const ChatEntry &e)
{
    const Palette &pal = Theme::palette();
    auto *row = new QWidget;
    auto *rh = new QHBoxLayout(row);
    rh->setContentsMargins(0, 0, 0, 0);
    auto *b = new QFrame(row);
    auto *v = new QVBoxLayout(b);
    v->setContentsMargins(12, 9, 12, 9);
    v->setSpacing(6);
    QString style;
    switch (e.kind) {
    case ChatEntry::Kind::User:
        style = QStringLiteral("background: %1; border-radius: 12px;").arg(Theme::mix(pal.accent, pal.panel, 0.72).name());
        break;
    case ChatEntry::Kind::Assistant:
        style = QStringLiteral("background: %1; border: 1px solid %2; border-radius: 12px;").arg(pal.surface.name(), pal.border.name());
        break;
    case ChatEntry::Kind::Status:
        style = QStringLiteral("background: transparent; border: 1px dashed %1; border-radius: 12px;").arg(pal.borderStrong.name());
        break;
    case ChatEntry::Kind::Error:
        style = QStringLiteral("background: %1; border: 1px solid %2; border-radius: 12px;")
                    .arg(QColor(pal.danger.red(), pal.danger.green(), pal.danger.blue(), 30).name(QColor::HexArgb), pal.danger.name());
        break;
    }
    b->setObjectName(QStringLiteral("bubble"));
    b->setStyleSheet(QStringLiteral("QFrame#bubble { %1 }").arg(style));
    if (!e.assetIds.isEmpty()) {
        auto *thumbs = new QHBoxLayout;
        thumbs->setSpacing(4);
        for (const QString &id : e.assetIds.mid(0, 8)) {
            auto *t = new QToolButton(b);
            t->setFixedSize(56, 56);
            t->setIconSize(QSize(52, 52));
            const QPixmap pm = m_thumbs->get(id);
            if (!pm.isNull())
                t->setIcon(QIcon(pm));
            t->setProperty("asset", id);
            connect(t, &QToolButton::clicked, this, [this, id] { m_studio->openAssetInEditor(id); });
            thumbs->addWidget(t);
        }
        thumbs->addStretch();
        v->addLayout(thumbs);
    }
    if (!e.text.isEmpty()) {
        auto *label = new QLabel(b);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        label->setTextFormat(e.kind == ChatEntry::Kind::Assistant ? Qt::MarkdownText : Qt::PlainText);
        label->setText(e.kind == ChatEntry::Kind::Status ? QStringLiteral("⏳ ") + e.text : e.text);
        if (e.kind == ChatEntry::Kind::Status)
            label->setStyleSheet(QStringLiteral("color: %1; font-style: italic;").arg(pal.textMuted.name()));
        v->addWidget(label);
    }
    if (e.kind == ChatEntry::Kind::User) {
        rh->addStretch();
        rh->addWidget(b, 0);
        b->setMaximumWidth(420);
    } else {
        rh->addWidget(b, 1);
    }
    return row;
}

void CreatePage::rebuildChat()
{
    while (m_chat->count() > 1) {
        QLayoutItem *item = m_chat->takeAt(0);
        delete item->widget();
        delete item;
    }
    if (!m_session)
        return;
    if (m_session->messages().isEmpty()) {
        auto *hint = mutedLabel(tr("Start by attaching reference images of your character and describing the images you need."));
        hint->setAlignment(Qt::AlignCenter);
        m_chat->insertWidget(0, hint);
    }
    int i = 0;
    for (const ChatEntry &e : m_session->messages())
        m_chat->insertWidget(i++, bubble(e));
    QTimer::singleShot(30, this, [this] { m_chatScroll->verticalScrollBar()->setValue(m_chatScroll->verticalScrollBar()->maximum()); });
}

void CreatePage::refreshCharacter()
{
    auto *layout = static_cast<QHBoxLayout *>(m_refRow->layout());
    while (QLayoutItem *item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    if (!m_session)
        return;
    for (const QString &id : m_session->references()) {
        auto *t = new QToolButton(m_refRow);
        t->setFixedSize(64, 64);
        t->setIconSize(QSize(60, 60));
        const QPixmap pm = m_thumbs->get(id);
        if (!pm.isNull())
            t->setIcon(QIcon(pm));
        t->setToolTip(tr("Reference — click to remove"));
        connect(t, &QToolButton::clicked, this, [this, id] {
            if (QMessageBox::question(this, tr("Reference"), tr("Remove this reference from the session?")) == QMessageBox::Yes)
                m_session->removeReference(id);
        });
        layout->addWidget(t);
    }
    connect(m_thumbs, &ThumbnailCache::ready, this, &CreatePage::refreshCharacter, Qt::UniqueConnection);
    const QJsonObject c = m_session->character();
    if (c.isEmpty()) {
        m_characterSummary->setText(m_session->references().isEmpty()
                                        ? tr("No references yet. Attach images in the chat.")
                                        : tr("%n reference(s). The character sheet is created automatically when you send a message, "
                                             "or press Analyze.", nullptr, int(m_session->references().size())));
    } else {
        QStringList palette = json::toStringList(c.value(QStringLiteral("color_palette")).toArray()).mid(0, 6);
        m_characterSummary->setText(QStringLiteral("<b>%1</b> — %2<br><span>%3</span><br><i>%4</i>")
                                        .arg(c.value(QStringLiteral("name")).toString().toHtmlEscaped(),
                                             c.value(QStringLiteral("summary")).toString().toHtmlEscaped(),
                                             c.value(QStringLiteral("description")).toString().left(400).toHtmlEscaped(),
                                             palette.join(QStringLiteral(", ")).toHtmlEscaped()));
    }
    m_analyze->setText(c.isEmpty() ? tr("Analyze references") : tr("Re-analyze"));
}

void CreatePage::rebuildShots()
{
    for (auto it = m_cards.begin(); it != m_cards.end(); ++it) {
        if (it.value())
            it.value()->deleteLater();
    }
    m_cards.clear();
    if (!m_session)
        return;
    const QJsonObject plan = m_session->plan();
    m_emptyHint->setVisible(m_session->shots().isEmpty());
    m_planTitle->setText(plan.value(QStringLiteral("title")).toString().isEmpty() ? tr("Shot plan")
                                                                                   : plan.value(QStringLiteral("title")).toString());
    int i = 0;
    for (const ShotState &s : m_session->shots()) {
        auto *card = new ShotCard(m_studio, m_session, s.id(), m_thumbs);
        connect(card, &ShotCard::generateRequested, this, &CreatePage::generateShot);
        m_shots->insertWidget(1 + i++, card);
        m_cards.insert(s.id(), card);
        if (m_running.value(s.id()) > 0)
            card->setBusy(true, tr("Generating…"));
    }
    m_generateAll->setEnabled(!m_session->shots().isEmpty());
}

void CreatePage::setBusy(bool busy)
{
    m_busy = busy;
    m_send->setEnabled(!busy);
    m_analyze->setEnabled(!busy);
    m_send->setText(busy ? tr("Working…") : tr("Send"));
}

// ---- pipeline -------------------------------------------------------------------------------------------------

void CreatePage::loadReferenceImages(std::function<void(QList<QImage>)> then)
{
    doc::Project *project = m_studio->project();
    const QStringList ids = m_session ? m_session->references() : QStringList();
    if (!project || ids.isEmpty()) {
        then({});
        return;
    }
    struct State
    {
        QList<QImage> images;
        int remaining;
    };
    auto state = std::make_shared<State>();
    state->images.resize(ids.size());
    state->remaining = int(ids.size());
    for (int i = 0; i < ids.size(); ++i) {
        project->loadImage(ids[i])
            .then(this, [state, i, then](QImage img) {
                state->images[i] = img;
                if (--state->remaining == 0) {
                    state->images.removeIf([](const QImage &im) { return im.isNull(); });
                    then(state->images);
                }
            })
            .onFailed(this, [state, then](const std::exception &) {
                if (--state->remaining == 0) {
                    state->images.removeIf([](const QImage &im) { return im.isNull(); });
                    then(state->images);
                }
            });
    }
}

void CreatePage::send()
{
    if (!m_session || m_busy)
        return;
    const QString text = m_input->text().trimmed();
    QStringList attached;
    for (const ai::ReferenceImage &r : m_attachments->references()) {
        if (!r.assetId.isEmpty())
            attached << r.assetId;
    }
    if (text.isEmpty() && attached.isEmpty())
        return;
    if (!m_studio->providers()->llmFor(ai::LlmRole::Director)) {
        m_studio->notify(tr("Add a language model (llama.cpp, Ollama, LM Studio, OpenAI, Anthropic, Gemini…) in Settings → Providers."),
                         QStringLiteral("warning"));
        return;
    }
    ChatEntry entry;
    entry.kind = ChatEntry::Kind::User;
    entry.text = text;
    entry.assetIds = attached;
    m_session->addReferences(attached);
    m_session->setPurpose(m_purpose->currentData().toString());
    m_session->addMessage(entry);
    m_input->clear();
    m_attachments->clear();
    setBusy(true);
    const bool needsAnalysis = m_session->character().isEmpty() && !m_session->references().isEmpty() && !attached.isEmpty();
    if (needsAnalysis || (m_session->character().isEmpty() && !m_session->references().isEmpty()))
        analyze([this, text] { plan(text); });
    else
        plan(text);
}

void CreatePage::analyze(std::function<void()> then)
{
    if (!m_session)
        return;
    ChatEntry status;
    status.kind = ChatEntry::Kind::Status;
    status.text = tr("Studying %n reference image(s)…", nullptr, int(m_session->references().size()));
    m_session->addMessage(status);
    setBusy(true);
    QStringList notes;
    for (const ChatEntry &e : m_session->messages()) {
        if (e.kind == ChatEntry::Kind::User && !e.text.isEmpty())
            notes << e.text;
    }
    QPointer<CreatePage> self(this);
    QPointer<DirectorSession> session = m_session;
    loadReferenceImages([self, session, notes, then](QList<QImage> images) {
        if (!self || !session)
            return;
        ai::StructuredChatOperation *op = self->m_studio->director()->analyzeCharacter(images, notes.join(QStringLiteral("\n")));
        QObject::connect(op, &ai::Operation::finished, self, [self, session, op, then] {
            if (!self || !session)
                return;
            ChatEntry done;
            if (op->state() == ai::Operation::State::Succeeded) {
                session->setCharacter(op->result());
                done.kind = ChatEntry::Kind::Assistant;
                done.text = QObject::tr("**Character sheet:** %1 — %2").arg(op->result().value(QStringLiteral("name")).toString(),
                                                                           op->result().value(QStringLiteral("summary")).toString());
            } else {
                done.kind = ChatEntry::Kind::Error;
                done.text = QObject::tr("Reference analysis failed: %1\nPlanning continues without a character sheet.").arg(op->errorString());
            }
            session->replaceLastStatus(done);
            self->setBusy(false);
            then(); // may start the next step (planning), which sets the busy state again
        });
    });
}

void CreatePage::plan(const QString &instruction)
{
    if (!m_session)
        return;
    ChatEntry status;
    status.kind = ChatEntry::Kind::Status;
    status.text = m_session->plan().isEmpty() ? tr("Planning shots and poses…") : tr("Revising the plan…");
    m_session->addMessage(status);
    setBusy(true);

    ai::PlanInput in;
    QStringList requests;
    for (const ChatEntry &e : m_session->messages()) {
        if (e.kind == ChatEntry::Kind::User && !e.text.isEmpty())
            requests << e.text;
    }
    if (m_session->plan().isEmpty()) {
        in.request = requests.join(QStringLiteral("\n"));
    } else {
        in.request = tr("Current plan:\n%1\n\nRevise the plan according to this new request (keep shot ids that remain): %2")
                         .arg(QString::fromUtf8(QJsonDocument(m_session->plan()).toJson(QJsonDocument::Compact)), instruction);
    }
    in.character = m_session->character();
    in.purposeId = m_purpose->currentData().toString();
    in.count = m_count->value();
    in.style = ai::promptStyleFor(m_provider->imageProvider());
    QPointer<CreatePage> self(this);
    QPointer<DirectorSession> session = m_session;
    auto run = [self, session, in](QList<QImage> images) mutable {
        if (!self || !session)
            return;
        in.references = images;
        ai::StructuredChatOperation *op = self->m_studio->director()->planShots(in);
        QObject::connect(op, &ai::Operation::finished, self, [self, session, op] {
            if (!self || !session)
                return;
            self->setBusy(false);
            ChatEntry done;
            if (op->state() == ai::Operation::State::Succeeded) {
                const QJsonObject plan = op->result();
                session->setPlan(plan);
                QStringList lines;
                lines << QStringLiteral("**%1**").arg(plan.value(QStringLiteral("title")).toString());
                if (!plan.value(QStringLiteral("summary")).toString().isEmpty())
                    lines << plan.value(QStringLiteral("summary")).toString();
                int n = 0;
                for (const QJsonValue &v : plan.value(QStringLiteral("shots")).toArray()) {
                    const QJsonObject s = v.toObject();
                    lines << QStringLiteral("%1. **%2** — %3").arg(++n).arg(s.value(QStringLiteral("title")).toString(),
                                                                  s.value(QStringLiteral("description")).toString());
                }
                lines << QObject::tr("Review and adjust the poses on the right, then press **Generate all** (or generate shots one by one).");
                done.kind = ChatEntry::Kind::Assistant;
                done.text = lines.join(QStringLiteral("\n\n"));
            } else if (op->state() == ai::Operation::State::Cancelled) {
                done.kind = ChatEntry::Kind::Error;
                done.text = QObject::tr("Planning cancelled.");
            } else {
                done.kind = ChatEntry::Kind::Error;
                done.text = QObject::tr("Planning failed: %1").arg(op->errorString());
            }
            session->replaceLastStatus(done);
        });
    };
    // Without a character sheet, vision-capable directors look at the references directly.
    if (in.character.isEmpty())
        loadReferenceImages(run);
    else
        run({});
}

void CreatePage::generateShot(const QString &shotId)
{
    if (!m_session)
        return;
    ShotState *shot = m_session->shot(shotId);
    ai::ImageProvider *provider = m_provider->imageProvider();
    if (!shot || !provider) {
        if (!provider)
            m_studio->notify(tr("Add an image provider in Settings → Providers first."), QStringLiteral("warning"));
        return;
    }
    const ai::ImageCapabilities caps = provider->capabilities();
    const ai::PromptStyle style = ai::promptStyleFor(provider);
    const QJsonObject shotJson = shot->shot;
    const ShotState shotCopy = *shot;
    const int variants = m_variants->value();
    const QString providerId = provider->instanceId();
    const QString sessionId = m_session->id();
    if (ShotCard *card = m_cards.value(shotId))
        card->setBusy(true, tr("Preparing…"));
    m_running[shotId]++;
    QPointer<CreatePage> self(this);
    QPointer<DirectorSession> session = m_session;
    loadReferenceImages([self, session, shotId, shotJson, shotCopy, caps, style, variants, providerId, sessionId](QList<QImage> images) {
        if (!self || !session)
            return;
        QList<ai::ReferenceImage> refs;
        for (const QImage &img : images) {
            ai::ReferenceImage r;
            r.image = img;
            r.role = ai::ReferenceRole::Character;
            r.weight = 0.8;
            refs << r;
        }
        const QSize size = ai::Director::sizeForAspect(shotCopy.aspect(), caps.nativePixels, std::max(64, caps.sizeMultiple));
        const pose::Pose pose = ShotCard::shotPose(shotCopy, size);
        ai::GenerationSpec spec;
        spec.providerId = providerId;
        spec.request = ai::Director::buildShotRequest(shotJson, refs, pose, style, caps);
        spec.request.batchCount = variants;
        if (shotJson.value(QStringLiteral("background")).toString().contains(QLatin1String("transparent"), Qt::CaseInsensitive))
            spec.request.transparentBackground = caps.transparentBackground;
        spec.title = QObject::tr("Shot: %1").arg(shotJson.value(QStringLiteral("title")).toString());
        spec.meta = QJsonObject{{QStringLiteral("director_session"), sessionId}, {QStringLiteral("shot"), shotId},
                                {QStringLiteral("shot_title"), shotJson.value(QStringLiteral("title"))}};
        Job *job = self->m_studio->generation()->submit(spec);
        QObject::connect(job, &Job::progressChanged, self, [self, shotId](double f, const QString &status) {
            if (ShotCard *card = self->m_cards.value(shotId))
                card->setBusy(true, f >= 0 ? QStringLiteral("%1 — %2%").arg(status).arg(int(f * 100)) : status);
        });
        QObject::connect(job, &Job::finished, self, [self, session, shotId](Job *j) {
            if (!self)
                return;
            self->m_running[shotId] = std::max(0, self->m_running.value(shotId) - 1);
            ShotCard *card = self->m_cards.value(shotId);
            if (card)
                card->setBusy(self->m_running.value(shotId) > 0, QString());
            if (j->state() == Job::State::Succeeded && session) {
                const ai::GenerationOutput out = ai::GenerationService::outputOf(j);
                if (ShotState *s = session->shot(shotId)) {
                    for (const doc::AssetInfo &a : out.assets) {
                        if (!a.id.isEmpty())
                            s->results.append(a.id);
                    }
                    session->saveSoon();
                }
                if (card)
                    card->refresh();
            } else if (j->state() == Job::State::Failed) {
                if (card)
                    card->setBusy(false, j->errorString());
                self->m_studio->notify(j->errorString(), QStringLiteral("error"));
            }
        });
    });
}

void CreatePage::generateAll()
{
    if (!m_session)
        return;
    int n = 0;
    for (const ShotState &s : m_session->shots()) {
        if (s.approved) {
            generateShot(s.id());
            ++n;
        }
    }
    if (n == 0)
        m_studio->notify(tr("No shots are selected for generation."), QStringLiteral("warning"));
    else
        m_studio->notify(tr("Queued %n shot(s).", nullptr, n));
}

} // namespace lumen::ui
