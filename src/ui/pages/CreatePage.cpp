#include "ui/pages/CreatePage.h"

#include "ai/Director.h"
#include "ai/GenerationService.h"
#include "ai/PromptStyle.h"
#include "core/Error.h"
#include "core/Settings.h"
#include "pose/Rig.h"
#include "core/JsonUtil.h"
#include "core/JobQueue.h"
#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/dialogs/CharacterLoraDialog.h"
#include "ui/dialogs/LoraDatasetDialog.h"
#include "ui/director/ShotCard.h"
#include "ui/widgets/Controls.h"
#include "ui/widgets/ProviderCombo.h"
#include "ui/widgets/ReferenceStrip.h"
#include "ui/widgets/ThumbnailCache.h"

#include <QFileInfo>
#include <QCheckBox>
#include <QComboBox>
#include <QInputDialog>
#include <QListWidget>
#include <QMenu>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopeGuard>
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
    m_count->setRange(0, ai::Director::kMaxDatasetShots);
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
    auto *libraryButton = new QToolButton(charCard);
    libraryButton->setIcon(Icons::get(QStringLiteral("person")));
    libraryButton->setText(tr("Library"));
    libraryButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    libraryButton->setPopupMode(QToolButton::InstantPopup);
    libraryButton->setToolTip(tr("Reuse characters across projects"));
    auto *libraryMenu = new QMenu(libraryButton);
    libraryMenu->addAction(Icons::get(QStringLiteral("download")), tr("Save character to library…"), this, [this] { saveCharacterToLibrary(); });
    libraryMenu->addAction(Icons::get(QStringLiteral("upload")), tr("Load character from library…"), this, &CreatePage::loadCharacterFromLibrary);
    libraryButton->setMenu(libraryMenu);
    m_lora = new QToolButton(charCard);
    m_lora->setIcon(Icons::get(QStringLiteral("star")));
    m_lora->setText(tr("LoRA"));
    m_lora->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_lora->setPopupMode(QToolButton::InstantPopup);
    m_lora->setToolTip(tr("A LoRA keeps the character looking the same in every image"));
    auto *loraMenu = new QMenu(m_lora);
    loraMenu->addAction(Icons::get(QStringLiteral("link")), tr("Attach a LoRA…"), this, &CreatePage::editCharacterLora);
    loraMenu->addAction(Icons::get(QStringLiteral("gallery")), tr("Training set…"), this, &CreatePage::openTrainingSet);
    m_lora->setMenu(loraMenu);
    charHead->addWidget(libraryButton);
    charHead->addWidget(m_lora);
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
    connect(m_provider, &ProviderCombo::providerChanged, this, &CreatePage::refreshCharacter); // LoRA support differs
    // A LoRA attached to the session's library character (after training) reaches the session.
    connect(studio->characters(), &doc::CharacterLibrary::changed, this, [this] {
        if (!m_session || m_session->libraryCharacter().isEmpty())
            return;
        if (const auto e = m_studio->characters()->entry(m_session->libraryCharacter()); e && !e->lora.isEmpty() && e->lora != m_session->characterLora())
            m_session->setCharacterLora(e->lora);
    });
    m_variants = new QSpinBox(board);
    m_variants->setRange(1, 8);
    m_variants->setValue(2);
    m_variants->setSuffix(tr(" per shot"));
    genBar->addWidget(m_variants);
    Settings &settings = Settings::instance();
    m_critic = new QCheckBox(tr("Critic"), board);
    m_critic->setToolTip(tr("A vision model reviews every result against the references and the shot, and retries shots that "
                            "score below the minimum with an improved prompt"));
    m_critic->setChecked(settings.value(QStringLiteral("director/critic"), false).toBool());
    m_criticThreshold = new QSpinBox(board);
    m_criticThreshold->setRange(1, 10);
    m_criticThreshold->setPrefix(tr("min "));
    m_criticThreshold->setSuffix(QStringLiteral("/10"));
    m_criticThreshold->setToolTip(tr("Lowest acceptable critic score"));
    m_criticThreshold->setValue(settings.value(QStringLiteral("director/criticThreshold"), 7).toInt());
    m_criticRetries = new QSpinBox(board);
    m_criticRetries->setRange(0, 3);
    m_criticRetries->setPrefix(tr("retries "));
    m_criticRetries->setToolTip(tr("How often a shot is generated again when no result reaches the minimum score"));
    m_criticRetries->setValue(settings.value(QStringLiteral("director/criticRetries"), 1).toInt());
    genBar->addWidget(m_critic);
    genBar->addWidget(m_criticThreshold);
    genBar->addWidget(m_criticRetries);
    auto syncCritic = [this] {
        m_criticThreshold->setEnabled(m_critic->isChecked());
        m_criticRetries->setEnabled(m_critic->isChecked());
        Settings::instance().setValue(QStringLiteral("director/critic"), m_critic->isChecked());
        Settings::instance().setValue(QStringLiteral("director/criticThreshold"), m_criticThreshold->value());
        Settings::instance().setValue(QStringLiteral("director/criticRetries"), m_criticRetries->value());
    };
    connect(m_critic, &QCheckBox::toggled, this, syncCritic);
    connect(m_criticThreshold, &QSpinBox::valueChanged, this, syncCritic);
    connect(m_criticRetries, &QSpinBox::valueChanged, this, syncCritic);
    m_criticThreshold->setEnabled(m_critic->isChecked());
    m_criticRetries->setEnabled(m_critic->isChecked());
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
    studio->setDirectorActions(this, this);
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

    // The character's LoRA, and whether the selected generator can use it.
    const ai::CharacterLora lora = ai::CharacterLora::fromJson(m_session->characterLora());
    m_lora->setText(lora.isValid() ? tr("LoRA ✓") : tr("LoRA"));
    if (lora.isValid()) {
        QString line = QStringLiteral("<br><b>%1</b> %2 · %3 · %4")
                           .arg(tr("LoRA:"), QFileInfo(lora.file).completeBaseName().toHtmlEscaped(),
                                lora.trigger.isEmpty() ? tr("no trigger word") : lora.trigger.toHtmlEscaped(),
                                tr("weight %1").arg(lora.weight, 0, 'g', 3));
        const LoraCheck check = checkCharacterLora(m_studio, m_provider->imageProvider(), lora);
        if (!check.problem.isEmpty())
            line += QStringLiteral("<br><span style='color:%1'>%2</span>").arg(Theme::palette().warning.name(), check.problem.toHtmlEscaped());
        m_characterSummary->setText(m_characterSummary->text() + line);
    }
}

void CreatePage::planTrainingShots()
{
    if (!m_session)
        newSession();
    if (!m_session->plan().isEmpty()
        && QMessageBox::question(this, tr("Training shots"), tr("Replace the current shot plan with the LoRA training shots?")) != QMessageBox::Yes)
        return;
    m_purpose->setCurrentIndex(std::max(0, m_purpose->findData(QStringLiteral("lora_dataset"))));
    m_session->setPlan({});
    if (m_session->character().isEmpty()) {
        m_studio->notify(tr("Create the character sheet first (Analyze references)."), QStringLiteral("warning"));
        return;
    }
    plan(QString(), {});
}

void CreatePage::openTrainingSet()
{
    if (!m_session)
        newSession();
    const QString id = m_session->libraryCharacter();
    if (id.isEmpty() || !m_studio->characters()->entry(id)) {
        if (QMessageBox::question(this, tr("Training set"),
                                  tr("The training set belongs to a character in the character library. Save this character to "
                                     "the library now?"))
            != QMessageBox::Yes)
            return;
        saveCharacterToLibrary([this] { openTrainingSet(); });
        return;
    }
    if (m_trainingSet) {
        m_trainingSet->raise();
        m_trainingSet->activateWindow();
        return;
    }
    auto *dlg = new LoraDatasetDialog(m_studio, id, m_session, [this] { planTrainingShots(); }, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    m_trainingSet = dlg;
    dlg->show();
}

void CreatePage::editCharacterLora()
{
    if (!m_session)
        newSession();
    CharacterLoraDialog dlg(m_studio, m_provider->imageProvider(), ai::CharacterLora::fromJson(m_session->characterLora()), this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    m_session->setCharacterLora(dlg.detached() ? QJsonObject() : dlg.lora().toJson());
    refreshCharacter();
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
        connect(card, &ShotCard::generateRequested, this, [this](const QString &id) { generateShot(id); });
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

void CreatePage::plan(const QString &instruction, std::function<void(QString error)> done)
{
    if (!m_session) {
        if (done)
            done(tr("There is no director session."));
        return;
    }
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
        in.request = QStringLiteral("Current plan:\n%1\n\nRevise the plan according to this new request (keep shot ids that remain): %2")
                         .arg(QString::fromUtf8(QJsonDocument(m_session->plan()).toJson(QJsonDocument::Compact)), instruction);
    }
    in.character = m_session->character();
    in.purposeId = m_purpose->currentData().toString();
    in.count = m_count->value();
    // A LoRA training set follows a fixed recipe; no language model needed.
    if (in.purposeId == QLatin1String("lora_dataset") && !in.character.isEmpty() && m_session->plan().isEmpty()) {
        const QJsonObject plan = ai::Director::datasetPlan(in.character, in.count);
        m_session->setPlan(plan);
        ChatEntry entry;
        entry.kind = ChatEntry::Kind::Assistant;
        entry.text = QStringLiteral("**%1**\n\n%2\n\n%3")
                         .arg(plan.value(QStringLiteral("title")).toString(), plan.value(QStringLiteral("summary")).toString(),
                              tr("Generate them (two variants per shot work well), then add the results to the training set: "
                                 "Character → LoRA → Training set…"));
        m_session->replaceLastStatus(entry);
        setBusy(false);
        if (done)
            done(QString());
        return;
    }
    in.style = ai::promptStyleFor(m_provider->imageProvider());
    QPointer<CreatePage> self(this);
    QPointer<DirectorSession> session = m_session;
    auto run = [self, session, in, done](QList<QImage> images) mutable {
        if (!self || !session)
            return;
        in.references = images;
        ai::StructuredChatOperation *op = self->m_studio->director()->planShots(in);
        QObject::connect(op, &ai::Operation::finished, self, [self, session, op, done] {
            if (!self || !session)
                return;
            self->setBusy(false);
            ChatEntry entry;
            if (op->state() == ai::Operation::State::Succeeded) {
                const QJsonObject plan = ai::Director::sanitizePlan(op->result());
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
                entry.kind = ChatEntry::Kind::Assistant;
                entry.text = lines.join(QStringLiteral("\n\n"));
            } else if (op->state() == ai::Operation::State::Cancelled) {
                entry.kind = ChatEntry::Kind::Error;
                entry.text = QObject::tr("Planning cancelled.");
            } else {
                entry.kind = ChatEntry::Kind::Error;
                entry.text = QObject::tr("Planning failed: %1").arg(op->errorString());
            }
            session->replaceLastStatus(entry);
            // Reported after the plan is stored: the caller reads it from the session.
            if (done)
                done(op->state() == ai::Operation::State::Succeeded ? QString()
                     : op->state() == ai::Operation::State::Cancelled ? tr("Planning was cancelled.")
                                                                       : op->errorString());
        });
    };
    // Without a character sheet, vision-capable directors look at the references directly.
    if (in.character.isEmpty())
        loadReferenceImages(run);
    else
        run({});
}

void CreatePage::generateShot(const QString &shotId, int attempt, const QJsonObject &shotOverride)
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
    const QJsonObject shotJson = shotOverride.isEmpty() ? shot->shot : shotOverride;
    const ShotState shotCopy = *shot;
    const int variants = m_variants->value();
    const ai::CharacterLora lora = ai::CharacterLora::fromJson(m_session->characterLora());
    const QString providerId = provider->instanceId();
    const QString sessionId = m_session->id();
    if (ShotCard *card = m_cards.value(shotId))
        card->setBusy(true, tr("Preparing…"));
    m_running[shotId]++;
    QPointer<CreatePage> self(this);
    QPointer<DirectorSession> session = m_session;
    loadReferenceImages([self, session, shotId, shotJson, shotCopy, caps, style, variants, providerId, sessionId, attempt, lora](QList<QImage> images) {
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
        ai::Director::applyCharacterLora(spec.request, lora, caps);
        if (shotJson.value(QStringLiteral("background")).toString().contains(QLatin1String("transparent"), Qt::CaseInsensitive))
            spec.request.transparentBackground = caps.transparentBackground;
        spec.title = QObject::tr("Shot: %1").arg(shotJson.value(QStringLiteral("title")).toString());
        spec.meta = QJsonObject{{QStringLiteral("director_session"), sessionId}, {QStringLiteral("shot"), shotId},
                                {QStringLiteral("shot_title"), shotJson.value(QStringLiteral("title"))}};
        if (attempt > 0)
            spec.meta.insert(QStringLiteral("critic_attempt"), attempt);
        Job *job = self->m_studio->generation()->submit(spec);
        ++self->m_generating;
        QObject::connect(job, &Job::progressChanged, self, [self, shotId](double f, const QString &status) {
            if (ShotCard *card = self->m_cards.value(shotId))
                card->setBusy(true, f >= 0 ? QStringLiteral("%1 — %2%").arg(status).arg(int(f * 100)) : status);
        });
        QObject::connect(job, &Job::finished, self, [self, session, shotId, attempt, shotJson](Job *j) {
            if (!self)
                return;
            --self->m_generating;
            self->m_running[shotId] = std::max(0, self->m_running.value(shotId) - 1);
            ShotCard *card = self->m_cards.value(shotId);
            if (card)
                card->setBusy(self->m_running.value(shotId) > 0, QString());
            if (j->state() == Job::State::Succeeded && session) {
                const ai::GenerationOutput out = ai::GenerationService::outputOf(j);
                QStringList added;
                if (ShotState *s = session->shot(shotId)) {
                    for (const doc::AssetInfo &a : out.assets) {
                        if (!a.id.isEmpty()) {
                            s->results.append(a.id);
                            added << a.id;
                        }
                    }
                    session->saveSoon();
                }
                if (card)
                    card->refresh();
                if (self->m_critic->isChecked() && !added.isEmpty() && session == self->m_session) {
                    // Counts as running, so the shot is not reported as finished before its review.
                    self->m_running[shotId]++;
                    self->m_pendingCritiques.append({shotId, added, attempt, shotJson});
                    if (ShotCard *c = self->m_cards.value(shotId))
                        c->setBusy(true, tr("Waiting for the other shots before the Critic reviews…"));
                }
            } else if (j->state() == Job::State::Failed) {
                if (card)
                    card->setBusy(false, j->errorString());
                self->m_studio->notify(j->errorString(), QStringLiteral("error"));
                self->shotFailed(shotId, j->errorString());
            }
            if (self->m_running.value(shotId) == 0)
                self->shotSettled(shotId);
            if (self->m_generating == 0)
                self->startPendingCritiques();
        });
    });
}

void CreatePage::startPendingRetries()
{
    const QList<PendingCritique> retries = m_pendingRetries;
    m_pendingRetries.clear();
    for (const PendingCritique &r : retries) {
        m_running[r.shotId] = std::max(0, m_running.value(r.shotId) - 1);
        generateShot(r.shotId, r.attempt, r.shotUsed);
    }
}

void CreatePage::startPendingCritiques()
{
    if (m_pendingCritiques.isEmpty() || m_generating > 0)
        return;
    const QList<PendingCritique> batch = m_pendingCritiques;
    m_pendingCritiques.clear();
    QPointer<CreatePage> self(this);
    m_studio->generation()->releaseGpu().then(this, [self, batch] {
        if (!self)
            return;
        for (const PendingCritique &p : batch) {
            self->m_running[p.shotId] = std::max(0, self->m_running.value(p.shotId) - 1);
            self->critique(p.shotId, p.assetIds, p.attempt, p.shotUsed);
        }
    });
}

void CreatePage::critique(const QString &shotId, const QStringList &assetIds, int attempt, const QJsonObject &shotUsed)
{
    doc::Project *project = m_studio->project();
    if (!m_session || !project)
        return;
    struct Round
    {
        int remaining;
        QJsonObject best;
        int bestScore = -1;
        QString error;
    };
    auto round = std::make_shared<Round>();
    round->remaining = int(assetIds.size());
    const int threshold = m_criticThreshold->value();
    const int retries = m_criticRetries->value();
    QPointer<CreatePage> self(this);
    QPointer<DirectorSession> session = m_session;
    QPointer<doc::Project> projectGuard = project;
    if (ShotCard *card = m_cards.value(shotId))
        card->setBusy(true, tr("Critic is reviewing…"));
    m_running[shotId]++;
    ++m_critiquing;
    auto finishRound = [self, session, shotId, attempt, shotUsed, round, threshold, retries] {
        if (!self || --round->remaining > 0)
            return;
        self->m_running[shotId] = std::max(0, self->m_running.value(shotId) - 1);
        --self->m_critiquing;
        // Retries start once every review of the batch is done (the GPU is shared).
        auto startRetries = qScopeGuard([self] {
            if (self && self->m_critiquing == 0)
                self->startPendingRetries();
        });
        // Settles the shot unless a retry is queued.
        auto settle = qScopeGuard([self, shotId] {
            if (self && self->m_running.value(shotId) == 0)
                self->shotSettled(shotId);
        });
        ShotCard *card = self->m_cards.value(shotId);
        if (round->bestScore < 0) {
            if (card)
                card->setBusy(false, tr("Critic failed: %1").arg(round->error));
            return;
        }
        if (round->bestScore >= threshold) {
            if (card)
                card->setBusy(false, tr("Critic: %1/10 ✓").arg(round->bestScore));
            return;
        }
        if (attempt < retries && session) {
            if (card)
                card->setBusy(true, tr("Critic: best %1/10 — retrying (%2/%3)…").arg(round->bestScore).arg(attempt + 1).arg(retries));
            self->m_running[shotId]++;
            self->m_pendingRetries.append({shotId, {}, attempt + 1, ai::Director::applyCritique(shotUsed, round->best)});
            return;
        }
        if (card)
            card->setBusy(false, tr("Critic: best %1/10 after %n attempt(s)", nullptr, attempt + 1).arg(round->bestScore));
    };
    loadReferenceImages([self, session, projectGuard, shotId, assetIds, shotUsed, round, finishRound](QList<QImage> refs) {
        if (!self || !session || !projectGuard)
            return;
        for (const QString &assetId : assetIds) {
            projectGuard->loadImage(assetId)
                .then(self, [self, session, projectGuard, shotId, assetId, shotUsed, refs, round, finishRound](QImage image) {
                    if (!self || !session) {
                        finishRound();
                        return;
                    }
                    ai::StructuredChatOperation *op = self->m_studio->director()->critiqueShot(image, shotUsed, session->character(), refs);
                    QObject::connect(op, &ai::Operation::finished, self, [self, session, projectGuard, shotId, assetId, op, round, finishRound] {
                        if (op->state() == ai::Operation::State::Succeeded && session) {
                            QJsonObject verdict = op->result();
                            verdict.insert(QStringLiteral("score"), ai::Director::critiqueScore(verdict));
                            if (ShotState *s = session->shot(shotId)) {
                                s->critiques.insert(assetId, verdict);
                                session->saveSoon();
                            }
                            if (projectGuard) {
                                if (auto info = projectGuard->asset(assetId)) {
                                    info->metadata.insert(QStringLiteral("critic"), verdict);
                                    projectGuard->updateAsset(*info);
                                }
                            }
                            if (verdict.value(QStringLiteral("score")).toInt() > round->bestScore) {
                                round->bestScore = verdict.value(QStringLiteral("score")).toInt();
                                round->best = verdict;
                            }
                            if (self) {
                                if (ShotCard *card = self->m_cards.value(shotId))
                                    card->refresh();
                            }
                        } else {
                            round->error = op->errorString();
                        }
                        finishRound();
                    });
                })
                .onFailed(self, [round, finishRound](const std::exception &e) {
                    round->error = QString::fromUtf8(e.what());
                    finishRound();
                });
        }
    });
}

// ---- DirectorActions ----------------------------------------------------------------------------------------

QJsonObject CreatePage::directorState() const
{
    QJsonObject state;
    if (!m_session)
        return state;
    state.insert(QStringLiteral("references"), json::toArray(m_session->references()));
    const QJsonObject c = m_session->character();
    if (!c.isEmpty())
        state.insert(QStringLiteral("character"), QJsonObject{{QStringLiteral("name"), c.value(QStringLiteral("name"))},
                                                              {QStringLiteral("summary"), c.value(QStringLiteral("summary"))}});
    state.insert(QStringLiteral("purpose"), m_session->purpose());
    QJsonArray purposes;
    for (const ai::PurposeProfile &p : ai::purposeProfiles())
        purposes.append(QJsonObject{{QStringLiteral("id"), p.id}, {QStringLiteral("name"), p.name}});
    state.insert(QStringLiteral("purposes"), purposes);
    state.insert(QStringLiteral("plan_title"), m_session->plan().value(QStringLiteral("title")));
    QJsonArray shots;
    for (const ShotState &s : m_session->shots()) {
        QJsonObject o{{QStringLiteral("id"), s.id()},
                      {QStringLiteral("title"), s.shot.value(QStringLiteral("title"))},
                      {QStringLiteral("description"), s.shot.value(QStringLiteral("description"))},
                      {QStringLiteral("prompt"), s.shot.value(QStringLiteral("prompt"))},
                      {QStringLiteral("approved"), s.approved},
                      {QStringLiteral("results"), json::toArray(s.results)},
                      {QStringLiteral("generating"), m_running.value(s.id()) > 0}};
        int best = -1;
        for (const QString &id : s.results)
            best = std::max(best, s.critiques.value(id).toObject().value(QStringLiteral("score")).toInt(-1));
        if (best >= 0)
            o.insert(QStringLiteral("best_critic_score"), best);
        shots.append(o);
    }
    state.insert(QStringLiteral("shots"), shots);
    if (ai::ImageProvider *p = m_provider->imageProvider())
        state.insert(QStringLiteral("image_provider"), p->displayName());
    return state;
}

void CreatePage::analyzeReferences(const QStringList &assetIds, const QString &notes,
                                   std::function<void(QJsonObject, QString)> done)
{
    if (!m_session)
        newSession();
    m_session->addReferences(assetIds);
    if (m_session->references().isEmpty()) {
        done({}, tr("The session has no reference images."));
        return;
    }
    if (!notes.isEmpty()) {
        ChatEntry entry;
        entry.kind = ChatEntry::Kind::User;
        entry.text = notes;
        m_session->addMessage(entry);
    }
    QPointer<DirectorSession> session = m_session;
    analyze([session, done] {
        if (!session) {
            done({}, tr("The session was closed."));
            return;
        }
        const QList<ChatEntry> &messages = session->messages();
        if (!messages.isEmpty() && messages.last().kind == ChatEntry::Kind::Error)
            done({}, messages.last().text);
        else
            done(session->character(), QString());
    });
}

void CreatePage::planShots(const QString &request, const QString &purpose, int count, std::function<void(QJsonObject, QString)> done)
{
    if (!m_session)
        newSession();
    if (m_busy) {
        done({}, tr("The director is busy; try again when it has finished."));
        return;
    }
    if (!purpose.isEmpty()) {
        const int index = m_purpose->findData(purpose);
        if (index >= 0)
            m_purpose->setCurrentIndex(index);
    }
    m_count->setValue(std::max(0, count));
    ChatEntry entry;
    entry.kind = ChatEntry::Kind::User;
    entry.text = request;
    m_session->setPurpose(m_purpose->currentData().toString());
    m_session->addMessage(entry);
    QPointer<DirectorSession> session = m_session;
    plan(request, [session, done](const QString &error) {
        if (!error.isEmpty() || !session)
            done({}, error.isEmpty() ? tr("The session was closed.") : error);
        else
            done(session->plan(), QString());
    });
}

void CreatePage::revisePose(const QString &shotId, const QString &instruction, std::function<void(QJsonObject, QString)> done)
{
    ShotState *shot = m_session ? m_session->shot(shotId) : nullptr;
    if (!shot) {
        done({}, tr("There is no shot \"%1\".").arg(shotId));
        return;
    }
    QPointer<DirectorSession> session = m_session;
    ai::StructuredChatOperation *op = m_studio->director()->revisePose(shot->shot, instruction);
    connect(op, &ai::Operation::finished, this, [this, session, op, shotId, done] {
        ShotState *s = session ? session->shot(shotId) : nullptr;
        if (op->state() != ai::Operation::State::Succeeded || !s) {
            done({}, op->state() == ai::Operation::State::Succeeded ? tr("The shot no longer exists.") : op->errorString());
            return;
        }
        const pose::RigParams rig = pose::RigParams::fromJson(op->result(), s->rig());
        s->shot.insert(QStringLiteral("pose"), rig.toJson());
        s->editedPose.reset();
        session->notifyShotChanged(shotId);
        if (ShotCard *card = m_cards.value(shotId))
            card->refresh();
        done(s->shot, QString());
    });
}

void CreatePage::generateShots(const QStringList &shotIds, std::function<void(QStringList, QString)> done)
{
    if (!m_session || m_session->shots().isEmpty()) {
        done({}, tr("There is no shot plan yet; plan the shots first."));
        return;
    }
    if (!m_provider->imageProvider()) {
        done({}, tr("No image provider is configured."));
        return;
    }
    QStringList ids;
    for (const ShotState &s : m_session->shots()) {
        if (shotIds.isEmpty() ? s.approved : shotIds.contains(s.id()))
            ids << s.id();
    }
    if (ids.isEmpty()) {
        done({}, tr("None of the requested shots exist."));
        return;
    }
    auto waiter = std::make_shared<ShotWaiter>();
    waiter->pending = ids;
    waiter->done = std::move(done);
    for (const QString &id : ids)
        waiter->baseline.insert(id, int(m_session->shot(id)->results.size()));
    m_waiters << waiter;
    for (const QString &id : ids)
        generateShot(id);
}

void CreatePage::shotFailed(const QString &shotId, const QString &error)
{
    for (const auto &w : std::as_const(m_waiters)) {
        if (w->pending.contains(shotId))
            w->errors << QStringLiteral("%1: %2").arg(shotId, error);
    }
}

void CreatePage::shotSettled(const QString &shotId)
{
    const auto waiters = m_waiters;
    for (const auto &w : waiters) {
        if (!w->pending.removeAll(shotId) || !w->pending.isEmpty())
            continue;
        QStringList assets;
        for (auto it = w->baseline.cbegin(); it != w->baseline.cend(); ++it) {
            if (const ShotState *s = m_session ? m_session->shot(it.key()) : nullptr)
                assets += s->results.mid(it.value());
        }
        m_waiters.removeAll(w);
        w->done(assets, assets.isEmpty() ? w->errors.join(QStringLiteral("; ")) : QString());
    }
}

void CreatePage::saveCharacterToLibrary(std::function<void()> then)
{
    if (!m_session || (m_session->character().isEmpty() && m_session->references().isEmpty())) {
        m_studio->notify(tr("Attach references or create a character sheet first."), QStringLiteral("warning"));
        return;
    }
    const QString suggested = m_session->character().value(QStringLiteral("name")).toString();
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Save character"), tr("Name in the character library:"), QLineEdit::Normal,
                                               suggested.isEmpty() ? tr("Character") : suggested, &ok)
                             .trimmed();
    if (!ok || name.isEmpty())
        return;
    // Saving under an existing name updates that character.
    QString existing;
    for (const doc::CharacterEntry &e : m_studio->characters()->entries()) {
        if (e.name.compare(name, Qt::CaseInsensitive) == 0)
            existing = e.id;
    }
    if (!existing.isEmpty()
        && QMessageBox::question(this, tr("Save character"), tr("Replace the saved character \"%1\"?").arg(name)) != QMessageBox::Yes)
        return;
    QPointer<CreatePage> self(this);
    const QJsonObject sheet = m_session->character();
    const QJsonObject lora = m_session->characterLora();
    QPointer<DirectorSession> session = m_session;
    loadReferenceImages([self, session, name, sheet, existing, lora, then](QList<QImage> images) {
        if (!self)
            return;
        self->m_studio->characters()
            ->save(name, sheet, images, QString(), existing, lora)
            .then(self, [self, session, then](doc::CharacterEntry e) {
                if (session)
                    session->setLibraryCharacter(e.id);
                self->m_studio->notify(tr("Saved \"%1\" with %n reference(s) to the character library.", nullptr, e.referenceCount).arg(e.name),
                                       QStringLiteral("success"));
                if (then)
                    then();
            })
            .onFailed(self, [self](const std::exception &e) { self->m_studio->notify(describeException(e), QStringLiteral("error")); });
    });
}

void CreatePage::loadCharacterFromLibrary()
{
    doc::CharacterLibrary *library = m_studio->characters();
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Character library"));
    dlg.resize(520, 460);
    auto *v = new QVBoxLayout(&dlg);
    v->addWidget(mutedLabel(tr("Characters saved from any project. Loading one adds its references to this project and sets "
                               "the character sheet of the session."), &dlg));
    auto *list = new QListWidget(&dlg);
    list->setIconSize(QSize(64, 64));
    list->setSpacing(2);
    v->addWidget(list, 1);
    auto fill = [list, library] {
        list->clear();
        for (const doc::CharacterEntry &e : library->entries()) {
            auto *item = new QListWidgetItem(QStringLiteral("%1\n%2").arg(e.name, tr("%n reference(s) · %1", nullptr, e.referenceCount)
                                                                                     .arg(QLocale().toString(e.modified, QLocale::ShortFormat))),
                                             list);
            if (!e.thumbnail.isNull())
                item->setIcon(QIcon(QPixmap::fromImage(e.thumbnail)));
            item->setData(Qt::UserRole, e.id);
        }
    };
    fill();
    auto *buttons = new QDialogButtonBox(&dlg);
    QPushButton *load = buttons->addButton(tr("Load"), QDialogButtonBox::AcceptRole);
    QPushButton *remove = buttons->addButton(tr("Delete"), QDialogButtonBox::DestructiveRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    Theme::setRole(load, QStringLiteral("primary"));
    v->addWidget(buttons);
    auto sync = [list, load, remove] {
        load->setEnabled(list->currentItem());
        remove->setEnabled(list->currentItem());
    };
    sync();
    connect(list, &QListWidget::currentItemChanged, &dlg, sync);
    connect(list, &QListWidget::itemDoubleClicked, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(remove, &QPushButton::clicked, &dlg, [&dlg, list, library, fill, sync] {
        QListWidgetItem *item = list->currentItem();
        if (!item)
            return;
        if (QMessageBox::question(&dlg, tr("Character library"), tr("Delete \"%1\" from the character library?")
                                                                      .arg(item->text().section(u'\n', 0, 0))) == QMessageBox::Yes) {
            library->remove(item->data(Qt::UserRole).toString());
            fill();
            sync();
        }
    });
    if (library->entries().isEmpty())
        list->addItem(tr("The library is empty. Save a character from any project first."));
    if (dlg.exec() != QDialog::Accepted || !list->currentItem() || list->currentItem()->data(Qt::UserRole).toString().isEmpty())
        return;
    const auto entry = library->entry(list->currentItem()->data(Qt::UserRole).toString());
    doc::Project *project = m_studio->project();
    if (!entry || !project)
        return;
    if (!m_session)
        newSession();
    QPointer<CreatePage> self(this);
    QPointer<DirectorSession> session = m_session;
    QPointer<doc::Project> projectGuard = project;
    const doc::CharacterEntry e = *entry;
    library->loadReferences(e.id).then(this, [self, session, projectGuard, e](QList<QImage> images) {
        if (!self || !session || !projectGuard)
            return;
        session->setCharacter(e.sheet);
        session->setCharacterLora(e.lora);
        session->setLibraryCharacter(e.id);
        if (images.isEmpty()) {
            self->m_studio->notify(tr("Loaded \"%1\".").arg(e.name), QStringLiteral("success"));
            return;
        }
        auto ids = std::make_shared<QStringList>();
        auto remaining = std::make_shared<int>(int(images.size()));
        for (int i = 0; i < images.size(); ++i) {
            projectGuard
                ->addAsset(images[i], doc::AssetKind::Reference, tr("%1 — reference %2").arg(e.name).arg(i + 1),
                           QJsonObject{{QStringLiteral("character_library"), e.id}})
                .then(self, [self, session, ids, remaining, e](doc::AssetInfo info) {
                    *ids << info.id;
                    if (--*remaining == 0 && session) {
                        session->addReferences(*ids);
                        self->m_studio->notify(tr("Loaded \"%1\" with %n reference(s).", nullptr, int(ids->size())).arg(e.name),
                                               QStringLiteral("success"));
                    }
                });
        }
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
