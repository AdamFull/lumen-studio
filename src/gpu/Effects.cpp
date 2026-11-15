#include "gpu/Effects.h"

#include "core/Async.h"

#include <QDir>
#include <QFile>

#include <cmath>

namespace lumen::gpu {

namespace {

const QString kSource = QStringLiteral("source");
const QString kResult = QStringLiteral("result");

} // namespace

QStringList Effect::builtIn()
{
    QStringList out;
    for (const QString &file : QDir(QStringLiteral(":/gpu")).entryList({QStringLiteral("*.passes")}, QDir::Files, QDir::Name))
        out << file.chopped(int(qstrlen(".passes")));
    return out;
}

Effect Effect::load(const QString &id, Context &context, QString *error)
{
    auto failed = [error](const QString &message) {
        if (error)
            *error = message;
        return Effect();
    };
    QFile list(QStringLiteral(":/gpu/%1.passes").arg(id));
    if (!list.open(QIODevice::ReadOnly))
        return failed(QStringLiteral("no effect %1").arg(id));
    const QStringList entries = QString::fromUtf8(list.readAll()).split(u'\n', Qt::SkipEmptyParts);
    if (entries.isEmpty())
        return failed(QStringLiteral("the effect %1 has no passes").arg(id));

    Effect e;
    e.m_id = id;
    for (const QString &entry : entries) {
        QString why;
        const Kernel k = context.kernel(id + u'.' + entry.trimmed(), &why);
        if (k.isNull())
            return failed(QStringLiteral("%1: %2").arg(id, why));
        // Effects work in half floats; the 8-bit picture is the chain's business.
        for (auto it = k.info().formats.cbegin(); it != k.info().formats.cend(); ++it) {
            if (it.value() != QLatin1String("rgba16f"))
                return failed(QStringLiteral("%1: %2 of %3 is %4; effects take rgba16f").arg(id, it.key(), entry, it.value()));
        }
        for (auto it = k.info().inputs.cbegin(); it != k.info().inputs.cend(); ++it)
            e.m_inputs.insert(it.key(), it.value());
        for (auto it = k.info().textureAttributes.cbegin(); it != k.info().textureAttributes.cend(); ++it) {
            const QVariantList scale = it.value().value(QStringLiteral("Scale"));
            const QVariantList fixed = it.value().value(QStringLiteral("Size"));
            const QVariantList fit = it.value().value(QStringLiteral("Fit"));
            if (!fit.isEmpty() && fit.first().toInt() < 1)
                return failed(QStringLiteral("%1: %2 fits in %3 pixels; Fit takes 1 or more").arg(id, it.key(), fit.first().toString()));
            if (!scale.isEmpty() && !(scale.first().toDouble() >= 1.0))
                return failed(QStringLiteral("%1: %2 is scaled by %3; Scale takes 1 or more").arg(id, it.key(), scale.first().toString()));
            if (!fixed.isEmpty() && (fixed.size() != 2 || fixed.at(0).toInt() < 1 || fixed.at(1).toInt() < 1))
                return failed(QStringLiteral("%1: the Size of %2 takes a width and a height of 1 or more").arg(id, it.key()));
            if (!scale.isEmpty() || !fixed.isEmpty() || !fit.isEmpty())
                e.m_textures.insert(it.key(), it.value());
        }
        // A pass over a texture: one of the effect's own, which it writes or reads.
        if (const QVariantList over = k.info().attributes.value(QStringLiteral("Over")); !over.isEmpty()) {
            const QString name = over.first().toString();
            if (name != kResult && (!k.info().bindings.contains(name) || name == kSource || k.info().inputs.contains(name)))
                return failed(QStringLiteral("%1: %2 runs over %3, which is not one of its own textures").arg(id, entry, name));
        }
        e.m_passes << k;
    }
    if (e.m_passes.first().info().params.size() != e.m_passes.last().info().params.size())
        return failed(QStringLiteral("%1: its passes have different parameters").arg(id));

    const KernelInterface &first = e.m_passes.first().info();
    const QVariantList effect = first.attributes.value(QStringLiteral("Effect"));
    e.m_title = effect.value(0, id).toString();
    e.m_group = effect.value(1).toString();
    QString section;
    for (const ParamField &f : first.params) {
        EffectParam p;
        p.name = f.name;
        p.scalar = f.scalar;
        p.components = f.components;
        p.label = f.attributes.value(QStringLiteral("Label")).value(0).toString();
        p.unit = f.attributes.value(QStringLiteral("Unit")).value(0).toString();
        p.control = f.attributes.value(QStringLiteral("Control")).value(0).toString();
        if (f.attributes.contains(QStringLiteral("Section")))
            section = f.attributes.value(QStringLiteral("Section")).value(0).toString();
        p.section = section;
        if (const QVariantList range = f.attributes.value(QStringLiteral("Range")); range.size() == 2) {
            p.minimum = range.at(0).toDouble();
            p.maximum = range.at(1).toDouble();
            p.hasRange = p.maximum >= p.minimum;
        }
        const double fallback = p.hasRange ? std::clamp(0.0, p.minimum, p.maximum) : 0.0;
        const QVariantList given = f.attributes.value(QStringLiteral("Default"));
        for (int i = 0; i < p.components; ++i)
            p.defaults << (given.isEmpty() ? fallback : given.value(std::min<int>(i, int(given.size()) - 1)).toDouble());
        e.m_params << p;
    }
    return e;
}

QSize Effect::textureSize(const QString &name, const QSize &picture) const
{
    const QHash<QString, QVariantList> rules = m_textures.value(name);
    if (const QVariantList fixed = rules.value(QStringLiteral("Size")); fixed.size() == 2)
        return QSize(fixed.at(0).toInt(), fixed.at(1).toInt());
    if (const QVariantList fit = rules.value(QStringLiteral("Fit")); !fit.isEmpty()) {
        // The long side at n (never larger than the picture), the picture's shape kept.
        const int longSide = std::max(picture.width(), picture.height());
        const double n = std::min<double>(fit.first().toInt(), longSide) / std::max(1, longSide);
        return QSize(std::max(1, int(std::lround(picture.width() * n))), std::max(1, int(std::lround(picture.height() * n))));
    }
    if (const QVariantList scale = rules.value(QStringLiteral("Scale")); !scale.isEmpty()) {
        const double n = std::max(1.0, scale.first().toDouble());
        return QSize(std::max(1, int(std::ceil(picture.width() / n))), std::max(1, int(std::ceil(picture.height() / n))));
    }
    return picture;
}

QSize Effect::passSize(int pass, const QSize &picture) const
{
    const QVariantList over = m_passes.value(pass).info().attributes.value(QStringLiteral("Over"));
    return over.isEmpty() ? picture : textureSize(over.first().toString(), picture);
}

QVariantMap Effect::defaults() const
{
    QVariantMap out;
    for (const EffectParam &p : m_params) {
        if (!p.shown())
            continue;
        if (p.components == 1) {
            out.insert(p.name, p.defaults.value(0));
        } else {
            QVariantList list;
            for (double d : p.defaults)
                list << d;
            out.insert(p.name, list);
        }
    }
    return out;
}

QVariantMap Effect::resolve(const QVariantMap &values, QString *error) const
{
    QVariantMap out = defaults();
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        const EffectParam *param = nullptr;
        for (const EffectParam &p : m_params) {
            if (p.name == it.key() && p.shown())
                param = &p;
        }
        if (!param) {
            if (error)
                *error = QStringLiteral("%1 has no setting %2").arg(m_title, it.key());
            return {};
        }
        QVariantList list = it.value().typeId() == QMetaType::QVariantList ? it.value().toList() : QVariantList{it.value()};
        if (list.size() != param->components) {
            if (error)
                *error = QStringLiteral("%1 of %2 takes %3 value(s)").arg(it.key(), m_title).arg(param->components);
            return {};
        }
        for (int i = 0; i < list.size(); ++i) {
            double d = list[i].toDouble();
            if (!std::isfinite(d))
                d = param->defaults.value(i);
            if (param->control == QLatin1String("wheel")) {
                // A hue goes round; a saturation is a percentage.
                d = i == 0 ? std::fmod(std::fmod(d, 360.0) + 360.0, 360.0) : std::clamp(d, 0.0, 100.0);
            } else if (param->hasRange) {
                d = std::clamp(d, param->minimum, param->maximum);
            }
            if (param->scalar != QLatin1String("float32"))
                d = std::round(d);
            list[i] = d;
        }
        out.insert(it.key(), param->components == 1 ? list.first() : QVariant(list));
    }
    return out;
}

// ---- Chain -----------------------------------------------------------------------------------------

Chain::Chain(Context &context)
    : m_context(context)
{
}

Chain::~Chain()
{
    // A render on a worker thread holds the lock until it is done.
    QMutexLocker lock(&m_lock);
}

bool Chain::setSteps(const QList<EffectStep> &steps, QString *error)
{
    QList<Prepared> prepared;
    for (const EffectStep &step : steps) {
        QString why;
        const Effect effect = Effect::load(step.effect, m_context, &why);
        if (effect.isNull()) {
            if (error)
                *error = why;
            return false;
        }
        const QVariantMap values = effect.resolve(step.values, &why);
        if (!why.isEmpty()) {
            if (error)
                *error = why;
            return false;
        }
        for (auto it = effect.inputs().cbegin(); it != effect.inputs().cend(); ++it) {
            if (step.inputs.value(it.key()).isNull()) {
                if (error)
                    *error = QStringLiteral("%1 needs its %2").arg(effect.title(), it.value());
                return false;
            }
        }
        if (step.enabled)
            prepared << Prepared{effect, values, step.inputs};
    }
    QMutexLocker lock(&m_lock);
    m_steps = steps;
    m_prepared = prepared;
    return true;
}

QList<EffectStep> Chain::steps() const
{
    QMutexLocker lock(&m_lock);
    return m_steps;
}

QString Chain::error() const
{
    QMutexLocker lock(&m_lock);
    return m_error;
}

bool Chain::reusedPicture() const
{
    QMutexLocker lock(&m_lock);
    return m_reused;
}

double Chain::gpuMilliseconds() const
{
    QMutexLocker lock(&m_lock);
    return m_gpuMs;
}

QImage Chain::render(const QImage &picture)
{
    QMutexLocker lock(&m_lock);
    m_error.clear();
    auto failed = [this](const QString &message) {
        m_error = message;
        return QImage();
    };
    if (picture.isNull())
        return failed(QStringLiteral("no picture"));
    if (!m_context.isAvailable())
        return failed(m_context.unavailableReason());
    QString why;
    const Kernel decode = m_context.kernel(QStringLiteral("color.decode"), &why);
    const Kernel encode = m_context.kernel(QStringLiteral("color.encode"), &why);
    if (decode.isNull() || encode.isNull())
        return failed(why);

    const QSize size = picture.size();
    auto texture = [&](Texture &t, Format format, QSize wanted = QSize()) {
        if (!wanted.isValid())
            wanted = size;
        if (t.isNull() || t.size() != wanted)
            t = m_context.texture(wanted, format, &why);
        return !t.isNull();
    };
    Run run(m_context);
    // A picture that is already up (the same image data and size) is not sent again.
    const qint64 key = picture.cacheKey();
    m_reused = key == m_pictureKey && size == m_size && !m_base.isNull();
    if (!m_reused) {
        m_pictureKey = 0;
        m_size = size;
        m_scratch.clear();
        const Texture input = run.upload(picture);
        if (!texture(m_base, Format::RGBA16F))
            return failed(why);
        run.dispatch(decode, {{QStringLiteral("picture"), input}, {QStringLiteral("light"), m_base}}, {}, size);
    }

    Texture current = m_base;
    for (const Prepared &step : std::as_const(m_prepared)) {
        // Never the base: it stays the picture for the next run.
        Texture &target = current == m_ping ? m_pong : m_ping;
        if (!texture(target, Format::RGBA16F))
            return failed(why);
        const QList<Kernel> passes = step.effect.passes();
        for (int i = 0; i < passes.size(); ++i) {
            const Kernel &pass = passes.at(i);
            QHash<QString, Texture> bound{{kSource, current}, {kResult, target}};
            for (const QString &name : pass.info().bindings.keys()) {
                if (name == kSource || name == kResult)
                    continue;
                if (step.effect.inputs().contains(name)) {
                    // Sent once; again when it is another picture.
                    const QImage picture = step.inputs.value(name);
                    Input &input = m_inputs[step.effect.id() + u'/' + name];
                    if (input.texture.isNull() || input.key != picture.cacheKey()) {
                        input.texture = run.upload(picture);
                        input.key = picture.cacheKey();
                    }
                    bound.insert(name, input.texture);
                    continue;
                }
                Texture &scratch = m_scratch[step.effect.id() + u'/' + name];
                if (!texture(scratch, Format::RGBA16F, step.effect.textureSize(name, size)))
                    return failed(why);
                bound.insert(name, scratch);
            }
            // "size" is what the pass runs over: the picture, or a texture of the effect's.
            run.dispatch(pass, bound, step.values, step.effect.passSize(i, size));
        }
        current = target;
    }
    if (!texture(m_output, Format::RGBA8))
        return failed(why);
    run.dispatch(encode, {{QStringLiteral("picture"), m_output}, {QStringLiteral("light"), current}},
                 {{QStringLiteral("dither"), 1.0}, {QStringLiteral("seed"), 1}}, size);
    const int back = run.readBack(m_output);
    if (!run.submit()) {
        m_pictureKey = 0; // whatever is on the card is not known to be the picture
        return failed(run.error());
    }
    m_pictureKey = key;
    m_gpuMs = run.gpuMilliseconds();
    return run.image(back);
}

QFuture<QImage> Chain::renderLater(const QImage &picture)
{
    return async::run([this, picture] { return render(picture); });
}

} // namespace lumen::gpu
