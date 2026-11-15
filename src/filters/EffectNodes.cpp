#include "filters/EffectNodes.h"

#include "core/Error.h"
#include "core/Paths.h"
#include "filters/Ops.h"
#include "filters/Registry.h"
#include "gpu/Effects.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFloat16>
#include <QMutex>
#include <QTextStream>

#include <atomic>
#include <cmath>
#include <optional>

namespace lumen::filters::effects {

namespace {

struct Text
{
    Q_DECLARE_TR_FUNCTIONS(lumen::filters::EffectNodes)
};

const QString kImage = QStringLiteral("image");
const QString kCurve = QStringLiteral("curve");
const QString kLut = QStringLiteral("lut");

// The effects' own words (titles, labels, sections) are translated with the Develop tab's.
QString effectWord(const QString &english)
{
    return QCoreApplication::translate("lumen::ui::DevelopPanel", english.toUtf8().constData());
}

struct Known
{
    const char *effect;
    const char *node;
};

// The built-in effects with a node, in the menu's order (the Develop tab's).
const Known kKnown[] = {
    {"develop", "develop"},       {"local", "local-contrast"},   {"curve", "tone-curve"}, {"mixer", "colour-mixer"},
    {"grading", "colour-grading"}, {"lut", "lut"},               {"detail", "detail"},    {"finish", "vignette-grain"},
};

QString describeNode(const QString &effect)
{
    if (effect == QLatin1String("develop"))
        return Text::tr("White balance, exposure, contrast, highlights and shadows, vibrance and saturation.");
    if (effect == QLatin1String("local"))
        return Text::tr("Texture, clarity and dehaze: the contrast of fine detail and of larger shapes, and haze.");
    if (effect == QLatin1String("curve"))
        return Text::tr("A curve for all three colours and one for each.");
    if (effect == QLatin1String("mixer"))
        return Text::tr("The hue, saturation and luminance of eight colours.");
    if (effect == QLatin1String("grading"))
        return Text::tr("Tints for the shadows, midtones and highlights.");
    if (effect == QLatin1String("lut"))
        return Text::tr("A look from a LUT (.cube) in your LUTs folder.");
    if (effect == QLatin1String("detail"))
        return Text::tr("Sharpening and noise reduction.");
    if (effect == QLatin1String("finish"))
        return Text::tr("A vignette and film grain.");
    return {};
}

std::atomic_int g_runs = 0;

// Chains kept between runs, so the same picture is not sent to the graphics card again while a
// slider moves. Two: a chain holds several full-size textures.
class ChainPool
{
public:
    static ChainPool &instance()
    {
        static ChainPool pool;
        return pool;
    }

    std::shared_ptr<gpu::Chain> take(qint64 picture)
    {
        QMutexLocker lock(&m_lock);
        Slot *best = nullptr;
        for (Slot &s : m_slots) {
            if (s.busy)
                continue;
            if (s.picture == picture) {
                best = &s;
                break;
            }
            if (!best || s.used < best->used)
                best = &s;
        }
        if (!best && m_slots.size() < 2) {
            m_slots.append(Slot{std::make_shared<gpu::Chain>()});
            best = &m_slots.last();
        }
        if (!best)
            return std::make_shared<gpu::Chain>(); // both busy: one for this run alone
        best->busy = true;
        best->picture = picture;
        best->used = ++m_clock;
        return best->chain;
    }

    void give(const std::shared_ptr<gpu::Chain> &chain)
    {
        QMutexLocker lock(&m_lock);
        for (Slot &s : m_slots) {
            if (s.chain == chain)
                s.busy = false;
        }
    }

private:
    struct Slot
    {
        std::shared_ptr<gpu::Chain> chain;
        qint64 picture = 0;
        bool busy = false;
        quint64 used = 0;
    };
    QMutex m_lock;
    QList<Slot> m_slots;
    quint64 m_clock = 0;
};

// A picture with effects still to run over it on the graphics card.
class EffectRun : public PendingImage
{
public:
    EffectRun(QImage base, QList<gpu::EffectStep> steps)
        : m_base(std::move(base))
        , m_steps(std::move(steps))
    {
    }

    // `input` (a picture, or effects still to run) with one more effect after it.
    static PendingImagePtr after(const QVariant &input, const gpu::EffectStep &step)
    {
        if (const auto run = std::dynamic_pointer_cast<const EffectRun>(pendingOf(input)))
            return std::make_shared<EffectRun>(run->m_base, run->m_steps + QList<gpu::EffectStep>{step});
        return std::make_shared<EffectRun>(imageOf(input), QList<gpu::EffectStep>{step});
    }

    QImage image() const override
    {
        QMutexLocker lock(&m_lock);
        if (m_made)
            return *m_made;
        ChainPool &pool = ChainPool::instance();
        const std::shared_ptr<gpu::Chain> chain = pool.take(m_base.cacheKey());
        QString error;
        QImage out;
        if (chain->setSteps(m_steps, &error)) {
            out = chain->render(m_base);
            if (out.isNull())
                error = chain->error();
        }
        pool.give(chain);
        if (out.isNull())
            throw Error(Text::tr("The graphics card could not run the effects: %1").arg(error));
        ++g_runs;
        m_made = out.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        return *m_made;
    }

private:
    const QImage m_base;
    const QList<gpu::EffectStep> m_steps;
    mutable QMutex m_lock;
    mutable std::optional<QImage> m_made;
};

// The tone curve's channels as node parameters.
struct CurveParam
{
    const char *id;
    const char *channel;
};
const CurveParam kCurves[] = {{"curve", "rgb"}, {"curveRed", "r"}, {"curveGreen", "g"}, {"curveBlue", "b"}};

class EffectNode : public NodeType
{
public:
    EffectNode(NodeTypeInfo info, gpu::Effect effect)
        : NodeType(std::move(info))
        , m_effect(std::move(effect))
    {
    }

    QString unavailableReason(const QVariantMap &) const override
    {
        gpu::Context &gpu = gpu::Context::instance();
        return gpu.isAvailable() ? QString() : Text::tr("It needs the graphics card: %1").arg(gpu.unavailableReason());
    }

    NodeResult evaluate(const NodeArgs &args, EvalContext &) const override
    {
        const QVariant input = args.inputs().value(kImage);
        gpu::EffectStep step{m_effect.id(), {}};
        bool changes = false;
        const QVariantMap defaults = m_effect.defaults();
        for (const gpu::EffectParam &p : m_effect.params()) {
            if (!p.shown())
                continue;
            if (p.control == QLatin1String("wheel") && p.components == 2) {
                // A tint: the colour's hue and how strong it is; its brightness does not count.
                const QColor c = args.color(p.name);
                const double hue = std::max(0.0, double(c.hsvHueF())) * 360.0;
                const double saturation = std::max(0.0, double(c.hsvSaturationF())) * 100.0;
                step.values.insert(p.name, QVariantList{hue, saturation});
                changes = changes || saturation > 0.0;
            } else if (p.components == 1) {
                const double value = args.number(p.name);
                step.values.insert(p.name, value);
                changes = changes || std::abs(value - defaults.value(p.name).toDouble()) > 1e-9;
            }
        }
        const QHash<QString, QString> inputs = m_effect.inputs();
        if (inputs.contains(kCurve)) {
            QHash<QString, QString> curves;
            for (const CurveParam &c : kCurves) {
                const QString points = args.text(QLatin1String(c.id));
                curves.insert(QLatin1String(c.channel), points);
                const QList<int> table = ops::curveTable(ops::parseCurve(points.trimmed().isEmpty() ? straightCurve() : points));
                for (int i = 0; i < table.size(); ++i)
                    changes = changes || table[i] != i;
            }
            step.inputs.insert(kCurve, curveImage(curves));
        }
        if (inputs.contains(kLut)) {
            const QString name = args.text(kLut).trimmed();
            if (!name.isEmpty()) {
                QString why;
                const QImage cube = loadCube(QDir(paths::defaultLuts()).filePath(name), &why);
                if (cube.isNull())
                    throw Error(why);
                step.inputs.insert(kLut, cube);
                changes = true;
            } else {
                changes = false; // no LUT: Amount alone changes nothing
            }
        }
        // Nothing to do: the input as it came, pending or not.
        if (!changes)
            return Values{{kImage, input}};
        gpu::Context &gpu = gpu::Context::instance();
        if (!gpu.isAvailable())
            throw Error(Text::tr("It needs the graphics card: %1").arg(gpu.unavailableReason()));
        return Values{{kImage, QVariant::fromValue(EffectRun::after(input, step))}};
    }

private:
    gpu::Effect m_effect;
};

NodeTypePtr makeNode(const QString &nodeId, const gpu::Effect &effect)
{
    NodeTypeInfo info;
    info.id = nodeId;
    info.name = effectWord(effect.title());
    info.category = categories::Develop;
    info.description = describeNode(effect.id());
    info.takesPending = true;
    info.inputs = {{kImage, Text::tr("Image"), PortType::Image, false}};
    info.outputs = {{kImage, Text::tr("Image"), PortType::Image, false}};

    if (effect.inputs().contains(kCurve)) {
        const QString labels[] = {Text::tr("Curve for all three"), Text::tr("Red curve"), Text::tr("Green curve"), Text::tr("Blue curve")};
        for (int i = 0; i < 4; ++i) {
            ParamSpec spec;
            spec.id = QLatin1String(kCurves[i].id);
            spec.label = labels[i];
            spec.type = PortType::Text;
            spec.defaultValue = straightCurve();
            spec.editor = QStringLiteral("curve");
            info.params << spec;
        }
    }
    if (effect.inputs().contains(kLut)) {
        ParamSpec spec;
        spec.id = kLut;
        spec.label = effectWord(QStringLiteral("LUT"));
        spec.type = PortType::Text;
        spec.defaultValue = QString();
        spec.editor = QStringLiteral("lut");
        spec.help = Text::tr("A .cube file in your LUTs folder; the Develop tab's Add… puts one there.");
        info.params << spec;
    }

    // A label two parameters share ("Amount" of the vignette and of the grain) gets its section.
    QHash<QString, int> uses;
    for (const gpu::EffectParam &p : effect.params()) {
        if (p.shown())
            ++uses[p.label];
    }
    for (const gpu::EffectParam &p : effect.params()) {
        if (!p.shown())
            continue;
        ParamSpec spec;
        spec.id = p.name;
        spec.label = uses.value(p.label) > 1 && !p.section.isEmpty()
                         ? Text::tr("%1: %2").arg(effectWord(p.section), effectWord(p.label))
                         : effectWord(p.label);
        if (p.control == QLatin1String("wheel") && p.components == 2) {
            spec.type = PortType::Color;
            spec.defaultValue = QColor::fromHsvF(float(p.defaults.value(0) / 360.0), float(p.defaults.value(1) / 100.0), 1.0f);
            spec.help = Text::tr("The tint's colour: its hue and how strong it is count, not how light it is.");
        } else if (p.components == 1) {
            spec.type = PortType::Number;
            spec.minimum = p.minimum;
            spec.maximum = p.maximum;
            spec.defaultValue = p.defaults.value(0);
            spec.decimals = p.maximum - p.minimum <= 20 ? 2 : 0;
            spec.unit = p.unit;
            // Detail's radius is in the picture's pixels: smaller in a preview at a smaller size.
            spec.pixels = effect.id() == QLatin1String("detail") && p.name == QLatin1String("radius");
        } else {
            continue;
        }
        info.params << spec;
    }
    return std::make_shared<EffectNode>(std::move(info), effect);
}

} // namespace

void registerNodes(NodeRegistry &registry)
{
    for (const Known &k : kKnown) {
        QString error;
        const gpu::Effect effect = gpu::Effect::describe(QLatin1String(k.effect), &error);
        if (effect.isNull()) {
            qWarning("No node for the effect %s: %s", k.effect, qPrintable(error));
            continue;
        }
        registry.add(makeNode(QLatin1String(k.node), effect));
    }
}

QString nodeIdOf(const QString &effect)
{
    for (const Known &k : kKnown) {
        if (effect == QLatin1String(k.effect))
            return QLatin1String(k.node);
    }
    return {};
}

int chainRuns()
{
    return g_runs.load();
}

QString straightCurve()
{
    return QStringLiteral("0 0, 255 255");
}

QImage curveImage(const QHash<QString, QString> &curves)
{
    static const char *const kChannels[] = {"rgb", "r", "g", "b"};
    QImage image(256, 1, QImage::Format_RGBA16FPx4);
    QList<int> tables[4];
    for (int c = 0; c < 4; ++c) {
        const QString points = curves.value(QLatin1String(kChannels[c]));
        tables[c] = ops::curveTable(ops::parseCurve(points.trimmed().isEmpty() ? straightCurve() : points));
    }
    auto *px = reinterpret_cast<qfloat16 *>(image.scanLine(0));
    for (int i = 0; i < 256; ++i) {
        // Red, green, blue: each channel's curve; alpha: the one for all three.
        px[i * 4 + 0] = qfloat16(tables[1].value(i, i) / 255.0f);
        px[i * 4 + 1] = qfloat16(tables[2].value(i, i) / 255.0f);
        px[i * 4 + 2] = qfloat16(tables[3].value(i, i) / 255.0f);
        px[i * 4 + 3] = qfloat16(tables[0].value(i, i) / 255.0f);
    }
    return image;
}

QImage loadCube(const QString &path, QString *error)
{
    auto failed = [error, &path](const QString &why) {
        if (error)
            *error = Text::tr("%1 cannot be used as a LUT: %2").arg(QFileInfo(path).fileName(), why);
        return QImage();
    };
    // Read once per file and change.
    static QMutex lock;
    static QHash<QString, QPair<QDateTime, QImage>> cache;
    const QFileInfo info(path);
    {
        QMutexLocker locker(&lock);
        const auto it = cache.constFind(path);
        if (it != cache.cend() && it->first == info.lastModified())
            return it->second;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return failed(Text::tr("the file is not there"));
    QTextStream in(&file);
    int n = 0;
    QList<float> values;
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(u'#'))
            continue;
        const QStringList words = line.split(u' ', Qt::SkipEmptyParts);
        const QString key = words.first().toUpper();
        if (key == QLatin1String("LUT_3D_SIZE")) {
            n = words.value(1).toInt();
        } else if (key == QLatin1String("LUT_1D_SIZE")) {
            return failed(Text::tr("it is a 1D LUT; the Develop tab takes 3D ones"));
        } else if (key.at(0).isLetter()) {
            continue; // TITLE, DOMAIN_MIN, DOMAIN_MAX and the like
        } else if (words.size() == 3) {
            for (const QString &w : words)
                values << w.toFloat();
        }
    }
    if (n < 2 || n > 128)
        return failed(Text::tr("no LUT_3D_SIZE between 2 and 128"));
    if (values.size() != n * n * n * 3)
        return failed(Text::tr("%1 values instead of %2").arg(values.size()).arg(n * n * n * 3));
    // Red runs fastest, then green, then blue: blue picks the block of columns, red the column in
    // it, green the row.
    QImage image(n * n, n, QImage::Format_RGBA16FPx4);
    for (int b = 0; b < n; ++b) {
        for (int g = 0; g < n; ++g) {
            auto *row = reinterpret_cast<qfloat16 *>(image.scanLine(g));
            for (int r = 0; r < n; ++r) {
                const int at = ((b * n + g) * n + r) * 3;
                qfloat16 *px = row + (r + b * n) * 4;
                px[0] = qfloat16(values[at]);
                px[1] = qfloat16(values[at + 1]);
                px[2] = qfloat16(values[at + 2]);
                px[3] = qfloat16(1.0f);
            }
        }
    }
    QMutexLocker locker(&lock);
    cache.insert(path, {info.lastModified(), image});
    return image;
}

QStringList availableLuts()
{
    return QDir(paths::defaultLuts()).entryList({QStringLiteral("*.cube")}, QDir::Files, QDir::Name | QDir::IgnoreCase);
}

QString addLut(const QString &path, QString *error)
{
    if (loadCube(path, error).isNull())
        return {};
    const QDir folder(paths::defaultLuts());
    QDir().mkpath(folder.path());
    const QString name = QFileInfo(path).fileName();
    const QString target = folder.filePath(name);
    if (QFileInfo(target) == QFileInfo(path))
        return name;
    // The same file again is fine; another one of the same name gets a number.
    QString chosen = name;
    for (int i = 2; QFile::exists(folder.filePath(chosen)); ++i) {
        QFile a(folder.filePath(chosen));
        QFile b(path);
        if (a.open(QIODevice::ReadOnly) && b.open(QIODevice::ReadOnly) && a.readAll() == b.readAll())
            return chosen;
        chosen = QStringLiteral("%1 %2.cube").arg(QFileInfo(name).completeBaseName()).arg(i);
    }
    if (!QFile::copy(path, folder.filePath(chosen))) {
        if (error)
            *error = Text::tr("%1 could not be copied to %2.").arg(name, QDir::toNativeSeparators(folder.path()));
        return {};
    }
    return chosen;
}

} // namespace lumen::filters::effects
