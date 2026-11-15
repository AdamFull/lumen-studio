#include "ui/widgets/DevelopPanel.h"

#include "core/ImageOps.h"
#include "doc/Document.h"
#include "ui/Studio.h"
#include "ui/canvas/CanvasView.h"
#include "ui/widgets/Controls.h"

#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <cmath>

namespace lumen::ui {

namespace {

const QString kEffect = QStringLiteral("develop");

// The Develop effect's words, which its shader gives in English.
[[maybe_unused]] const char *const kEffectWords[] = {
    QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "White balance"), QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Temperature"),
    QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Tint"),          QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Tone"),
    QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Exposure"),      QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Contrast"),
    QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Highlights"),    QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Shadows"),
    QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Whites"),        QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Blacks"),
    QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Presence"),      QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Vibrance"),
    QT_TRANSLATE_NOOP("lumen::ui::DevelopPanel", "Saturation"),
};

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
    m_sliders = new QWidget(scroll);
    new QVBoxLayout(m_sliders);
    scroll->setWidget(m_sliders);
    v->addWidget(scroll, 1);

    auto *buttons = new QHBoxLayout;
    m_before = new QPushButton(tr("Before"), this);
    m_before->setObjectName(QStringLiteral("developBefore"));
    m_before->setCheckable(true);
    m_before->setToolTip(tr("Show the layer as it was before the grading"));
    m_reset = new QPushButton(tr("Reset"), this);
    m_reset->setObjectName(QStringLiteral("developReset"));
    m_reset->setToolTip(tr("Every slider back to 0, and the layer as it was"));
    m_bake = new QPushButton(tr("Bake"), this);
    m_bake->setObjectName(QStringLiteral("developBake"));
    m_bake->setToolTip(tr("Make the grading part of the layer's pixels, so you can paint on it"));
    buttons->addWidget(m_before);
    buttons->addStretch();
    buttons->addWidget(m_reset);
    buttons->addWidget(m_bake);
    v->addLayout(buttons);

    connect(m_before, &QPushButton::toggled, this, [this](bool on) {
        // Off again: the sliders' preview while they differ from the layer, else the layer itself.
        if (on || differsFromLayer())
            renderPreview();
        else
            clearPreview();
    });
    connect(m_reset, &QPushButton::clicked, this, [this] {
        const QVariantMap defaults = m_effect.defaults();
        for (auto it = defaults.cbegin(); it != defaults.cend(); ++it) {
            if (SliderField *f = m_fields.value(it.key())) {
                QSignalBlocker block(f);
                f->setValue(it.value().toDouble());
            }
        }
        m_values = defaults;
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
    QString error;
    m_effect = gpu::Effect::load(kEffect, gpu, &error);
    if (m_effect.isNull()) {
        m_unavailable = gpu.isAvailable() ? error : gpu.unavailableReason();
        updateState();
        return;
    }
    auto *layout = static_cast<QVBoxLayout *>(m_sliders->layout());
    QString section;
    QList<SliderField *> fields;
    for (const gpu::EffectParam &p : m_effect.params()) {
        if (!p.shown() || p.components != 1)
            continue;
        if (p.section != section) {
            section = p.section;
            auto *heading = new QLabel(tr(section.toUtf8().constData()), m_sliders);
            QFont f = heading->font();
            f.setBold(true);
            heading->setFont(f);
            layout->addSpacing(fields.isEmpty() ? 0 : 6);
            layout->addWidget(heading);
        }
        const double def = p.defaults.value(0);
        const int decimals = p.maximum - p.minimum <= 20 ? 2 : 0;
        auto *field = new SliderField(tr(p.label.toUtf8().constData()), p.minimum, p.maximum, def, decimals, p.unit, m_sliders);
        field->setObjectName(QStringLiteral("develop_%1").arg(p.name));
        field->setResetValue(def);
        layout->addWidget(field);
        fields << field;
        m_fields.insert(p.name, field);
        const QString name = p.name;
        connect(field, &SliderField::valueChanged, this, [this, name](double value) {
            m_values.insert(name, value);
            schedulePreview();
            updateState();
        });
        connect(field, &SliderField::editingFinished, this, &DevelopPanel::commit);
    }
    layout->addStretch();
    SliderField::alignLabels(fields);
    loadLayer();
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
    if (m_effect.isNull()) {
        updateState();
        return;
    }
    QVariantMap values = m_effect.defaults();
    if (layer && layer->graded()) {
        for (const QJsonValue &step : layer->grading().value(QStringLiteral("steps")).toArray()) {
            if (step[QStringLiteral("effect")].toString() != kEffect)
                continue;
            QString error;
            const QVariantMap stored = m_effect.resolve(step[QStringLiteral("values")].toObject().toVariantMap(), &error);
            if (error.isEmpty())
                values = stored;
        }
    }
    m_values = values;
    for (auto it = m_fields.cbegin(); it != m_fields.cend(); ++it) {
        QSignalBlocker block(it.value());
        it.value()->setValue(values.value(it.key()).toDouble());
    }
    {
        QSignalBlocker block(m_before);
        m_before->setChecked(false);
    }
    clearPreview();
    updateState();
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
    m_sliders->setEnabled(usable);
    const bool graded = layer && layer->graded();
    m_reset->setEnabled(usable && (graded || !isNeutral(m_values)));
    m_bake->setEnabled(usable && graded && !m_committing);
    m_before->setEnabled(usable && (graded || m_previewing));
}

void DevelopPanel::setValue(const QString &name, double value)
{
    ensureBuilt();
    SliderField *field = m_fields.value(name);
    if (!field)
        return;
    // SliderField::setValue says nothing; what a moved slider does is done here.
    field->setValue(value);
    m_values.insert(name, field->value());
    schedulePreview();
    updateState();
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
    const QString key = layer.id.toString() + (&pixels == &layer.original ? QStringLiteral("/original") : QString());
    const Scaled &cached = m_scaled[key];
    if (cached.revision == layer.revision && cached.key == pixels.cacheKey() && cached.image.size() == size)
        return cached.image;
    const QImage scaled = img::normalized(img::scaled(pixels, size, true));
    m_scaled.insert(key, Scaled{layer.revision, pixels.cacheKey(), scaled});
    return scaled;
}

// The picture at screen size with the active layer graded as the sliders say (or as it was, for
// Before), shown on the canvas instead of the document.
void DevelopPanel::renderPreview()
{
    const doc::LayerPtr layer = m_doc ? m_doc->findLayer(m_layer) : nullptr;
    if (!layer || m_effect.isNull() || !m_shown) {
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
        if (!m_previewChain.setSteps({{kEffect, m_values}}, &error)) {
            m_status->setText(error);
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

QJsonObject DevelopPanel::gradingFor(const QVariantMap &values) const
{
    if (isNeutral(values))
        return {};
    // Only what moved: a later version's new sliders start at their defaults.
    const QVariantMap defaults = m_effect.defaults();
    QJsonObject changed;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (std::abs(it.value().toDouble() - defaults.value(it.key()).toDouble()) > 1e-9)
            changed.insert(it.key(), it.value().toDouble());
    }
    return QJsonObject{{QStringLiteral("steps"), QJsonArray{QJsonObject{{QStringLiteral("effect"), kEffect}, {QStringLiteral("values"), changed}}}}};
}

bool DevelopPanel::differsFromLayer() const
{
    const doc::LayerPtr layer = m_doc ? m_doc->findLayer(m_layer) : nullptr;
    if (!layer || m_effect.isNull())
        return false;
    return gradingFor(m_values) != layer->grading();
}

bool DevelopPanel::isNeutral(const QVariantMap &values) const
{
    const QVariantMap defaults = m_effect.defaults();
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (std::abs(it.value().toDouble() - defaults.value(it.key()).toDouble()) > 1e-9)
            return false;
    }
    return true;
}

void DevelopPanel::commit()
{
    ensureBuilt();
    const doc::LayerPtr layer = m_doc ? m_doc->findLayer(m_layer) : nullptr;
    if (m_effect.isNull() || !layer || layer->props.locked)
        return;
    if (m_committing) {
        m_commitAgain = true;
        return;
    }
    const QJsonObject wanted = gradingFor(m_values);
    if (wanted == layer->grading() && (layer->graded() || wanted.isEmpty())) {
        if (!m_before->isChecked())
            clearPreview();
        updateState();
        return;
    }
    if (wanted.isEmpty()) {
        // Every slider at 0: the layer as it was.
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
    if (!m_fullChain.setSteps({{kEffect, m_values}}, &error)) {
        emit committed(false, error);
        return;
    }
    m_committing = true;
    updateState();
    const QUuid id = m_layer;
    const quint64 revision = layer->revision;
    const QPointer<doc::Document> doc = m_doc;
    const QVariantMap values = m_values;
    m_fullChain.renderLater(source).then(this, [this, id, revision, doc, values, source](const QImage &out) {
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
            doc->setGrading(id, doc::Grading{source, gradingFor(values), out.convertToFormat(QImage::Format_ARGB32_Premultiplied)}, tr("Develop"));
            m_applying = false;
            emit committed(true, QString());
        }
        if (m_commitAgain) {
            m_commitAgain = false;
            commit();
            return;
        }
        // The canvas shows the layer itself again, unless the sliders moved on.
        if (values == m_values && !m_before->isChecked())
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
