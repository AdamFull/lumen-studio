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
        for (QVariant &v : list) {
            double d = v.toDouble();
            if (!std::isfinite(d))
                d = param->defaults.value(0);
            if (param->hasRange)
                d = std::clamp(d, param->minimum, param->maximum);
            if (param->scalar != QLatin1String("float32"))
                d = std::round(d);
            v = d;
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
        if (step.enabled)
            prepared << Prepared{effect, values};
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
    auto texture = [&](Texture &t, Format format) {
        if (t.isNull() || t.size() != size)
            t = m_context.texture(size, format, &why);
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
        for (const Kernel &pass : step.effect.passes()) {
            QHash<QString, Texture> bound{{kSource, current}, {kResult, target}};
            for (const QString &name : pass.info().bindings.keys()) {
                if (name == kSource || name == kResult)
                    continue;
                Texture &scratch = m_scratch[step.effect.id() + u'/' + name];
                if (!texture(scratch, Format::RGBA16F))
                    return failed(why);
                bound.insert(name, scratch);
            }
            run.dispatch(pass, bound, step.values, size);
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
