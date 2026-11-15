#include "ui/Grading.h"

#include "core/Paths.h"
#include "filters/EffectNodes.h"
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

} // namespace lumen::ui::grading
