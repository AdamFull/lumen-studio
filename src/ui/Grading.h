#pragma once

#include "gpu/Effects.h"

#include <QHash>
#include <QJsonObject>
#include <QStringList>
#include <QVariantMap>

// A layer's grading (plan M20 steps 3 and 4) between what is saved with it (Layer::grading()) and the
// effect steps that make it on the graphics card: the Develop tab's effects in their order, with
// their values, the tone curves and the LUT.
namespace lumen::ui::grading {

// The Develop tab's effects, in the order they run.
const QStringList &effectOrder();
// The same, in the order the tab shows them: the basics and local contrast, colour, then detail and
// the finishing touches, as photo editors put them.
const QStringList &panelOrder();

// The curves' channels: all three, then red, green and blue.
const QStringList &curveChannels();
// A curve that changes nothing, as CurveEditor writes it.
QString straightCurve();

struct State
{
    QHash<QString, QVariantMap> values; // effect id → its values (missing ones are the defaults)
    QHash<QString, QString> curves;     // channel → points "x y, x y" from 0 to 255
    QString lut;                        // a file name in the LUTs folder; empty for none

    bool operator==(const State &other) const = default;
};

// What is saved with the layer: {"steps": [{"effect", "values", "inputs"}]}, only the effects that
// change something.
State fromJson(const QJsonObject &grading);
QJsonObject toJson(const State &state, const QHash<QString, gpu::Effect> &effects);
// Every effect at its defaults, straight curves, no LUT.
bool isNeutral(const State &state, const QHash<QString, gpu::Effect> &effects);

// The chain's steps for a state, with the curves and the LUT as pictures; empty with the reason when
// the LUT cannot be read.
QList<gpu::EffectStep> steps(const State &state, const QHash<QString, gpu::Effect> &effects, QString *error = nullptr);

// The curves as the "curve" input: 256 × 1, red, green and blue each channel's curve, alpha the curve
// for all three, from 0 to 1.
QImage curveImage(const QHash<QString, QString> &curves);

// A .cube file as the "lut" input (N² × N, half floats); null with the reason when it cannot be read.
QImage loadCube(const QString &path, QString *error = nullptr);
// The LUTs in the LUTs folder, and adding one there (a copy, after checking it can be read).
QStringList availableLuts();
QString addLut(const QString &path, QString *error = nullptr);

} // namespace lumen::ui::grading
