#include "ui/dialogs/SelectByDescriptionDialog.h"

#include "core/ImageOps.h"
#include "doc/Document.h"
#include "ui/SmartSelect.h"
#include "ui/widgets/Controls.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace lumen::ui {

namespace {

QString colorLabel(const QString &name)
{
    static const QHash<QString, const char *> names{
        {QStringLiteral("red"), QT_TRANSLATE_NOOP("lumen::ui::SelectByDescriptionDialog", "red")},
        {QStringLiteral("orange"), QT_TRANSLATE_NOOP("lumen::ui::SelectByDescriptionDialog", "orange")},
        {QStringLiteral("yellow"), QT_TRANSLATE_NOOP("lumen::ui::SelectByDescriptionDialog", "yellow")},
        {QStringLiteral("green"), QT_TRANSLATE_NOOP("lumen::ui::SelectByDescriptionDialog", "green")},
        {QStringLiteral("cyan"), QT_TRANSLATE_NOOP("lumen::ui::SelectByDescriptionDialog", "cyan")},
        {QStringLiteral("blue"), QT_TRANSLATE_NOOP("lumen::ui::SelectByDescriptionDialog", "blue")},
        {QStringLiteral("purple"), QT_TRANSLATE_NOOP("lumen::ui::SelectByDescriptionDialog", "purple")},
        {QStringLiteral("pink"), QT_TRANSLATE_NOOP("lumen::ui::SelectByDescriptionDialog", "pink")},
        {QStringLiteral("brown"), QT_TRANSLATE_NOOP("lumen::ui::SelectByDescriptionDialog", "brown")},
        {QStringLiteral("black"), QT_TRANSLATE_NOOP("lumen::ui::SelectByDescriptionDialog", "black")},
        {QStringLiteral("white"), QT_TRANSLATE_NOOP("lumen::ui::SelectByDescriptionDialog", "white")},
        {QStringLiteral("gray"), QT_TRANSLATE_NOOP("lumen::ui::SelectByDescriptionDialog", "gray")},
    };
    const char *text = names.value(name);
    return text ? QCoreApplication::translate("lumen::ui::SelectByDescriptionDialog", text) : name;
}

} // namespace

QString finderDescription(const img::FinderRequest &r)
{
    switch (r.kind) {
    case img::FinderRequest::Kind::Strokes:
        return (r.strokes.dark ? QCoreApplication::translate("lumen::ui::SelectByDescriptionDialog", "Lines darker than what is around them, up to %1 px wide.")
                               : QCoreApplication::translate("lumen::ui::SelectByDescriptionDialog", "Lines lighter than what is around them, up to %1 px wide."))
            .arg(r.strokes.maxWidth);
    case img::FinderRequest::Kind::Specks:
        return (r.specks.tone == img::SpeckOptions::Tone::Dark  ? QCoreApplication::translate("lumen::ui::SelectByDescriptionDialog", "Dark specks up to %1 px across.")
                : r.specks.tone == img::SpeckOptions::Tone::Light ? QCoreApplication::translate("lumen::ui::SelectByDescriptionDialog", "Light specks up to %1 px across.")
                                                                  : QCoreApplication::translate("lumen::ui::SelectByDescriptionDialog", "Dark and light specks up to %1 px across."))
            .arg(r.specks.maxSize);
    case img::FinderRequest::Kind::Color:
        return QCoreApplication::translate("lumen::ui::SelectByDescriptionDialog", "Everything %1, in all its shades.").arg(colorLabel(r.color));
    case img::FinderRequest::Kind::None: break;
    }
    return QCoreApplication::translate("lumen::ui::SelectByDescriptionDialog", "Lumen finds lines, scratches, dust and colours by itself (e.g. \"the black lines\", \"dust\", \"the red\"). "
              "Objects such as \"the cup\" need a detection model, which comes in a later version.");
}

QString selectionWords(const QString &words)
{
    static const QRegularExpression ask(QStringLiteral("^\\s*(?:select|auswählen|wähle|markiere)\\b\\s*:?\\s*(.*)$"),
                                        QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption
                                            | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch m = ask.match(words);
    return m.hasMatch() ? m.captured(1).trimmed() : QString();
}

SelectByDescriptionDialog::SelectByDescriptionDialog(doc::Document *doc, QWidget *parent)
    : QDialog(parent)
    , m_doc(doc)
{
    setWindowTitle(tr("Select by description"));
    m_picture = doc ? doc->flattened() : QImage();
    auto *v = new QVBoxLayout(this);
    v->addWidget(sectionLabel(tr("What to select"), this));
    m_words = new QLineEdit(this);
    m_words->setObjectName(QStringLiteral("selectWords"));
    m_words->setAccessibleName(tr("What to select"));
    m_words->setPlaceholderText(tr("e.g. the black lines, dust, the red"));
    m_words->setClearButtonEnabled(true);
    v->addWidget(m_words);
    m_understood = new QLabel(this);
    m_understood->setObjectName(QStringLiteral("selectUnderstood"));
    m_understood->setWordWrap(true);
    m_understood->setProperty("role", QStringLiteral("muted"));
    v->addWidget(m_understood);

    // Lines and specks: dark or light, the size, the contrast.
    m_strokeBox = new QWidget(this);
    auto *sf = new QFormLayout(m_strokeBox);
    sf->setContentsMargins(0, 0, 0, 0);
    m_tone = new QComboBox(m_strokeBox);
    m_tone->setAccessibleName(tr("Dark or light"));
    sf->addRow(tr("Find"), m_tone);
    m_width = new QSpinBox(m_strokeBox);
    m_width->setAccessibleName(tr("Size"));
    m_width->setRange(1, 64);
    m_width->setSuffix(tr(" px"));
    sf->addRow(tr("Up to"), m_width);
    m_contrast = new SliderField(tr("Contrast"), 1, 255, 40, 0, QString(), m_strokeBox);
    m_contrast->setToolTip(tr("How much darker (or lighter) than what is around it something must be to be found. Lower finds fainter "
                              "lines and more of the paper's grain."));
    sf->addRow(m_contrast);
    v->addWidget(m_strokeBox);
    m_colorBox = new QWidget(this);
    auto *cf = new QVBoxLayout(m_colorBox);
    cf->setContentsMargins(0, 0, 0, 0);
    m_tolerance = new SliderField(tr("How wide a range"), 0, 100, 50, 0, QString(), m_colorBox);
    m_tolerance->setToolTip(tr("Higher takes in more shades near the colour."));
    cf->addWidget(m_tolerance);
    v->addWidget(m_colorBox);

    m_download = new QPushButton(this);
    m_download->setObjectName(QStringLiteral("selectDownload"));
    m_download->setToolTip(tr("Grounding DINO tiny finds objects by name and SAM 2.1 cuts them out; they run on this computer."));
    m_download->hide();
    v->addWidget(m_download, 0, Qt::AlignLeft);
    connect(m_download, &QPushButton::clicked, this, [this] {
        if (!m_finder)
            return;
        m_finder->installObjects();
        m_download->setEnabled(false);
        m_download->setText(tr("Downloading… (the Queue shows how far it is)"));
    });

    m_preview = new QLabel(this);
    m_preview->setObjectName(QStringLiteral("selectPreview"));
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setMinimumSize(240, 160);
    m_preview->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    v->addWidget(m_preview, 1);
    m_result = new QLabel(this);
    m_result->setObjectName(QStringLiteral("selectResult"));
    m_result->setWordWrap(true);
    v->addWidget(m_result);

    auto *modes = new QHBoxLayout;
    m_mode = new QButtonGroup(this);
    int id = 0;
    for (const QString &label : {tr("New selection"), tr("Add"), tr("Take away"), tr("Where both are")}) {
        auto *b = new QRadioButton(label, this);
        m_mode->addButton(b, id++);
        modes->addWidget(b);
    }
    m_mode->button(0)->setChecked(true);
    modes->addStretch(1);
    v->addLayout(modes);
    auto *buttons = new QDialogButtonBox(this);
    m_select = buttons->addButton(tr("Select"), QDialogButtonBox::AcceptRole);
    m_select->setDefault(true);
    buttons->addButton(QDialogButtonBox::Cancel);
    v->addWidget(buttons);

    m_debounce.setSingleShot(true);
    m_debounce.setInterval(150);
    connect(&m_debounce, &QTimer::timeout, this, &SelectByDescriptionDialog::find);
    connect(m_words, &QLineEdit::textChanged, this, &SelectByDescriptionDialog::parseWords);
    connect(m_tone, &QComboBox::activated, this, &SelectByDescriptionDialog::readSettings);
    connect(m_width, &QSpinBox::valueChanged, this, &SelectByDescriptionDialog::readSettings);
    connect(m_contrast, &SliderField::valueChanged, this, &SelectByDescriptionDialog::readSettings);
    connect(m_tolerance, &SliderField::valueChanged, this, &SelectByDescriptionDialog::readSettings);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (apply())
            accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    parseWords();
    resizeWithinScreen(this, QSize(560, 620));
}

void SelectByDescriptionDialog::setWords(const QString &words)
{
    m_words->setText(words);
}

void SelectByDescriptionDialog::setObjectFinder(SmartSelect *finder)
{
    m_finder = finder;
    // When the models arrive, what was typed is looked for.
    connect(finder, &SmartSelect::ready, this, [this] {
        if (m_objects)
            parseWords();
    });
    parseWords();
}

void SelectByDescriptionDialog::parseWords()
{
    m_request = img::parseFinderRequest(m_words->text());
    m_objects = m_finder && m_request.kind == img::FinderRequest::Kind::None && !m_words->text().trimmed().isEmpty();
    showSettings();
    find();
}

void SelectByDescriptionDialog::showSettings()
{
    using K = img::FinderRequest::Kind;
    m_understood->setText(m_words->text().trimmed().isEmpty() ? tr("Say what to select, and Lumen finds it in the picture.")
                          : m_objects ? tr("Objects: \"%1\", found by a detection model on this computer.").arg(m_words->text().trimmed())
                                      : finderDescription(m_request));
    const bool missing = m_objects && !m_finder->objectsReady();
    m_download->setVisible(missing);
    if (missing && m_download->isEnabled())
        m_download->setText(tr("Download the detection models (about %1 MB, once)").arg(QLocale().toString(m_finder->objectsMissingSize() / 1e6, 'f', 0)));
    const bool sized = m_request.kind == K::Strokes || m_request.kind == K::Specks;
    m_strokeBox->setVisible(sized);
    m_colorBox->setVisible(m_request.kind == K::Color);
    const QSignalBlocker b1(m_tone), b2(m_width), b3(m_contrast), b4(m_tolerance);
    m_tone->clear();
    if (m_request.kind == K::Strokes) {
        m_tone->addItem(tr("Dark lines"), QStringLiteral("dark"));
        m_tone->addItem(tr("Light lines"), QStringLiteral("light"));
        m_tone->setCurrentIndex(m_request.strokes.dark ? 0 : 1);
        m_width->setValue(m_request.strokes.maxWidth);
        m_contrast->setValue(m_request.strokes.contrast);
    } else if (m_request.kind == K::Specks) {
        m_tone->addItem(tr("Dark and light specks"), QStringLiteral("both"));
        m_tone->addItem(tr("Dark specks"), QStringLiteral("dark"));
        m_tone->addItem(tr("Light specks"), QStringLiteral("light"));
        m_tone->setCurrentIndex(m_request.specks.tone == img::SpeckOptions::Tone::Both   ? 0
                                : m_request.specks.tone == img::SpeckOptions::Tone::Dark ? 1
                                                                                          : 2);
        m_width->setValue(m_request.specks.maxSize);
        m_contrast->setValue(m_request.specks.contrast);
    }
    m_tolerance->setValue(m_request.tolerance);
}

void SelectByDescriptionDialog::readSettings()
{
    using K = img::FinderRequest::Kind;
    const QString tone = m_tone->currentData().toString();
    if (m_request.kind == K::Strokes) {
        m_request.strokes.dark = tone != QLatin1String("light");
        m_request.strokes.maxWidth = m_width->value();
        m_request.strokes.contrast = int(m_contrast->value());
    } else if (m_request.kind == K::Specks) {
        m_request.specks.tone = tone == QLatin1String("dark")    ? img::SpeckOptions::Tone::Dark
                                : tone == QLatin1String("light") ? img::SpeckOptions::Tone::Light
                                                                 : img::SpeckOptions::Tone::Both;
        m_request.specks.maxSize = m_width->value();
        m_request.specks.contrast = int(m_contrast->value());
    } else if (m_request.kind == K::Color) {
        m_request.tolerance = int(m_tolerance->value());
    }
    m_understood->setText(finderDescription(m_request));
    m_debounce.start();
}

void SelectByDescriptionDialog::find()
{
    const int run = ++m_run;
    m_found = QImage();
    if (m_objects && !m_picture.isNull()) {
        showPreview();
        if (!m_finder->objectsReady())
            return;
        m_result->setText(tr("Looking… (the first time takes a few seconds)"));
        QPointer<SelectByDescriptionDialog> self(this);
        m_finder->findObjects(m_picture, m_words->text().trimmed())
            .then(this, [self, run](const SmartSelect::FoundArea &area) {
                if (!self || run != self->m_run)
                    return;
                self->m_found = area.mask;
                self->showPreview();
                self->m_result->setText(area.mask.isNull() ? area.error : tr("Found %n object(s) (tinted).", nullptr, area.objects));
                emit self->foundChanged();
            });
        return;
    }
    if (m_request.kind == img::FinderRequest::Kind::None || m_picture.isNull()) {
        showPreview();
        emit foundChanged();
        return;
    }
    // The last result's tint goes while the new one is looked for.
    showPreview();
    m_result->setText(tr("Looking…"));
    QPointer<SelectByDescriptionDialog> self(this);
    QtConcurrent::run([picture = m_picture, request = m_request] { return img::runFinder(picture, request); })
        .then(this, [self, run](const QImage &mask) {
            if (!self || run != self->m_run)
                return;
            self->m_found = mask;
            self->showPreview();
            emit self->foundChanged();
        });
}

void SelectByDescriptionDialog::showPreview()
{
    const bool any = !m_found.isNull() && !img::maskIsEmpty(m_found);
    m_select->setEnabled(any);
    if (m_picture.isNull()) {
        m_result->setText(tr("There is no picture to select in."));
        return;
    }
    // The picture with the found area tinted, at the preview's size.
    const QSize box = m_preview->contentsRect().size().expandedTo(QSize(240, 160));
    QImage shown = m_picture.scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (any) {
        const QImage mask = img::scaled(m_found, shown.size());
        QImage tint(shown.size(), QImage::Format_ARGB32_Premultiplied);
        tint.fill(QColor(255, 40, 80));
        tint = img::applyMaskToAlpha(tint, mask);
        QPainter p(&shown);
        p.setOpacity(0.6);
        p.drawImage(0, 0, tint);
    }
    m_preview->setPixmap(QPixmap::fromImage(shown));
    if (m_request.kind == img::FinderRequest::Kind::None && !m_objects)
        m_result->clear();
    else if (m_objects)
        ; // findObjects says what it found
    else if (!any)
        m_result->setText(tr("Nothing like that was found. Try a lower contrast or a larger size."));
    else {
        qint64 count = 0;
        for (int y = 0; y < m_found.height(); ++y) {
            const uchar *line = m_found.constScanLine(y);
            for (int x = 0; x < m_found.width(); ++x)
                count += line[x] > 127;
        }
        const double percent = 100.0 * count / (qint64(m_found.width()) * m_found.height());
        m_result->setText(tr("Found %1 % of the picture (tinted).").arg(QLocale().toString(percent, 'f', percent < 1 ? 2 : 1)));
    }
}

void SelectByDescriptionDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    // The preview fills the room it has.
    if (!m_picture.isNull())
        showPreview();
}

bool SelectByDescriptionDialog::apply()
{
    if (!m_doc || m_found.isNull() || img::maskIsEmpty(m_found))
        return false;
    QImage mask = m_found;
    const QImage current = m_doc->selection();
    const int mode = m_mode->checkedId();
    if (mode > 0 && !current.isNull()) {
        const img::MaskOp op = mode == 1 ? img::MaskOp::Union : mode == 2 ? img::MaskOp::Subtract : img::MaskOp::Intersect;
        mask = img::combineMasks(current, m_found, op);
    } else if (mode == 2 || mode == 3) {
        return false; // nothing to take away from or to meet
    }
    m_doc->applySelection(mask, tr("Select: %1").arg(m_words->text().trimmed().left(40)));
    return true;
}

} // namespace lumen::ui
