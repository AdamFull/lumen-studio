#include "ui/effects/EffectEditorWindow.h"

#include "core/Async.h"
#include "core/Error.h"
#include "core/ImageOps.h"
#include "doc/Document.h"
#include "filters/EffectNodes.h"
#include "filters/Evaluator.h"
#include "gpu/Effects.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/effects/CodeEditor.h"
#include "ui/effects/UserEffects.h"
#include "ui/filters/FilterCatalog.h"
#include "ui/filters/ParamWidgets.h"

#include <QCloseEvent>
#include <QFileDialog>
#include <QToolButton>
#include <QShortcut>
#include <QTreeWidget>
#include <QTabWidget>
#include <QStatusBar>
#include <QMenuBar>
#include <QInputDialog>
#include <QCheckBox>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSplitter>
#include <QTextBlock>
#include <QVBoxLayout>

namespace lumen::ui {

namespace {

QPointer<EffectEditorWindow> g_window;
int g_previews = 0;

} // namespace

QString EffectEditorWindow::templateSource()
{
    return QStringLiteral(
        "// An effect of your own. Lumen runs it on the graphics card over the picture, in linear\n"
        "// light (RGBA, half floats). Its sliders come from [Label] and [Range]; \"size\" is filled in.\n"
        "import effect;      // [Effect], [Label], [Range], [Default], [Unit], [Section]…\n"
        "import colorspace;  // toSrgb, toLinear, luma, toOklab, fromOklab\n"
        "\n"
        "struct Params\n"
        "{\n"
        "    [Label(\"Warmth\")] [Range(-100, 100)] float warmth;\n"
        "    [Label(\"Fade\")] [Range(0, 100)] float fade;\n"
        "    uint2 size;\n"
        "};\n"
        "[[vk::push_constant]] ConstantBuffer<Params> p;\n"
        "\n"
        "[[vk::binding(0)]] [format(\"rgba16f\")] RWTexture2D<float4> source; // the picture so far\n"
        "[[vk::binding(1)]] [format(\"rgba16f\")] RWTexture2D<float4> result; // what this effect makes\n"
        "\n"
        "[Effect(\"Warm fade\", \"Mine\")]\n"
        "[shader(\"compute\")]\n"
        "[numthreads(16, 16, 1)]\n"
        "void main(uint3 id: SV_DispatchThreadID)\n"
        "{\n"
        "    if (any(id.xy >= p.size))\n"
        "        return;\n"
        "    const float4 c = source[id.xy];\n"
        "    float3 rgb = c.rgb * float3(1.0 + 0.2 * p.warmth / 100.0, 1.0, 1.0 - 0.2 * p.warmth / 100.0);\n"
        "    // Fade: the darks lifted, on display values.\n"
        "    float3 shown = toSrgb(max(rgb, 0.0));\n"
        "    shown = lerp(shown, 0.15 + 0.85 * shown, p.fade / 100.0);\n"
        "    result[id.xy] = float4(toLinear(shown), c.a);\n"
        "}\n");
}

EffectEditorWindow *EffectEditorWindow::show(Studio *studio, QWidget *parent)
{
    if (!g_window)
        g_window = new EffectEditorWindow(studio, parent);
    g_window->QMainWindow::show();
    g_window->raise();
    g_window->activateWindow();
    return g_window;
}

EffectEditorWindow::EffectEditorWindow(Studio *studio, QWidget *parent)
    : QMainWindow(parent, Qt::Window)
    , m_studio(studio)
    , m_effects(studio->userEffects())
    , m_previewId(QStringLiteral("preview:%1").arg(++g_previews))
{
    setObjectName(QStringLiteral("effectEditor"));
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("Effect editor"));
    resize(1180, 740);

    auto *central = new QWidget(this);
    auto *outer = new QVBoxLayout(central);

    // What this is, and what it needs: the compiler, or a look at a file from elsewhere.
    auto *bannerRow = new QHBoxLayout;
    m_banner = new QLabel(central);
    m_banner->setObjectName(QStringLiteral("effectBanner"));
    m_banner->setWordWrap(true);
    m_download = new QPushButton(tr("Download"), central);
    m_download->setObjectName(QStringLiteral("effectDownloadCompiler"));
    bannerRow->addWidget(m_banner, 1);
    bannerRow->addWidget(m_download);
    outer->addLayout(bannerRow);

    auto *split = new QSplitter(Qt::Horizontal, central);
    outer->addWidget(split, 1);

    // Your effects.
    auto *left = new QWidget(split);
    auto *lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);
    auto *yours = new QLabel(tr("Your effects"), left);
    yours->setProperty("role", QStringLiteral("muted"));
    lv->addWidget(yours);
    m_list = new QListWidget(left);
    m_list->setObjectName(QStringLiteral("effectList"));
    m_list->setAccessibleName(tr("Your effects"));
    lv->addWidget(m_list, 1);
    auto *listButtons = new QHBoxLayout;
    auto *add = new QPushButton(tr("New"), left);
    add->setObjectName(QStringLiteral("effectNew"));
    m_delete = new QPushButton(tr("Delete"), left);
    m_delete->setObjectName(QStringLiteral("effectDelete"));
    listButtons->addWidget(add);
    listButtons->addWidget(m_delete);
    lv->addLayout(listButtons);

    // The source and what the compiler said.
    auto *middle = new QWidget(split);
    auto *mv = new QVBoxLayout(middle);
    mv->setContentsMargins(0, 0, 0, 0);
    auto *nameRow = new QHBoxLayout;
    auto *nameLabel = new QLabel(tr("Name"), middle);
    m_name = new QLineEdit(middle);
    m_name->setObjectName(QStringLiteral("effectName"));
    nameLabel->setBuddy(m_name);
    auto *open = new QPushButton(tr("Open…"), middle);
    open->setObjectName(QStringLiteral("effectOpen"));
    open->setToolTip(tr("Look at an effect someone shared (a .slang file); it is added only when you save it"));
    auto *exportButton = new QPushButton(tr("Export…"), middle);
    exportButton->setObjectName(QStringLiteral("effectExport"));
    exportButton->setToolTip(tr("Save a copy of the source to share"));
    m_save = new QPushButton(tr("Save"), middle);
    m_save->setObjectName(QStringLiteral("effectSave"));
    m_save->setDefault(true);
    m_save->setToolTip(tr("Keep it in your effects folder: it is then a step of the filter editor"));
    nameRow->addWidget(nameLabel);
    nameRow->addWidget(m_name, 1);
    nameRow->addWidget(open);
    nameRow->addWidget(exportButton);
    nameRow->addWidget(m_save);
    mv->addLayout(nameRow);
    // Find and replace (Ctrl+F, Ctrl+H), over the source.
    m_findBar = new QWidget(middle);
    m_findBar->setObjectName(QStringLiteral("effectFindBar"));
    auto *fv = new QVBoxLayout(m_findBar);
    fv->setContentsMargins(0, 0, 0, 0);
    auto *findRow = new QHBoxLayout;
    m_findText = new QLineEdit(m_findBar);
    m_findText->setObjectName(QStringLiteral("effectFind"));
    m_findText->setPlaceholderText(tr("Find"));
    m_findText->setClearButtonEnabled(true);
    auto *previous = new QPushButton(tr("Previous"), m_findBar);
    auto *next = new QPushButton(tr("Next"), m_findBar);
    m_caseBox = new QCheckBox(tr("Match case"), m_findBar);
    m_wordsBox = new QCheckBox(tr("Whole words"), m_findBar);
    auto *closeFind = new QToolButton(m_findBar);
    closeFind->setText(QStringLiteral("✕"));
    closeFind->setToolTip(tr("Close (Esc)"));
    closeFind->setAutoRaise(true);
    findRow->addWidget(m_findText, 1);
    findRow->addWidget(previous);
    findRow->addWidget(next);
    findRow->addWidget(m_caseBox);
    findRow->addWidget(m_wordsBox);
    findRow->addWidget(closeFind);
    fv->addLayout(findRow);
    m_replaceRow = new QWidget(m_findBar);
    auto *replaceRow = new QHBoxLayout(m_replaceRow);
    replaceRow->setContentsMargins(0, 0, 0, 0);
    m_replaceText = new QLineEdit(m_replaceRow);
    m_replaceText->setObjectName(QStringLiteral("effectReplace"));
    m_replaceText->setPlaceholderText(tr("Replace with"));
    auto *replaceOne = new QPushButton(tr("Replace"), m_replaceRow);
    auto *replaceAll = new QPushButton(tr("Replace all"), m_replaceRow);
    replaceRow->addWidget(m_replaceText, 1);
    replaceRow->addWidget(replaceOne);
    replaceRow->addWidget(replaceAll);
    fv->addWidget(m_replaceRow);
    m_findBar->hide();
    mv->addWidget(m_findBar);
    auto findFlags = [this] { return std::pair{m_caseBox->isChecked(), m_wordsBox->isChecked()}; };
    auto findNext = [this, findFlags](bool backwards) {
        const auto [matchCase, words] = findFlags();
        const bool found = m_editor->find(m_findText->text(), backwards, matchCase, words);
        m_findText->setProperty("role", found || m_findText->text().isEmpty() ? QVariant() : QVariant(QStringLiteral("error")));
        m_findText->style()->unpolish(m_findText);
        m_findText->style()->polish(m_findText);
        return found;
    };
    connect(m_findText, &QLineEdit::returnPressed, this, [findNext] { findNext(false); });
    connect(m_findText, &QLineEdit::textEdited, this, [this, findNext] {
        // As you type: from where the match started.
        QTextCursor c = m_editor->textCursor();
        c.setPosition(c.selectionStart());
        m_editor->setTextCursor(c);
        findNext(false);
    });
    connect(next, &QPushButton::clicked, this, [findNext] { findNext(false); });
    connect(previous, &QPushButton::clicked, this, [findNext] { findNext(true); });
    connect(replaceOne, &QPushButton::clicked, this, [this, findFlags] {
        const auto [matchCase, words] = findFlags();
        m_editor->replaceNext(m_findText->text(), m_replaceText->text(), matchCase, words);
    });
    connect(replaceAll, &QPushButton::clicked, this, [this, findFlags] {
        const auto [matchCase, words] = findFlags();
        const int n = m_editor->replaceAll(m_findText->text(), m_replaceText->text(), matchCase, words);
        statusBar()->showMessage(tr("%n replaced.", nullptr, n), 4000);
    });
    connect(closeFind, &QToolButton::clicked, this, [this] {
        m_findBar->hide();
        m_editor->setFocus();
    });
    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), m_findBar, nullptr, nullptr, Qt::WidgetWithChildrenShortcut);
    connect(escape, &QShortcut::activated, closeFind, &QToolButton::click);

    auto *codeSplit = new QSplitter(Qt::Vertical, middle);
    m_editor = new CodeEditor(codeSplit);
    m_editor->setObjectName(QStringLiteral("effectSource"));
    m_editor->setAccessibleName(tr("Source"));
    m_problems = new QListWidget(codeSplit);
    m_problems->setObjectName(QStringLiteral("effectProblems"));
    m_problems->setAccessibleName(tr("Problems"));
    codeSplit->addWidget(m_editor);
    codeSplit->addWidget(m_problems);
    codeSplit->setStretchFactor(0, 4);
    codeSplit->setStretchFactor(1, 1);
    mv->addWidget(codeSplit, 1);
    m_status = new QLabel(middle);
    m_status->setObjectName(QStringLiteral("effectStatus"));
    m_status->setWordWrap(true);
    mv->addWidget(m_status);

    // The effect on the picture, and its sliders.
    auto *right = new QWidget(split);
    auto *rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);
    m_previewLabel = new QLabel(right);
    m_previewLabel->setObjectName(QStringLiteral("effectPreview"));
    m_previewLabel->setAlignment(Qt::AlignCenter);
    m_previewLabel->setMinimumSize(240, 180);
    m_previewLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    rv->addWidget(m_previewLabel, 3);
    auto *tabs = new QTabWidget(right);
    tabs->setObjectName(QStringLiteral("effectSideTabs"));
    tabs->setDocumentMode(true);
    auto *scroll = new QScrollArea(tabs);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_fieldsHost = new QWidget(scroll);
    m_fields = new QVBoxLayout(m_fieldsHost);
    m_fields->addStretch();
    scroll->setWidget(m_fieldsHost);
    tabs->addTab(scroll, tr("Settings"));
    // What Lumen gives effects, with a sentence each; a double-click puts it in the source.
    m_reference = new QTreeWidget(tabs);
    m_reference->setObjectName(QStringLiteral("effectReference"));
    m_reference->setHeaderHidden(true);
    m_reference->setRootIsDecorated(true);
    const std::pair<QString, QString> groups[] = {{QStringLiteral("attribute"), tr("Attributes (import effect)")},
                                                  {QStringLiteral("lumen"), tr("Lumen's functions")},
                                                  {QStringLiteral("function"), tr("Slang's functions")}};
    for (const auto &[kind, title] : groups) {
        auto *group = new QTreeWidgetItem(m_reference, {title});
        group->setFlags(Qt::ItemIsEnabled);
        for (const QString &word : documentedWords(kind)) {
            const CodeDoc doc = codeDoc(word);
            auto *item = new QTreeWidgetItem(group, {doc.signature});
            item->setToolTip(0, doc.text);
            item->setData(0, Qt::UserRole, doc.signature);
        }
        group->setExpanded(kind != QLatin1String("function"));
    }
    connect(m_reference, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        const QString text = item->data(0, Qt::UserRole).toString();
        if (text.isEmpty())
            return;
        m_editor->insertPlainText(text);
        m_editor->setFocus();
    });
    tabs->addTab(m_reference, tr("Reference"));
    rv->addWidget(tabs, 2);

    split->addWidget(left);
    split->addWidget(middle);
    split->addWidget(right);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 4);
    split->setStretchFactor(2, 3);
    setCentralWidget(central);

    m_compileTimer.setSingleShot(true);
    m_compileTimer.setInterval(700);
    connect(&m_compileTimer, &QTimer::timeout, this, &EffectEditorWindow::compileNow);
    connect(m_editor, &QPlainTextEdit::textChanged, this, [this] {
        m_dirty = m_editor->toPlainText() != m_savedSource;
        m_compileTimer.start();
    });
    connect(m_problems, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        const int line = item->data(Qt::UserRole).toInt();
        const int column = item->data(Qt::UserRole + 1).toInt();
        if (line > 0)
            m_editor->goToLine(line, column);
    });
    connect(m_problems, &QListWidget::itemClicked, m_problems, &QListWidget::itemActivated);
    connect(m_list, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        if (item->text() != effectName() && confirmDiscard())
            openEffect(item->data(Qt::UserRole).toString());
    });
    connect(m_list, &QListWidget::itemClicked, m_list, &QListWidget::itemActivated);
    connect(add, &QPushButton::clicked, this, [this] {
        if (confirmDiscard())
            newEffect();
    });
    connect(m_delete, &QPushButton::clicked, this, [this] {
        const QString name = effectName();
        if (m_effects->item(name).path.isEmpty())
            return;
        if (QMessageBox::question(this, tr("Delete effect"), tr("Delete “%1”? Its file goes to the recycle bin.").arg(name))
            != QMessageBox::Yes)
            return;
        if (m_effects->remove(name))
            newEffect();
    });
    connect(open, &QPushButton::clicked, this, [this] {
        if (!confirmDiscard())
            return;
        const QString file = QFileDialog::getOpenFileName(this, tr("Open an effect"), QString(), tr("Slang effects (*.slang)"));
        QString error;
        if (!file.isEmpty() && !importFile(file, &error))
            QMessageBox::warning(this, tr("Open an effect"), error);
    });
    connect(exportButton, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getSaveFileName(this, tr("Export the effect"), effectName() + QStringLiteral(".slang"),
                                                          tr("Slang effects (*.slang)"));
        if (file.isEmpty())
            return;
        QSaveFile out(file);
        if (!out.open(QIODevice::WriteOnly) || out.write(source().toUtf8()) < 0 || !out.commit())
            QMessageBox::warning(this, tr("Export the effect"), tr("%1 could not be written.").arg(QDir::toNativeSeparators(file)));
    });
    connect(m_save, &QPushButton::clicked, this, &EffectEditorWindow::save);
    connect(m_download, &QPushButton::clicked, this, [this] {
        if (m_effects->installCompiler())
            m_studio->notify(tr("Downloading the Slang compiler; the Queue shows how far it is."), QStringLiteral("info"));
        updateBanner();
    });
    connect(m_effects, &UserEffects::changed, this, &EffectEditorWindow::fillList);
    connect(m_effects, &UserEffects::compilerInstalled, this, [this] {
        updateBanner();
        compileNow();
    });

    buildMenus(add, open, exportButton);
    m_position = new QLabel(this);
    m_position->setObjectName(QStringLiteral("effectPosition"));
    statusBar()->addPermanentWidget(m_position);
    connect(m_editor, &CodeEditor::cursorMoved, this, [this](int line, int column) {
        m_position->setText(tr("Line %1, column %2").arg(line).arg(column));
    });
    m_position->setText(tr("Line %1, column %2").arg(1).arg(1));

    fillList();
    const QList<UserEffects::Item> items = m_effects->items();
    if (items.isEmpty())
        newEffect();
    else
        openEffect(items.first().name);
}

EffectEditorWindow::~EffectEditorWindow()
{
    gpu::Effect::removeCompiled(m_previewId);
}

void EffectEditorWindow::closeEvent(QCloseEvent *e)
{
    if (!confirmDiscard()) {
        e->ignore();
        return;
    }
    QMainWindow::closeEvent(e);
}

bool EffectEditorWindow::confirmDiscard()
{
    if (!m_dirty)
        return true;
    const auto answer = QMessageBox::question(this, tr("Effect editor"), tr("“%1” has changes that are not saved. Save them?").arg(effectName()),
                                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Cancel)
        return false;
    if (answer == QMessageBox::Save)
        return save();
    return true;
}

void EffectEditorWindow::buildMenus(QPushButton *add, QPushButton *open, QPushButton *exportButton)
{
    auto action = [this](QMenu *menu, const QString &text, const QKeySequence &key, auto &&slot, const QString &name = {}) {
        QAction *a = menu->addAction(text, this, std::forward<decltype(slot)>(slot));
        if (!key.isEmpty()) {
            a->setShortcut(key);
            a->setShortcutContext(Qt::WindowShortcut);
        }
        if (!name.isEmpty())
            a->setObjectName(name);
        return a;
    };
    QMenu *file = menuBar()->addMenu(tr("&File"));
    action(file, tr("New effect"), QKeySequence::New, [add] { add->click(); });
    action(file, tr("Open…"), QKeySequence::Open, [open] { open->click(); });
    action(file, tr("Save"), QKeySequence::Save, [this] { save(); }, QStringLiteral("effectSaveAction"));
    action(file, tr("Export…"), QKeySequence(), [exportButton] { exportButton->click(); });
    file->addSeparator();
    action(file, tr("Close"), QKeySequence::Close, [this] { close(); });

    QMenu *edit = menuBar()->addMenu(tr("&Edit"));
    action(edit, tr("Undo"), QKeySequence::Undo, [this] { m_editor->undo(); });
    action(edit, tr("Redo"), QKeySequence::Redo, [this] { m_editor->redo(); });
    edit->addSeparator();
    action(edit, tr("Find…"), QKeySequence::Find, [this] { showFind(false); }, QStringLiteral("effectFindAction"));
    action(edit, tr("Replace…"), QKeySequence(Qt::CTRL | Qt::Key_H), [this] { showFind(true); });
    action(edit, tr("Find next"), QKeySequence::FindNext, [this] { m_editor->find(m_findText->text(), false, m_caseBox->isChecked(), m_wordsBox->isChecked()); });
    action(edit, tr("Find previous"), QKeySequence::FindPrevious,
           [this] { m_editor->find(m_findText->text(), true, m_caseBox->isChecked(), m_wordsBox->isChecked()); });
    action(edit, tr("Go to line…"), QKeySequence(Qt::CTRL | Qt::Key_G), [this] {
        bool ok = false;
        const int line = QInputDialog::getInt(this, tr("Go to line"), tr("Line (1 to %1):").arg(m_editor->blockCount()),
                                              m_editor->currentLine(), 1, m_editor->blockCount(), 1, &ok);
        if (ok)
            m_editor->goToLine(line);
    });
    edit->addSeparator();
    action(edit, tr("Comment or uncomment"), QKeySequence(Qt::CTRL | Qt::Key_Slash), [this] { m_editor->toggleComment(); });
    action(edit, tr("Indent"), QKeySequence(), [this] { m_editor->indentSelection(false); });
    action(edit, tr("Unindent"), QKeySequence(), [this] { m_editor->indentSelection(true); });
    action(edit, tr("Complete word"), QKeySequence(Qt::CTRL | Qt::Key_Space), [this] { m_editor->complete(); });

    QMenu *build = menuBar()->addMenu(tr("&Build"));
    action(build, tr("Compile now"), QKeySequence(Qt::CTRL | Qt::Key_B), [this] { compileNow(); }, QStringLiteral("effectCompileAction"));
    action(build, tr("Next problem"), QKeySequence(Qt::Key_F8), [this] { m_editor->nextProblem(false); });
    action(build, tr("Previous problem"), QKeySequence(Qt::SHIFT | Qt::Key_F8), [this] { m_editor->nextProblem(true); });

    QMenu *view = menuBar()->addMenu(tr("&View"));
    action(view, tr("Larger text"), QKeySequence::ZoomIn, [this] { m_editor->zoom(1); });
    action(view, tr("Smaller text"), QKeySequence::ZoomOut, [this] { m_editor->zoom(-1); });
    action(view, tr("Normal text size"), QKeySequence(Qt::CTRL | Qt::Key_0), [this] { m_editor->zoom(0); });
}

void EffectEditorWindow::showFind(bool replace)
{
    m_findBar->show();
    m_replaceRow->setVisible(replace);
    // The selected word, or the one at the cursor, to look for.
    QTextCursor c = m_editor->textCursor();
    if (!c.hasSelection())
        c.select(QTextCursor::WordUnderCursor);
    if (!c.selectedText().isEmpty() && !c.selectedText().contains(QChar::ParagraphSeparator))
        m_findText->setText(c.selectedText());
    m_findText->setFocus();
    m_findText->selectAll();
}

void EffectEditorWindow::fillList()
{
    const QString current = effectName();
    QSignalBlocker block(m_list);
    m_list->clear();
    for (const UserEffects::Item &item : m_effects->items()) {
        auto *row = new QListWidgetItem(item.name, m_list);
        row->setData(Qt::UserRole, item.name);
        if (!item.ready) {
            row->setForeground(Theme::palette().textMuted);
            row->setToolTip(item.problem);
        }
        if (item.name == current)
            m_list->setCurrentItem(row);
    }
    m_delete->setEnabled(!m_effects->item(current).path.isEmpty());
    updateBanner();
}

QString EffectEditorWindow::source() const
{
    return m_editor->toPlainText();
}

void EffectEditorWindow::setSource(const QString &source)
{
    m_editor->setPlainText(source);
}

QString EffectEditorWindow::effectName() const
{
    return m_name->text().trimmed();
}

void EffectEditorWindow::setEffectName(const QString &name)
{
    m_name->setText(name);
}

void EffectEditorWindow::newEffect()
{
    m_importedFrom.clear();
    m_values.clear();
    setEffectName(m_effects->freeName(tr("My effect")));
    m_savedSource.clear();
    setSource(templateSource());
    m_dirty = true;
    fillList();
    compileNow();
}

bool EffectEditorWindow::openEffect(const QString &name)
{
    const UserEffects::Item item = m_effects->item(name);
    QFile file(item.path);
    if (item.path.isEmpty() || !file.open(QIODevice::ReadOnly))
        return false;
    m_importedFrom.clear();
    m_values.clear();
    setEffectName(name);
    m_savedSource = QString::fromUtf8(file.readAll());
    setSource(m_savedSource);
    m_dirty = false;
    fillList();
    compileNow();
    return true;
}

bool EffectEditorWindow::importFile(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = tr("%1 could not be read.").arg(QDir::toNativeSeparators(path));
        return false;
    }
    const QByteArray bytes = file.read(2 << 20);
    m_importedFrom = path;
    m_values.clear();
    setEffectName(m_effects->freeName(QFileInfo(path).completeBaseName()));
    m_savedSource.clear();
    setSource(QString::fromUtf8(bytes));
    m_dirty = true;
    fillList();
    compileNow();
    return true;
}

void EffectEditorWindow::updateBanner()
{
    QString text;
    bool download = false;
    if (!m_effects->compilerReady()) {
        const qint64 size = m_effects->compilerDownloadSize();
        text = tr("To compile effects, Lumen needs the Slang compiler: a download of %1 MB from the Slang project on GitHub, once. "
                  "Effects you have already compiled work without it.")
                   .arg(QLocale().toString(size / 1e6, 'f', 0));
        download = size > 0;
    } else if (!m_importedFrom.isEmpty()) {
        text = tr("This effect comes from %1 and is not saved yet. Read it before you save it: an effect runs on your graphics "
                  "card, and a careless or unkind one can make the screen stop for a moment.")
                   .arg(QDir::toNativeSeparators(m_importedFrom));
    }
    m_banner->setText(text);
    m_banner->setVisible(!text.isEmpty());
    m_download->setVisible(download);
}

void EffectEditorWindow::compileNow()
{
    m_compileTimer.stop();
    updateBanner();
    if (!m_effects->compilerReady()) {
        m_error = tr("Not compiled: the Slang compiler is not installed.");
        m_messages.clear();
        showMessages();
        showPicture();
        emit updated();
        return;
    }
    const int generation = ++m_generation;
    m_compiling = true;
    m_status->setText(tr("Compiling…"));
    QPointer<EffectEditorWindow> self(this);
    m_effects->compile(source(), effectName()).then(this, [self, generation](const gpu::Compiled &result) {
        if (self)
            self->compiled(result, generation);
    });
}

void EffectEditorWindow::compiled(const gpu::Compiled &result, int generation)
{
    if (generation != m_generation)
        return; // the source changed meanwhile: a newer compile is on its way
    m_compiling = false;
    m_messages = result.messages;
    m_error = result.error;
    if (result.ok()) {
        QString why;
        if (gpu::Effect::addCompiled(m_previewId, result.spirv, result.reflection, &why).isNull()) {
            m_error = why;
            m_node.reset();
        } else {
            m_node = filters::effects::yourNode(m_previewId, effectName());
        }
    }
    showMessages();
    rebuildFields();
    if (m_error.isEmpty() && m_node) {
        renderPreview();
    } else {
        showPicture();
        emit updated();
    }
}

void EffectEditorWindow::showMessages()
{
    m_problems->clear();
    QList<CodeProblem> problems;
    const Palette &pal = Theme::palette();
    for (const gpu::CompileMessage &m : std::as_const(m_messages)) {
        const bool error = m.severity == QLatin1String("error");
        const QString where = m.inSource && m.line > 0 ? tr("Line %1").arg(m.line) : tr("In a module");
        auto *item = new QListWidgetItem(QStringLiteral("%1: %2").arg(where, m.text), m_problems);
        item->setForeground(error ? pal.danger : pal.warning);
        item->setData(Qt::UserRole, m.inSource ? m.line : 0);
        item->setData(Qt::UserRole + 1, m.column);
        if (m.inSource && m.line > 0)
            problems << CodeProblem{m.line, m.column, m.endLine, m.endColumn, error, m.text};
    }
    // A problem Lumen found that is not the compiler's (no "result", a texture not rgba16f); a
    // missing compiler is said in the banner and the status line.
    if (!m_error.isEmpty() && m_messages.isEmpty() && m_effects->compilerReady()) {
        auto *item = new QListWidgetItem(m_error, m_problems);
        item->setForeground(pal.danger);
    }
    m_editor->setProblems(problems);
    m_status->setText(status());
}

QString EffectEditorWindow::status() const
{
    if (m_compiling)
        return tr("Compiling…");
    if (!m_error.isEmpty()) {
        int errors = 0;
        for (const gpu::CompileMessage &m : m_messages)
            errors += m.severity == QLatin1String("error");
        return errors > 0 ? tr("%n error(s): the preview shows the last version that worked.", nullptr, errors) : m_error;
    }
    return m_dirty ? tr("Compiled. Not saved yet.") : tr("Compiled and saved.");
}

ParamField *EffectEditorWindow::field(const QString &param) const
{
    for (ParamField *f : m_paramFields) {
        if (f->spec().id == param)
            return f;
    }
    return nullptr;
}

void EffectEditorWindow::rebuildFields()
{
    if (!m_node)
        return;
    // The same sliders keep their values while the source changes around them.
    for (ParamField *f : std::as_const(m_paramFields)) {
        m_values.insert(f->spec().id, f->value());
        f->deleteLater();
    }
    m_paramFields.clear();
    int at = 0;
    for (const filters::ParamSpec &spec : m_node->info().params) {
        const QVariant value = m_values.contains(spec.id) ? filters::coerceParam(spec, m_values.value(spec.id)) : spec.defaultValue;
        auto *f = new ParamField(spec, value, {}, 0.0, 0.0, m_fieldsHost);
        f->setObjectName(QStringLiteral("param:%1").arg(spec.id));
        connect(f, &ParamField::valueChanged, this, [this, f](const QVariant &v) {
            m_values.insert(f->spec().id, v);
            renderPreview();
        });
        m_fields->insertWidget(at++, f);
        m_paramFields << f;
    }
}

QImage EffectEditorWindow::previewSource() const
{
    // The active layer, at a size that is quick; a sample picture without one.
    QImage image;
    if (doc::Document *doc = m_studio->activeDocument()) {
        if (const doc::LayerPtr layer = doc->activeLayer())
            image = layer->image;
    }
    if (image.isNull())
        image = filterSampleImage();
    if (std::max(image.width(), image.height()) > 900)
        image = img::scaled(image, image.size().scaled(900, 900, Qt::KeepAspectRatio), true);
    return img::normalized(image);
}

void EffectEditorWindow::showPicture()
{
    // Nothing that worked yet: the picture as it is.
    if (!m_previewImage.isNull())
        return;
    const QImage picture = previewSource();
    m_previewLabel->setPixmap(QPixmap::fromImage(picture.scaled(m_previewLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
}

void EffectEditorWindow::renderPreview()
{
    if (!m_node)
        return;
    if (m_previewing) {
        m_previewAgain = true;
        return;
    }
    m_previewing = true;
    QVariantMap params = m_values;
    for (ParamField *f : std::as_const(m_paramFields))
        params.insert(f->spec().id, f->value());
    const filters::NodeTypePtr node = m_node;
    const QImage picture = previewSource();
    QPointer<EffectEditorWindow> self(this);
    async::run([node, picture, params]() -> QPair<QImage, QString> {
        try {
            const filters::Values out = filters::evaluateNode(*node, {{QStringLiteral("image"), picture}}, params);
            return {filters::imageOf(out.value(QStringLiteral("image"))), QString()};
        } catch (const std::exception &e) {
            return {QImage(), describeException(e)};
        }
    }).then(this, [self](const QPair<QImage, QString> &result) {
        if (!self)
            return;
        self->m_previewing = false;
        if (!result.first.isNull()) {
            self->m_previewImage = result.first;
            const QSize room = self->m_previewLabel->size();
            self->m_previewLabel->setPixmap(QPixmap::fromImage(result.first.scaled(room, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        } else {
            self->m_status->setText(tr("The preview did not work: %1").arg(result.second));
        }
        if (self->m_previewAgain) {
            self->m_previewAgain = false;
            self->renderPreview();
            return;
        }
        emit self->updated();
    });
}

bool EffectEditorWindow::save()
{
    const QString name = effectName();
    if (!UserEffects::isValidName(name)) {
        QMessageBox::warning(this, tr("Save the effect"), tr("Give the effect a name that can be a file's name (no \\ / : * ? \" < > |)."));
        m_name->setFocus();
        return false;
    }
    m_importedFrom.clear();
    m_savedSource = source();
    m_dirty = false;
    QPointer<EffectEditorWindow> self(this);
    m_effects->save(name, m_savedSource).then(this, [self](const gpu::Compiled &result) {
        if (!self)
            return;
        if (!result.ok() && result.messages.isEmpty())
            self->m_status->setText(result.error);
        self->fillList();
    });
    updateBanner();
    m_status->setText(status());
    return true;
}

} // namespace lumen::ui
