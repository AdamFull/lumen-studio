#include "ui/widgets/DevelopPanel.h"

#include "core/ImageOps.h"
#include "doc/Document.h"
#include "ui/Studio.h"
#include "ui/canvas/CanvasView.h"
#include "ui/filters/ParamWidgets.h"
#include "ui/widgets/ColorWheel.h"
#include "ui/widgets/Controls.h"

#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace lumen::ui {

namespace {

const QString kDevelop = QStringLiteral("develop");

// The effects' words, which their shaders give in English.
[[maybe_unused]] const char *const kEffectWords[] = {
    QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Develop"),         QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "White balance"),
    QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Temperature"),     QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Tint"),
    QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Tone"),            QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Exposure"),
    QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Contrast"),        QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Highlights"),
    QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Shadows"),         QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Whites"),
    QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Blacks"),          QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Presence"),
    QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Vibrance"),        QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Saturation"),
    QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Tone curve"),      QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Colour mixer"),
    QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Hue"),             QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Luminance"),
    QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Red"),             QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Orange"),
    QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Yellow"),          QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Green"),
    QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Aqua"),            QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Blue"),
    QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Purple"),          QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Magenta"),
    QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Colour grading"),  QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Midtones"),
    QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Lightness"),       QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Blend"),
    QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Blending"),        QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Balance"),
    QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "LUT"),             QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Amount"),
};

QString key(const QString &effect, const QString &name)
{
    return effect + u'/' + name;
}

} // namespace

DevelopPanel::DevelopPanel(Studio *studio, CanvasView *canvas, QWidget *parent)
    : QWidget(parent)
    , m_studio(studio)
    , m_canvas(canvas)
{
    setObjectName(QStringLiteral("developPanel"));
    auto *v = new QVBoxLayout(this);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("developStatus"));
    m_status->setWordWrap(true);
    m_status->setProperty("role", QStringLiteral("muted"));
    v->addWidget(m_status);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_controls = new QWidget(scroll);
    new QVBoxLayout(m_controls);
    scroll->setWidget(m_controls);
    v->addWidget(scroll, 1);

    auto *buttons = new QHBoxLayout;
    m_before = new QPushButton(tr("Before"), this);
    m_before->setObjectName(QStringLiteral("developBefore"));
    m_before->setCheckable(true);
    m_before->setToolTip(tr("Show the layer as it was before the grading"));
    m_reset = new QPushButton(tr("Reset"), this);
    m_reset->setObjectName(QStringLiteral("developReset"));
    m_reset->setToolTip(tr("Every control back to the start, and the layer as it was"));
    m_bake = new QPushButton(tr("Bake"), this);
    m_bake->setObjectName(QStringLiteral("developBake"));
    m_bake->setToolTip(tr("Make the grading part of the layer's pixels, so you can paint on it"));
    buttons->addWidget(m_before);
    buttons->addStretch();
    buttons->addWidget(m_reset);
    buttons->addWidget(m_bake);
    v->addLayout(buttons);

    connect(m_before, &QPushButton::toggled, this, [this](bool on) {
        // Off again: the controls' preview while they differ from the layer, else the layer itself.
        if (on || differsFromLayer())
            renderPreview();
        else
            clearPreview();
    });
    connect(m_reset, &QPushButton::clicked, this, [this] {
        m_state = grading::State();
        for (auto it = m_effects.cbegin(); it != m_effects.cend(); ++it)
            m_state.values.insert(it.key(), it.value().defaults());
        showState();
        commit();
    });
    connect(m_bake, &QPushButton::clicked, this, [this] {
        if (m_doc && !m_committing)
            m_doc->bakeGrading(m_layer);
    });

    m_previewTimer.setSingleShot(true);
    m_previewTimer.setInterval(0);
    connect(&m_previewTimer, &QTimer::timeout, this, &DevelopPanel::renderPreview);
    connect(studio, &Studio::activeDocumentChanged, this, &DevelopPanel::setDocument);
    setDocument(studio->activeDocument());
}

DevelopPanel::~DevelopPanel() = default;

void DevelopPanel::ensureBuilt()
{
    if (m_built)
        return;
    m_built = true;
    gpu::Context &gpu = gpu::Context::instance();
    for (const QString &id : grading::effectOrder()) {
        QString error;
        const gpu::Effect effect = gpu::Effect::load(id, gpu, &error);
        if (effect.isNull()) {
            if (m_unavailable.isEmpty())
                m_unavailable = gpu.isAvailable() ? error : gpu.unavailableReason();
            continue;
        }
        m_effects.insert(id, effect);
    }
    if (!m_effects.contains(kDevelop)) {
        m_effects.clear();
        updateState();
        return;
    }
    m_unavailable.clear();
    auto *layout = static_cast<QVBoxLayout *>(m_controls->layout());
    for (const QString &id : grading::effectOrder()) {
        if (m_effects.contains(id))
            layout->addWidget(buildGroup(m_effects.value(id), id == kDevelop));
    }
    layout->addStretch();
    loadLayer();
}

// One effect: a heading that opens and closes it, then its controls.
QWidget *DevelopPanel::buildGroup(const gpu::Effect &effect, bool open)
{
    const QString id = effect.id();
    auto *group = new QWidget(m_controls);
    auto *v = new QVBoxLayout(group);
    v->setContentsMargins(0, 0, 0, 6);
    auto *head = new QToolButton(group);
    head->setObjectName(QStringLiteral("developGroup_%1").arg(id));
    head->setText(tr(effect.title().toUtf8().constData()));
    head->setCheckable(true);
    head->setChecked(open);
    head->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    head->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    head->setAutoRaise(true);
    QFont bold = head->font();
    bold.setBold(true);
    head->setFont(bold);
    v->addWidget(head);
    auto *body = new QWidget(group);
    auto *b = new QVBoxLayout(body);
    b->setContentsMargins(4, 0, 0, 0);
    body->setVisible(open);
    v->addWidget(body);
    connect(head, &QToolButton::toggled, body, [head, body](bool on) {
        body->setVisible(on);
        head->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
    });
    m_groups.insert(id, head);

    const QHash<QString, QString> inputs = effect.inputs();
    for (auto it = inputs.cbegin(); it != inputs.cend(); ++it) {
        if (it.value() == QLatin1String("curve")) {
            m_curveChannel = new QComboBox(body);
            m_curveChannel->setObjectName(QStringLiteral("developCurveChannel"));
            m_curveChannel->setAccessibleName(tr("Curve for"));
            m_curveChannel->addItem(tr("All three"), QStringLiteral("rgb"));
            m_curveChannel->addItem(tr("Red"), QStringLiteral("r"));
            m_curveChannel->addItem(tr("Green"), QStringLiteral("g"));
            m_curveChannel->addItem(tr("Blue"), QStringLiteral("b"));
            b->addWidget(m_curveChannel);
            m_curve = new CurveEditor(body);
            m_curve->setObjectName(QStringLiteral("developCurve"));
            m_curve->setAccessibleName(tr("Tone curve"));
            b->addWidget(m_curve);
            connect(m_curveChannel, &QComboBox::currentIndexChanged, this, [this] {
                m_curve->setText(m_state.curves.value(m_curveChannel->currentData().toString(), grading::straightCurve()));
            });
            connect(m_curve, &CurveEditor::textChanged, this, [this](const QString &text) {
                m_state.curves.insert(m_curveChannel->currentData().toString(), text);
                changed();
            });
            connect(m_curve, &CurveEditor::editingFinished, this, &DevelopPanel::commit);
        } else if (it.value() == QLatin1String("lut")) {
            auto *row = new QHBoxLayout;
            m_lut = new QComboBox(body);
            m_lut->setObjectName(QStringLiteral("developLut"));
            m_lut->setAccessibleName(tr("LUT"));
            auto *add = new QPushButton(tr("Add…"), body);
            add->setObjectName(QStringLiteral("developAddLut"));
            add->setToolTip(tr("Add a LUT (.cube) to the ones offered here"));
            row->addWidget(m_lut, 1);
            row->addWidget(add);
            b->addLayout(row);
            fillLuts();
            connect(m_lut, &QComboBox::activated, this, [this] {
                m_state.lut = m_lut->currentData().toString();
                changed();
                commit();
            });
            connect(add, &QPushButton::clicked, this, [this] {
                const QString file = QFileDialog::getOpenFileName(this, tr("Add a LUT"), QString(), tr("LUTs (*.cube)"));
                if (file.isEmpty())
                    return;
                QString error;
                const QString name = grading::addLut(file, &error);
                if (name.isEmpty()) {
                    m_studio->notify(error, QStringLiteral("warning"));
                    return;
                }
                setLut(name);
                commit();
            });
        }
    }

    QString section;
    QList<SliderField *> fields;
    for (const gpu::EffectParam &p : effect.params()) {
        if (!p.shown())
            continue;
        if (p.section != section) {
            section = p.section;
            auto *heading = new QLabel(tr(section.toUtf8().constData()), body);
            heading->setProperty("role", QStringLiteral("muted"));
            b->addSpacing(fields.isEmpty() ? 0 : 4);
            b->addWidget(heading);
        }
        const QString name = p.name;
        m_ownerOf.insert(name, id);
        if (p.control == QLatin1String("wheel") && p.components == 2) {
            auto *wheel = new ColorWheel(body);
            wheel->setObjectName(QStringLiteral("%1_%2").arg(id, name));
            wheel->setAccessibleName(tr(p.label.toUtf8().constData()));
            b->addWidget(wheel, 0, Qt::AlignHCenter);
            m_wheels.insert(key(id, name), wheel);
            connect(wheel, &ColorWheel::valueChanged, this, [this, id, name](double hue, double saturation) {
                m_state.values[id].insert(name, QVariantList{hue, saturation});
                changed();
            });
            connect(wheel, &ColorWheel::editingFinished, this, &DevelopPanel::commit);
            continue;
        }
        if (p.components != 1)
            continue;
        const double def = p.defaults.value(0);
        const int decimals = p.maximum - p.minimum <= 20 ? 2 : 0;
        auto *field = new SliderField(tr(p.label.toUtf8().constData()), p.minimum, p.maximum, def, decimals, p.unit, body);
        field->setObjectName(QStringLiteral("%1_%2").arg(id, name));
        field->setResetValue(def);
        b->addWidget(field);
        fields << field;
        m_fields.insert(key(id, name), field);
        connect(field, &SliderField::valueChanged, this, [this, id, name](double value) {
            m_state.values[id].insert(name, value);
            changed();
        });
        connect(field, &SliderField::editingFinished, this, &DevelopPanel::commit);
    }
    SliderField::alignLabels(fields);
    return group;
}

void DevelopPanel::fillLuts()
{
    if (!m_lut)
        return;
    QSignalBlocker block(m_lut);
    m_lut->clear();
    m_lut->addItem(tr("None"), QString());
    for (const QString &name : grading::availableLuts())
        m_lut->addItem(QFileInfo(name).completeBaseName(), name);
    // One the layer uses that is not in the folder (another computer, a deleted file) still shows.
    if (!m_state.lut.isEmpty() && m_lut->findData(m_state.lut) < 0)
        m_lut->addItem(tr("%1 (missing)").arg(QFileInfo(m_state.lut).completeBaseName()), m_state.lut);
    m_lut->setCurrentIndex(std::max(0, m_lut->findData(m_state.lut)));
}

void DevelopPanel::setDocument(doc::Document *doc)
{
    if (m_doc == doc)
        return;
    if (m_doc)
        disconnect(m_doc, nullptr, this, nullptr);
    clearPreview();
    m_doc = doc;
    m_scaled.clear();
    if (doc) {
        connect(doc, &doc::Document::activeLayerChanged, this, &DevelopPanel::loadLayer);
        connect(doc, &doc::Document::layersChanged, this, &DevelopPanel::loadLayer);
        connect(doc, &doc::Document::layerPropsChanged, this, &DevelopPanel::updateState);
        connect(doc, &doc::Document::gradingChanged, this, [this](const QUuid &id) {
            if (!m_applying && id == m_layer)
                loadLayer();
            updateState();
        });
        // Other layers painted on: the preview over them shows it.
        connect(doc, &doc::Document::pixelsChanged, this, [this] {
            if (m_previewing)
                schedulePreview();
        });
    }
    loadLayer();
}

void DevelopPanel::loadLayer()
{
    const doc::LayerPtr layer = m_doc ? m_doc->activeLayer() : nullptr;
    m_layer = layer ? layer->id : QUuid();
    if (m_effects.isEmpty()) {
        updateState();
        return;
    }
    grading::State state = layer && layer->graded() ? grading::fromJson(layer->grading()) : grading::State();
    for (auto it = m_effects.cbegin(); it != m_effects.cend(); ++it) {
        QString error;
        const QVariantMap values = it.value().resolve(state.values.value(it.key()), &error);
        state.values.insert(it.key(), error.isEmpty() ? values : it.value().defaults());
    }
    for (const QString &channel : grading::curveChannels()) {
        if (!state.curves.contains(channel))
            state.curves.insert(channel, grading::straightCurve());
    }
    m_state = state;
    showState();
    {
        QSignalBlocker block(m_before);
        m_before->setChecked(false);
    }
    clearPreview();
    updateState();
}

// The controls as the state says, without them saying they moved.
void DevelopPanel::showState()
{
    for (auto it = m_fields.cbegin(); it != m_fields.cend(); ++it) {
        const QString effect = it.key().section(u'/', 0, 0);
        const QString name = it.key().section(u'/', 1);
        QSignalBlocker block(it.value());
        it.value()->setValue(m_state.values.value(effect).value(name).toDouble());
    }
    for (auto it = m_wheels.cbegin(); it != m_wheels.cend(); ++it) {
        const QVariantList hs = m_state.values.value(it.key().section(u'/', 0, 0)).value(it.key().section(u'/', 1)).toList();
        it.value()->setValue(hs.value(0).toDouble(), hs.value(1).toDouble());
    }
    if (m_curve)
        m_curve->setText(m_state.curves.value(m_curveChannel->currentData().toString(), grading::straightCurve()));
    fillLuts();
}

void DevelopPanel::updateState()
{
    const doc::LayerPtr layer = m_doc ? m_doc->findLayer(m_layer) : nullptr;
    QString status;
    bool usable = false;
    if (!m_unavailable.isEmpty())
        status = tr("Grading needs the graphics card: %1").arg(m_unavailable);
    else if (!layer)
        status = tr("Open a picture to grade it.");
    else if (layer->props.locked)
        status = tr("The layer is locked.");
    else
        usable = true;
    if (usable && m_committing)
        status = tr("Grading at full size…");
    else if (usable && layer->graded())
        status = tr("Graded: change it any time. Bake it to paint on the layer.");
    m_status->setText(status);
    m_status->setVisible(!status.isEmpty());
    m_controls->setEnabled(usable);
    const bool graded = layer && layer->graded();
    m_reset->setEnabled(usable && (graded || !grading::isNeutral(m_state, m_effects)));
    m_bake->setEnabled(usable && graded && !m_committing);
    m_before->setEnabled(usable && (graded || m_previewing));
}

void DevelopPanel::changed()
{
    schedulePreview();
    updateState();
}

QVariantMap DevelopPanel::values() const
{
    QVariantMap out;
    for (auto it = m_state.values.cbegin(); it != m_state.values.cend(); ++it) {
        for (auto v = it.value().cbegin(); v != it.value().cend(); ++v)
            out.insert(v.key(), v.value());
    }
    return out;
}

void DevelopPanel::setValue(const QString &name, double value)
{
    ensureBuilt();
    const QString effect = m_ownerOf.value(name);
    SliderField *field = m_fields.value(key(effect, name));
    if (!field)
        return;
    // SliderField::setValue says nothing; what a moved slider does is done here.
    field->setValue(value);
    m_state.values[effect].insert(name, field->value());
    changed();
}

void DevelopPanel::setWheel(const QString &name, double hue, double saturation)
{
    ensureBuilt();
    const QString effect = m_ownerOf.value(name);
    ColorWheel *wheel = m_wheels.value(key(effect, name));
    if (!wheel)
        return;
    wheel->setValue(hue, saturation);
    m_state.values[effect].insert(name, QVariantList{wheel->hue(), wheel->saturation()});
    changed();
}

void DevelopPanel::setCurve(const QString &channel, const QString &points)
{
    ensureBuilt();
    if (!m_curve)
        return;
    m_state.curves.insert(channel, points);
    {
        QSignalBlocker block(m_curveChannel);
        m_curveChannel->setCurrentIndex(std::max(0, m_curveChannel->findData(channel)));
    }
    m_curve->setText(points);
    changed();
}

void DevelopPanel::setLut(const QString &name)
{
    ensureBuilt();
    m_state.lut = name;
    fillLuts();
    changed();
}

void DevelopPanel::expandAll(bool open)
{
    ensureBuilt();
    for (QToolButton *head : std::as_const(m_groups))
        head->setChecked(open);
}

void DevelopPanel::showBefore(bool before)
{
    ensureBuilt();
    m_before->setChecked(before);
}

void DevelopPanel::schedulePreview()
{
    if (m_shown)
        m_previewTimer.start();
}

QImage DevelopPanel::scaledLayer(const doc::Layer &layer, const QImage &pixels, const QSize &size)
{
    const QString cacheKey = layer.id.toString() + (&pixels == &layer.original ? QStringLiteral("/original") : QString());
    const Scaled &cached = m_scaled[cacheKey];
    if (cached.revision == layer.revision && cached.key == pixels.cacheKey() && cached.image.size() == size)
        return cached.image;
    const QImage scaled = img::normalized(img::scaled(pixels, size, true));
    m_scaled.insert(cacheKey, Scaled{layer.revision, pixels.cacheKey(), scaled});
    return scaled;
}

// The picture at screen size with the active layer graded as the controls say (or as it was, for
// Before), shown on the canvas instead of the document.
void DevelopPanel::renderPreview()
{
    const doc::LayerPtr layer = m_doc ? m_doc->findLayer(m_layer) : nullptr;
    if (!layer || m_effects.isEmpty() || !m_shown) {
        clearPreview();
        return;
    }
    const QSize docSize = m_doc->size();
    const qreal dpr = m_canvas->devicePixelRatioF();
    const int screen = std::max(64, int(std::max(m_canvas->width(), m_canvas->height()) * dpr));
    const double scale = std::min(1.0, double(screen) / std::max(docSize.width(), docSize.height()));
    const QSize size = QSize(std::max(1, int(std::lround(docSize.width() * scale))), std::max(1, int(std::lround(docSize.height() * scale))));
    const QImage &source = layer->graded() ? layer->original : layer->image;
    const QImage small = scaledLayer(*layer, source, size);
    QImage shown = small;
    if (!m_before->isChecked()) {
        QString error;
        const QList<gpu::EffectStep> steps = grading::steps(m_state, m_effects, &error);
        if (!error.isEmpty() || !m_previewChain.setSteps(steps, &error)) {
            m_status->setText(error);
            m_status->show();
            return;
        }
        const QImage graded = m_previewChain.render(small);
        if (graded.isNull()) {
            m_status->setText(tr("Grading did not work: %1").arg(m_previewChain.error()));
            m_status->show();
            return;
        }
        shown = graded.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
    doc::DocumentSnapshot snapshot;
    for (int i = 0; i < m_doc->layerCount(); ++i) {
        const doc::LayerPtr l = m_doc->layerAt(i);
        snapshot.append({l->id == m_layer ? shown : scaledLayer(*l, l->image, size), l->props});
    }
    m_canvas->setPreview(CanvasView::Preview::Filter, doc::flatten(snapshot, size));
    m_previewing = true;
    updateState();
}

void DevelopPanel::clearPreview()
{
    m_previewTimer.stop();
    if (!m_previewing)
        return;
    m_previewing = false;
    m_canvas->setPreview(CanvasView::Preview::Filter, {});
}

bool DevelopPanel::differsFromLayer() const
{
    const doc::LayerPtr layer = m_doc ? m_doc->findLayer(m_layer) : nullptr;
    if (!layer || m_effects.isEmpty())
        return false;
    return grading::toJson(m_state, m_effects) != layer->grading();
}

void DevelopPanel::commit()
{
    ensureBuilt();
    const doc::LayerPtr layer = m_doc ? m_doc->findLayer(m_layer) : nullptr;
    if (m_effects.isEmpty() || !layer || layer->props.locked)
        return;
    if (m_committing) {
        m_commitAgain = true;
        return;
    }
    const QJsonObject wanted = grading::toJson(m_state, m_effects);
    if (wanted == layer->grading()) {
        if (!m_before->isChecked())
            clearPreview();
        updateState();
        return;
    }
    if (wanted.isEmpty()) {
        // Every control at the start: the layer as it was.
        m_applying = true;
        m_doc->setGrading(m_layer, doc::Grading{QImage(), QJsonObject(), layer->original}, tr("Reset grading"));
        m_applying = false;
        clearPreview();
        updateState();
        emit committed(true, QString());
        return;
    }
    const QImage source = layer->graded() ? layer->original : layer->image;
    QString error;
    const QList<gpu::EffectStep> steps = grading::steps(m_state, m_effects, &error);
    if (!error.isEmpty() || !m_fullChain.setSteps(steps, &error)) {
        m_status->setText(error);
        m_status->show();
        emit committed(false, error);
        return;
    }
    m_committing = true;
    updateState();
    const QUuid id = m_layer;
    const quint64 revision = layer->revision;
    const QPointer<doc::Document> doc = m_doc;
    const grading::State state = m_state;
    m_fullChain.renderLater(source).then(this, [this, id, revision, doc, state, wanted, source](const QImage &out) {
        m_committing = false;
        const doc::LayerPtr now = doc ? doc->findLayer(id) : nullptr;
        if (out.isNull()) {
            const QString message = tr("Grading did not work: %1").arg(m_fullChain.error());
            m_status->setText(message);
            emit committed(false, message);
        } else if (!now || now->revision != revision) {
            emit committed(false, tr("The layer changed while it was being graded, so nothing was changed."));
        } else {
            m_applying = true;
            doc->setGrading(id, doc::Grading{source, wanted, out.convertToFormat(QImage::Format_ARGB32_Premultiplied)}, tr("Develop"));
            m_applying = false;
            emit committed(true, QString());
        }
        if (m_commitAgain) {
            m_commitAgain = false;
            commit();
            return;
        }
        // The canvas shows the layer itself again, unless the controls moved on.
        if (state == m_state && !m_before->isChecked())
            clearPreview();
        updateState();
    });
}

void DevelopPanel::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    m_shown = true;
    ensureBuilt();
    loadLayer();
}

void DevelopPanel::hideEvent(QHideEvent *e)
{
    QWidget::hideEvent(e);
    m_shown = false;
    {
        QSignalBlocker block(m_before);
        m_before->setChecked(false);
    }
    clearPreview();
}

} // namespace lumen::ui
