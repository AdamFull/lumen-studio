#include "ui/Grading.h"

#include "core/Paths.h"
#include "filters/Ops.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFloat16>
#include <QJsonArray>
#include <QMutex>
#include <QTextStream>

#include <cmath>

namespace lumen::ui::grading {

namespace {

struct Text
{
    Q_DECLARE_TR_FUNCTIONS(Grading)
};

const QString kCurve = QStringLiteral("curve");
const QString kLut = QStringLiteral("lut");

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
    static const QStringList order{QStringLiteral("develop"), kCurve, QStringLiteral("mixer"), QStringLiteral("grading"), kLut};
    return order;
}

const QStringList &curveChannels()
{
    static const QStringList channels{QStringLiteral("rgb"), QStringLiteral("r"), QStringLiteral("g"), QStringLiteral("b")};
    return channels;
}

QString straightCurve()
{
    return QStringLiteral("0 0, 255 255");
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
    QImage image(256, 1, QImage::Format_RGBA16FPx4);
    QList<int> tables[4];
    for (int c = 0; c < 4; ++c) {
        const QString points = curves.value(curveChannels()[c]);
        tables[c] = filters::ops::curveTable(filters::ops::parseCurve(points.trimmed().isEmpty() ? straightCurve() : points));
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

} // namespace lumen::ui::grading
