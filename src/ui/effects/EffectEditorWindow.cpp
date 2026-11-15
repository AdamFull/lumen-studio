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
#include "ui/effects/UserEffects.h"
#include "ui/filters/FilterCatalog.h"
#include "ui/filters/ParamWidgets.h"

#include <QCloseEvent>
#include <QFileDialog>
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

// The source with line numbers beside it.
class CodeEditor : public QPlainTextEdit
{
public:
    explicit CodeEditor(QWidget *parent)
        : QPlainTextEdit(parent)
        , m_gutter(new Gutter(this))
    {
        setLineWrapMode(QPlainTextEdit::NoWrap);
        QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        mono.setPointSizeF(std::max(9.0, font().pointSizeF()));
        setFont(mono);
        setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);
        connect(this, &QPlainTextEdit::blockCountChanged, this, [this] { setViewportMargins(gutterWidth(), 0, 0, 0); });
        connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect &rect, int dy) {
            if (dy)
                m_gutter->scroll(0, dy);
            else
                m_gutter->update(0, rect.y(), m_gutter->width(), rect.height());
        });
        setViewportMargins(gutterWidth(), 0, 0, 0);
    }

    int gutterWidth() const
    {
        const int digits = std::max(3, int(QString::number(std::max(1, blockCount())).size()));
        return 10 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
    }

    void setErrorLines(const QSet<int> &lines)
    {
        m_errorLines = lines;
        m_gutter->update();
    }

protected:
    void resizeEvent(QResizeEvent *e) override
    {
        QPlainTextEdit::resizeEvent(e);
        const QRect r = contentsRect();
        m_gutter->setGeometry(r.left(), r.top(), gutterWidth(), r.height());
    }

private:
    class Gutter : public QWidget
    {
    public:
        explicit Gutter(CodeEditor *editor)
            : QWidget(editor)
            , m_editor(editor)
        {
        }

    protected:
        void paintEvent(QPaintEvent *e) override
        {
            QPainter p(this);
            const Palette &pal = Theme::palette();
            p.fillRect(e->rect(), pal.panel);
            QTextBlock block = m_editor->firstVisibleBlock();
            int top = qRound(m_editor->blockBoundingGeometry(block).translated(m_editor->contentOffset()).top());
            while (block.isValid() && top <= e->rect().bottom()) {
                const int height = qRound(m_editor->blockBoundingRect(block).height());
                if (block.isVisible()) {
                    const int line = block.blockNumber() + 1;
                    const bool error = m_editor->m_errorLines.contains(line);
                    p.setPen(error ? pal.danger : pal.textMuted);
                    QFont f = m_editor->font();
                    f.setBold(error);
                    p.setFont(f);
                    p.drawText(0, top, width() - 6, height, Qt::AlignRight | Qt::AlignVCenter, QString::number(line));
                }
                block = block.next();
                top += height;
            }
        }

    private:
        CodeEditor *m_editor;
    };

    Gutter *m_gutter;
    QSet<int> m_errorLines;
};

CodeEditor *codeEditor(QPlainTextEdit *e)
{
    return static_cast<CodeEditor *>(e);
}

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
    auto *scroll = new QScrollArea(right);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_fieldsHost = new QWidget(scroll);
    m_fields = new QVBoxLayout(m_fieldsHost);
    m_fields->addStretch();
    scroll->setWidget(m_fieldsHost);
    rv->addWidget(scroll, 2);

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
        if (line <= 0)
            return;
        QTextCursor cursor(m_editor->document()->findBlockByNumber(line - 1));
        cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, std::max(0, column - 1));
        m_editor->setTextCursor(cursor);
        m_editor->setFocus();
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
    QList<QTextEdit::ExtraSelection> marks;
    QSet<int> errorLines;
    const Palette &pal = Theme::palette();
    for (const gpu::CompileMessage &m : std::as_const(m_messages)) {
        const bool error = m.severity == QLatin1String("error");
        const QString where = m.inSource && m.line > 0 ? tr("Line %1").arg(m.line) : tr("In a module");
        auto *item = new QListWidgetItem(QStringLiteral("%1: %2").arg(where, m.text), m_problems);
        item->setForeground(error ? pal.danger : pal.warning);
        item->setData(Qt::UserRole, m.inSource ? m.line : 0);
        item->setData(Qt::UserRole + 1, m.column);
        if (!m.inSource || m.line <= 0)
            continue;
        if (error)
            errorLines.insert(m.line);
        // Underlined where it is.
        const QTextBlock block = m_editor->document()->findBlockByNumber(m.line - 1);
        if (!block.isValid())
            continue;
        QTextEdit::ExtraSelection mark;
        mark.cursor = QTextCursor(block);
        const int start = std::clamp(m.column - 1, 0, std::max(0, block.length() - 1));
        const int end = m.endLine == m.line && m.endColumn > m.column ? m.endColumn - 1 : block.length() - 1;
        mark.cursor.setPosition(block.position() + start);
        mark.cursor.setPosition(block.position() + std::max(start + 1, std::min(end, block.length() - 1)), QTextCursor::KeepAnchor);
        mark.format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
        mark.format.setUnderlineColor(error ? pal.danger : pal.warning);
        mark.format.setToolTip(m.text);
        marks << mark;
    }
    // A problem Lumen found that is not the compiler's (no "result", a texture not rgba16f); a
    // missing compiler is said in the banner and the status line.
    if (!m_error.isEmpty() && m_messages.isEmpty() && m_effects->compilerReady()) {
        auto *item = new QListWidgetItem(m_error, m_problems);
        item->setForeground(pal.danger);
    }
    m_editor->setExtraSelections(marks);
    codeEditor(m_editor)->setErrorLines(errorLines);
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
