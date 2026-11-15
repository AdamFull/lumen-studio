#pragma once

#include "gpu/Effects.h"

#include <QHash>
#include <QJsonObject>
#include <QStringList>

#include <atomic>
#include <functional>
#include <optional>

class QMimeData;
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

// The Develop tab's effects as described by their shaders (no graphics card needed).
QHash<QString, gpu::Effect> describedEffects();
// A saved grading as a full state: every effect's values (its defaults where the grading has none),
// every curve (straight where it has none).
State resolved(const QJsonObject &grading, const QHash<QString, gpu::Effect> &effects);

// Presets (plan M20 step 8): a grading with a name. Lumen's own (read-only), then yours, in the
// presets folder as .json files. Applying one sets every control: what it has, the rest to start.
struct Preset
{
    QString id;          // the file's name without ".json"
    QString name;        // shown (Lumen's translated)
    QJsonObject grading;
    bool builtIn = false;
};
QList<Preset> presets();
std::optional<Preset> preset(const QString &name);
QString presetsFolder();
// Saves (or replaces) one of yours; false with the reason.
bool savePreset(const QString &name, const QJsonObject &grading, QString *error = nullptr);
bool removePreset(const QString &name);
// A preset file read (one someone shared); null grading with the reason when it is not one.
Preset readPreset(const QString &path, QString *error = nullptr);

// Develop settings on the clipboard: Lumen's own type and the JSON as text.
QMimeData *settingsMime(const QJsonObject &grading);
std::optional<QJsonObject> settingsFrom(const QMimeData *mime);

// Applying a grading to picture files (plan M20 step 8): each read, graded at full size on the
// graphics card and written into `folder` in its own format (PNG where Lumen cannot write it),
// never over a file that is there. Calls `progress` before each file; stops when `cancelled` is
// set. Returns the files written; `failed` gets "name: reason" for the others. Any thread.
QStringList gradeFiles(const QJsonObject &grading, const QStringList &files, const QString &folder, QStringList *failed,
                       const std::function<void(int done, const QString &file)> &progress = {},
                       const std::atomic_bool *cancelled = nullptr);
// Where a graded copy of `source` goes in `folder`: the same name in another folder, " (graded)"
// added in its own; numbered when that is taken.
QString gradedPath(const QString &source, const QString &folder, const QString &suffix);

} // namespace lumen::ui::grading
