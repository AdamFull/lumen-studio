#include "live2d/Motion.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

#include <cmath>

namespace lumen::live2d {

namespace {

std::optional<QJsonObject> readJson(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QObject::tr("Cannot open %1: %2").arg(QDir::toNativeSeparators(path), f.errorString());
        return std::nullopt;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (!doc.isObject()) {
        if (error)
            *error = QObject::tr("%1 is not valid JSON: %2").arg(QFileInfo(path).fileName(), pe.errorString());
        return std::nullopt;
    }
    return doc.object();
}

QString resolvePath(const QString &directory, const QString &relative)
{
    if (relative.isEmpty())
        return {};
    return QDir::cleanPath(QDir(directory).filePath(relative));
}

double cubic(double a, double b, double c, double d, double t)
{
    const double u = 1.0 - t;
    return u * u * u * a + 3 * u * u * t * b + 3 * u * t * t * c + t * t * t * d;
}

} // namespace

// ---- Model settings ------------------------------------------------------------------------------------

std::optional<ModelSettings> ModelSettings::load(const QString &path, QString *error)
{
    const auto json = readJson(path, error);
    if (!json)
        return std::nullopt;
    return fromJson(*json, QFileInfo(path).absolutePath(), error);
}

std::optional<ModelSettings> ModelSettings::fromJson(const QJsonObject &json, const QString &directory, QString *error)
{
    ModelSettings s;
    s.directory = directory;
    const QJsonObject refs = json.value(QStringLiteral("FileReferences")).toObject();
    s.moc = resolvePath(directory, refs.value(QStringLiteral("Moc")).toString());
    if (s.moc.isEmpty()) {
        if (error)
            *error = QObject::tr("The model settings have no FileReferences.Moc entry (is this a .model3.json?).");
        return std::nullopt;
    }
    for (const QJsonValue &t : refs.value(QStringLiteral("Textures")).toArray())
        s.textures << resolvePath(directory, t.toString());
    s.physics = resolvePath(directory, refs.value(QStringLiteral("Physics")).toString());
    const QJsonObject motions = refs.value(QStringLiteral("Motions")).toObject();
    for (auto it = motions.begin(); it != motions.end(); ++it) {
        QStringList files;
        for (const QJsonValue &m : it.value().toArray())
            files << resolvePath(directory, m.toObject().value(QStringLiteral("File")).toString());
        s.motions.insert(it.key(), files);
    }
    for (const QJsonValue &g : json.value(QStringLiteral("Groups")).toArray()) {
        const QJsonObject group = g.toObject();
        QStringList ids;
        for (const QJsonValue &id : group.value(QStringLiteral("Ids")).toArray())
            ids << id.toString();
        const QString name = group.value(QStringLiteral("Name")).toString();
        if (name == QLatin1String("EyeBlink"))
            s.eyeBlinkIds = ids;
        else if (name == QLatin1String("LipSync"))
            s.lipSyncIds = ids;
    }
    return s;
}

QString ModelSettings::idleMotion() const
{
    for (auto it = motions.begin(); it != motions.end(); ++it) {
        if (it.key().compare(QLatin1String("Idle"), Qt::CaseInsensitive) == 0 && !it.value().isEmpty())
            return it.value().first();
    }
    return {};
}

// ---- Curves ---------------------------------------------------------------------------------------------

double MotionCurve::valueAt(double time) const
{
    if (segments.isEmpty())
        return 0.0;
    if (time <= segments.first().p0.time)
        return segments.first().p0.value;
    for (const Segment &s : segments) {
        const Point &end = s.type == SegmentType::Bezier ? s.p3 : s.p1;
        if (time > end.time)
            continue;
        switch (s.type) {
        case SegmentType::Linear: {
            const double span = end.time - s.p0.time;
            const double t = span > 0 ? (time - s.p0.time) / span : 1.0;
            return s.p0.value + (end.value - s.p0.value) * std::clamp(t, 0.0, 1.0);
        }
        case SegmentType::Stepped:
            return time < end.time ? s.p0.value : end.value;
        case SegmentType::InverseStepped:
            return time > s.p0.time ? end.value : s.p0.value;
        case SegmentType::Bezier: {
            // Solve x(t) = time (x is monotonic in valid motions), then evaluate y(t).
            double lo = 0.0, hi = 1.0;
            for (int i = 0; i < 30; ++i) {
                const double mid = (lo + hi) / 2.0;
                if (cubic(s.p0.time, s.p1.time, s.p2.time, s.p3.time, mid) < time)
                    lo = mid;
                else
                    hi = mid;
            }
            return cubic(s.p0.value, s.p1.value, s.p2.value, s.p3.value, (lo + hi) / 2.0);
        }
        }
    }
    const Segment &last = segments.last();
    return (last.type == SegmentType::Bezier ? last.p3 : last.p1).value;
}

// ---- Motion ---------------------------------------------------------------------------------------------

std::optional<Motion> Motion::load(const QString &path, QString *error)
{
    const auto json = readJson(path, error);
    if (!json)
        return std::nullopt;
    return fromJson(*json, error);
}

std::optional<Motion> Motion::fromJson(const QJsonObject &json, QString *error)
{
    Motion m;
    const QJsonObject meta = json.value(QStringLiteral("Meta")).toObject();
    m.m_duration = meta.value(QStringLiteral("Duration")).toDouble();
    m.m_fps = meta.value(QStringLiteral("Fps")).toDouble(30.0);
    m.m_loop = meta.value(QStringLiteral("Loop")).toBool(true);
    for (const QJsonValue &cv : json.value(QStringLiteral("Curves")).toArray()) {
        const QJsonObject c = cv.toObject();
        MotionCurve curve;
        const QString target = c.value(QStringLiteral("Target")).toString();
        curve.target = target == QLatin1String("PartOpacity") ? MotionCurve::Target::PartOpacity
                       : target == QLatin1String("Model")     ? MotionCurve::Target::Model
                                                              : MotionCurve::Target::Parameter;
        curve.id = c.value(QStringLiteral("Id")).toString();
        curve.fadeIn = c.value(QStringLiteral("FadeInTime")).toDouble(-1.0);
        curve.fadeOut = c.value(QStringLiteral("FadeOutTime")).toDouble(-1.0);
        const QJsonArray seg = c.value(QStringLiteral("Segments")).toArray();
        if (seg.size() < 2)
            continue;
        MotionCurve::Point last{seg[0].toDouble(), seg[1].toDouble()};
        int i = 2;
        bool ok = true;
        while (i < seg.size()) {
            const auto type = static_cast<MotionCurve::SegmentType>(seg[i].toInt());
            auto point = [&](int k) { return MotionCurve::Point{seg[i + 1 + 2 * k].toDouble(), seg[i + 2 + 2 * k].toDouble()}; };
            MotionCurve::Segment s{type, last, last, last, last};
            if (type == MotionCurve::SegmentType::Bezier) {
                if (i + 6 >= seg.size()) {
                    ok = false;
                    break;
                }
                s.p1 = point(0);
                s.p2 = point(1);
                s.p3 = point(2);
                last = s.p3;
                i += 7;
            } else if (type == MotionCurve::SegmentType::Linear || type == MotionCurve::SegmentType::Stepped
                       || type == MotionCurve::SegmentType::InverseStepped) {
                if (i + 2 >= seg.size()) {
                    ok = false;
                    break;
                }
                s.p1 = point(0);
                last = s.p1;
                i += 3;
            } else {
                ok = false;
                break;
            }
            curve.segments.append(s);
        }
        if (!ok) {
            if (error)
                *error = QObject::tr("Malformed segments in motion curve %1").arg(curve.id);
            return std::nullopt;
        }
        m.m_curves.append(curve);
    }
    return m;
}

void Motion::evaluate(double time, const std::function<void(MotionCurve::Target, const QString &, double)> &sink) const
{
    double t = time;
    if (m_duration > 0) {
        if (m_loop)
            t = std::fmod(std::max(0.0, time), m_duration);
        else
            t = std::clamp(time, 0.0, m_duration);
    }
    for (const MotionCurve &c : m_curves)
        sink(c.target, c.id, c.valueAt(t));
}

} // namespace lumen::live2d
