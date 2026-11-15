#include "ui/Grading.h"

#include "core/Error.h"
#include "core/Paths.h"
#include "doc/ImageFile.h"
#include "filters/EffectNodes.h"
#include "filters/Ops.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFloat16>
#include <QDirIterator>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeData>
#include <QMutex>
#include <QSaveFile>
#include <QTextStream>

#include <cmath>

namespace lumen::ui::grading {

namespace {

struct Text
{
    Q_DECLARE_TR_FUNCTIONS(lumen::ui::Grading)
};

const QString kCurve = QStringLiteral("curve");
const QString kLut = QStringLiteral("lut");
const QString kMime = QStringLiteral("application/x-lumen-develop");

// Lumen's presets' names, translated where they are shown.
[[maybe_unused]] const char *const kPresetNames[] = {
    QT_TRANSLATE_NOOP("lumen::ui::Grading", "Warm film"),     QT_TRANSLATE_NOOP("lumen::ui::Grading", "Cool and crisp"),
    QT_TRANSLATE_NOOP("lumen::ui::Grading", "Black and white"), QT_TRANSLATE_NOOP("lumen::ui::Grading", "Soft matte"),
    QT_TRANSLATE_NOOP("lumen::ui::Grading", "Vivid"),
};

QJsonObject presetFile(const QString &name, const QJsonObject &grading)
{
    return {{QStringLiteral("lumen"), QStringLiteral("develop-preset")},
            {QStringLiteral("version"), 1},
            {QStringLiteral("name"), name},
            {QStringLiteral("grading"), grading}};
}

bool sameValues(const QVariantMap &values, const QVariantMap &defaults)
{
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        const QVariant d = defaults.value(it.key());
        const QVariantList a = it.value().typeId() == QMetaType::QVariantList ? it.value().toList() : QVariantList{it.value()};
        const QVariantList b = d.typeId() == QMetaType::QVariantList ? d.toList() : QVariantList{d};
        if (a.size() != b.size())
            return false;
        for (int i = 0; i < a.size(); ++i) {
            if (std::abs(a[i].toDouble() - b[i].toDouble()) > 1e-9)
                return false;
        }
    }
    return true;
}

bool straight(const QString &points)
{
    if (points.trimmed().isEmpty())
        return true;
    const QList<int> table = filters::ops::curveTable(filters::ops::parseCurve(points));
    for (int i = 0; i < table.size(); ++i) {
        if (table[i] != i)
            return false;
    }
    return true;
}

bool curvesStraight(const QHash<QString, QString> &curves)
{
    for (const QString &points : curves) {
        if (!straight(points))
            return false;
    }
    return true;
}

bool effectNeutral(const QString &id, const State &state, const gpu::Effect &effect)
{
    if (!sameValues(state.values.value(id), effect.defaults()))
        return false;
    if (id == kCurve)
        return curvesStraight(state.curves);
    if (id == kLut)
        return state.lut.isEmpty();
    return true;
}

} // namespace

const QStringList &effectOrder()
{
    // Noise and sharpening on the picture as it came, before local contrast lifts the noise; the
    // vignette and grain last, over the finished look.
    static const QStringList order{QStringLiteral("develop"), QStringLiteral("detail"), QStringLiteral("local"), kCurve,
                                   QStringLiteral("mixer"), QStringLiteral("grading"), kLut, QStringLiteral("finish")};
    return order;
}

const QStringList &panelOrder()
{
    static const QStringList order{QStringLiteral("develop"), QStringLiteral("local"), kCurve, QStringLiteral("mixer"),
                                   QStringLiteral("grading"), kLut, QStringLiteral("detail"), QStringLiteral("finish")};
    return order;
}

const QStringList &curveChannels()
{
    static const QStringList channels{QStringLiteral("rgb"), QStringLiteral("r"), QStringLiteral("g"), QStringLiteral("b")};
    return channels;
}

QString straightCurve()
{
    return filters::effects::straightCurve();
}

State fromJson(const QJsonObject &grading)
{
    State state;
    for (const QJsonValue &v : grading.value(QStringLiteral("steps")).toArray()) {
        const QJsonObject step = v.toObject();
        const QString id = step.value(QStringLiteral("effect")).toString();
        state.values.insert(id, step.value(QStringLiteral("values")).toObject().toVariantMap());
        const QJsonObject inputs = step.value(QStringLiteral("inputs")).toObject();
        if (id == kCurve) {
            const QJsonObject curves = inputs.value(kCurve).toObject();
            for (const QString &channel : curveChannels()) {
                if (curves.contains(channel))
                    state.curves.insert(channel, curves.value(channel).toString());
            }
        } else if (id == kLut) {
            state.lut = inputs.value(kLut).toString();
        }
    }
    return state;
}

QJsonObject toJson(const State &state, const QHash<QString, gpu::Effect> &effects)
{
    QJsonArray steps;
    for (const QString &id : effectOrder()) {
        const gpu::Effect effect = effects.value(id);
        if (effect.isNull() || effectNeutral(id, state, effect))
            continue;
        // Only what moved: a later version's new sliders start at their defaults.
        const QVariantMap defaults = effect.defaults();
        QJsonObject values;
        const QVariantMap given = state.values.value(id);
        for (auto it = given.cbegin(); it != given.cend(); ++it) {
            if (!sameValues({{it.key(), it.value()}}, defaults))
                values.insert(it.key(), QJsonValue::fromVariant(it.value()));
        }
        QJsonObject step{{QStringLiteral("effect"), id}, {QStringLiteral("values"), values}};
        if (id == kCurve) {
            QJsonObject curves;
            for (auto it = state.curves.cbegin(); it != state.curves.cend(); ++it) {
                if (!straight(it.value()))
                    curves.insert(it.key(), it.value());
            }
            step.insert(QStringLiteral("inputs"), QJsonObject{{kCurve, curves}});
        } else if (id == kLut) {
            step.insert(QStringLiteral("inputs"), QJsonObject{{kLut, state.lut}});
        }
        steps.append(step);
    }
    return steps.isEmpty() ? QJsonObject() : QJsonObject{{QStringLiteral("steps"), steps}};
}

bool isNeutral(const State &state, const QHash<QString, gpu::Effect> &effects)
{
    for (const QString &id : effectOrder()) {
        const gpu::Effect effect = effects.value(id);
        if (!effect.isNull() && !effectNeutral(id, state, effect))
            return false;
    }
    return true;
}

QList<gpu::EffectStep> steps(const State &state, const QHash<QString, gpu::Effect> &effects, QString *error)
{
    QList<gpu::EffectStep> out;
    for (const QString &id : effectOrder()) {
        const gpu::Effect effect = effects.value(id);
        if (effect.isNull() || effectNeutral(id, state, effect))
            continue;
        gpu::EffectStep step{id, state.values.value(id), true, {}};
        if (id == kCurve) {
            step.inputs.insert(kCurve, curveImage(state.curves));
        } else if (id == kLut) {
            QString why;
            const QImage lut = loadCube(QDir(paths::defaultLuts()).filePath(state.lut), &why);
            if (lut.isNull()) {
                if (error)
                    *error = why;
                return {};
            }
            step.inputs.insert(kLut, lut);
        }
        out << step;
    }
    return out;
}

QImage curveImage(const QHash<QString, QString> &curves)
{
    return filters::effects::curveImage(curves);
}

QImage loadCube(const QString &path, QString *error)
{
    return filters::effects::loadCube(path, error);
}

QStringList availableLuts()
{
    return filters::effects::availableLuts();
}

QString addLut(const QString &path, QString *error)
{
    return filters::effects::addLut(path, error);
}

QHash<QString, gpu::Effect> describedEffects()
{
    QHash<QString, gpu::Effect> out;
    for (const QString &id : effectOrder()) {
        const gpu::Effect effect = gpu::Effect::describe(id);
        if (!effect.isNull())
            out.insert(id, effect);
    }
    return out;
}

State resolved(const QJsonObject &grading, const QHash<QString, gpu::Effect> &effects)
{
    State state = fromJson(grading);
    for (auto it = effects.cbegin(); it != effects.cend(); ++it) {
        QString error;
        const QVariantMap values = it.value().resolve(state.values.value(it.key()), &error);
        state.values.insert(it.key(), error.isEmpty() ? values : it.value().defaults());
    }
    for (const QString &channel : curveChannels()) {
        if (!state.curves.contains(channel))
            state.curves.insert(channel, straightCurve());
    }
    return state;
}

// ---- Presets ----------------------------------------------------------------------------------------

QString presetsFolder()
{
    return paths::userPresets() + QStringLiteral("/develop");
}

Preset readPreset(const QString &path, QString *error)
{
    Preset p;
    p.id = QFileInfo(path).completeBaseName();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = Text::tr("%1 could not be read.").arg(QDir::toNativeSeparators(path));
        return p;
    }
    const QJsonObject o = QJsonDocument::fromJson(f.read(1 << 20)).object();
    if (o.value(QStringLiteral("lumen")).toString() != QLatin1String("develop-preset") || !o.value(QStringLiteral("grading")).isObject()) {
        if (error)
            *error = Text::tr("%1 is not a Develop preset.").arg(QFileInfo(path).fileName());
        return p;
    }
    p.grading = o.value(QStringLiteral("grading")).toObject();
    p.name = o.value(QStringLiteral("name")).toString(p.id).trimmed();
    if (p.name.isEmpty())
        p.name = p.id;
    return p;
}

QList<Preset> presets()
{
    QList<Preset> out;
    QDirIterator builtIn(QStringLiteral(":/develop"), {QStringLiteral("*.json")}, QDir::Files);
    QList<Preset> lumen;
    while (builtIn.hasNext()) {
        QString why;
        Preset p = readPreset(builtIn.next(), &why);
        if (!why.isEmpty())
            continue;
        p.builtIn = true;
        p.name = QCoreApplication::translate("lumen::ui::Grading", p.name.toUtf8().constData());
        lumen << p;
    }
    std::sort(lumen.begin(), lumen.end(), [](const Preset &a, const Preset &b) { return a.name.localeAwareCompare(b.name) < 0; });
    out << lumen;
    QList<Preset> yours;
    const QFileInfoList files = QDir(presetsFolder()).entryInfoList({QStringLiteral("*.json")}, QDir::Files);
    for (const QFileInfo &file : files) {
        QString why;
        Preset p = readPreset(file.filePath(), &why);
        if (why.isEmpty())
            yours << p;
    }
    std::sort(yours.begin(), yours.end(), [](const Preset &a, const Preset &b) { return a.name.localeAwareCompare(b.name) < 0; });
    out << yours;
    return out;
}

std::optional<Preset> preset(const QString &name)
{
    for (const Preset &p : presets()) {
        if (p.name == name)
            return p;
    }
    return std::nullopt;
}

namespace {
// A file name for a preset's name: what a file name cannot hold becomes "_".
QString fileNameFor(const QString &name)
{
    QString out = name.trimmed();
    for (QChar &c : out) {
        if (QStringLiteral("\\/:*?\"<>|").contains(c) || c.unicode() < 32)
            c = u'_';
    }
    return out;
}
} // namespace

bool savePreset(const QString &name, const QJsonObject &grading, QString *error)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > 80) {
        if (error)
            *error = Text::tr("A preset needs a name (up to 80 characters).");
        return false;
    }
    for (const Preset &p : presets()) {
        if (p.builtIn && p.name.compare(trimmed, Qt::CaseInsensitive) == 0) {
            if (error)
                *error = Text::tr("“%1” is one of Lumen's presets; give yours another name.").arg(trimmed);
            return false;
        }
    }
    QDir().mkpath(presetsFolder());
    // The same name again replaces that preset, whichever file it is in.
    QString path = QDir(presetsFolder()).filePath(fileNameFor(trimmed) + QStringLiteral(".json"));
    for (const Preset &p : presets()) {
        if (!p.builtIn && p.name == trimmed)
            path = QDir(presetsFolder()).filePath(p.id + QStringLiteral(".json"));
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(presetFile(trimmed, grading)).toJson()) < 0 || !f.commit()) {
        if (error)
            *error = Text::tr("%1 could not be written.").arg(QDir::toNativeSeparators(path));
        return false;
    }
    return true;
}

bool removePreset(const QString &name)
{
    for (const Preset &p : presets()) {
        if (!p.builtIn && p.name == name)
            return QFile::remove(QDir(presetsFolder()).filePath(p.id + QStringLiteral(".json")));
    }
    return false;
}

// ---- Clipboard ----------------------------------------------------------------------------------------

QMimeData *settingsMime(const QJsonObject &grading)
{
    auto *mime = new QMimeData;
    const QByteArray json = QJsonDocument(QJsonObject{{QStringLiteral("lumen"), QStringLiteral("develop-settings")},
                                                      {QStringLiteral("version"), 1},
                                                      {QStringLiteral("grading"), grading}})
                                .toJson(QJsonDocument::Compact);
    mime->setData(kMime, json);
    mime->setText(QString::fromUtf8(json));
    return mime;
}

std::optional<QJsonObject> settingsFrom(const QMimeData *mime)
{
    if (!mime)
        return std::nullopt;
    const QByteArray bytes = mime->hasFormat(kMime) ? mime->data(kMime) : mime->text().trimmed().toUtf8();
    const QJsonObject o = QJsonDocument::fromJson(bytes).object();
    const QString kind = o.value(QStringLiteral("lumen")).toString();
    if ((kind != QLatin1String("develop-settings") && kind != QLatin1String("develop-preset")) || !o.value(QStringLiteral("grading")).isObject())
        return std::nullopt;
    return o.value(QStringLiteral("grading")).toObject();
}

// ---- Many pictures ----------------------------------------------------------------------------------

QString gradedPath(const QString &source, const QString &folder, const QString &suffix)
{
    const QFileInfo in(source);
    const bool sameFolder = paths::sameFile(in.absolutePath(), QDir(folder).absolutePath());
    const QString base = sameFolder ? Text::tr("%1 (graded)").arg(in.completeBaseName()) : in.completeBaseName();
    QString path = QDir(folder).filePath(base + u'.' + suffix);
    for (int i = 2; QFileInfo::exists(path); ++i)
        path = QDir(folder).filePath(QStringLiteral("%1 %2.%3").arg(base).arg(i).arg(suffix));
    return path;
}

QStringList gradeFiles(const QJsonObject &grading, const QStringList &files, const QString &folder, QStringList *failed,
                       const std::function<void(int, const QString &)> &progress, const std::atomic_bool *cancelled)
{
    QStringList written;
    const QHash<QString, gpu::Effect> effects = describedEffects();
    const State state = resolved(grading, effects);
    QString error;
    const QList<gpu::EffectStep> steps = grading::steps(state, effects, &error);
    if (!error.isEmpty()) {
        for (const QString &file : files) {
            if (failed)
                *failed << QStringLiteral("%1: %2").arg(QFileInfo(file).fileName(), error);
        }
        return written;
    }
    gpu::Chain chain;
    if (!steps.isEmpty() && !chain.setSteps(steps, &error)) {
        if (failed)
            *failed << error;
        return written;
    }
    QDir().mkpath(folder);
    for (int i = 0; i < files.size(); ++i) {
        if (cancelled && cancelled->load())
            break;
        const QString &file = files.at(i);
        const QString name = QFileInfo(file).fileName();
        if (progress)
            progress(i, file);
        QImageReader reader(file);
        reader.setAutoTransform(true);
        const QImage picture = reader.read();
        if (picture.isNull()) {
            if (failed)
                *failed << QStringLiteral("%1: %2").arg(name, reader.errorString());
            continue;
        }
        QImage graded = picture.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        if (!steps.isEmpty()) {
            graded = chain.render(graded);
            if (graded.isNull()) {
                if (failed)
                    *failed << QStringLiteral("%1: %2").arg(name, chain.error());
                continue;
            }
            graded = graded.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        }
        // Its own format where Lumen writes it (a JPEG at about its quality), else PNG.
        const std::optional<doc::ImageFormat> own = doc::imageFormatFor(file);
        const doc::ImageFormat format = own && !own->layers ? *own : *doc::imageFormatById("png");
        const int quality = format.id == "jpeg" ? doc::estimateJpegQuality(file) : -1;
        const QString target = gradedPath(file, folder, format.suffixes.value(0));
        try {
            doc::writeImageFile(target, format, {doc::LayerSnapshot{graded, {}, {}}}, graded.size(), quality);
            written << target;
        } catch (const std::exception &e) {
            if (failed)
                *failed << QStringLiteral("%1: %2").arg(name, describeException(e));
        }
    }
    return written;
}

} // namespace lumen::ui::grading
