#include "live2d/Animation.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

#include <cmath>
#include <limits>

namespace lumen::live2d {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDefaultFade = 1.0;

std::optional<QJsonObject> readJson(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QObject::tr("Cannot open %1: %2").arg(QFileInfo(path).fileName(), f.errorString());
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

double fadeWeight(double now, double start, double end, double fadeIn, double fadeOut)
{
    const double in = fadeIn <= 0.0 ? 1.0 : easeSine((now - start) / fadeIn);
    const double out = (fadeOut <= 0.0 || end < 0.0) ? 1.0 : easeSine((end - now) / fadeOut);
    return in * out;
}

} // namespace

double easeSine(double v)
{
    if (v <= 0.0)
        return 0.0;
    if (v >= 1.0)
        return 1.0;
    return 0.5 - 0.5 * std::cos(v * kPi);
}

// ---- Motions --------------------------------------------------------------------------------------------

MotionPlayer::Priority MotionPlayer::currentPriority(double now) const
{
    for (auto it = m_entries.crbegin(); it != m_entries.crend(); ++it) {
        if (!it->stopped && (it->end < 0.0 || now < it->end))
            return it->options.priority;
    }
    return Priority::None;
}

bool MotionPlayer::play(std::shared_ptr<const Motion> motion, double now, const Options &options)
{
    if (!motion)
        return false;
    if (options.priority != Priority::Force && currentPriority(now) > options.priority)
        return false;
    for (Entry &e : m_entries) {
        if (e.stopped)
            continue;
        e.stopped = true;
        const double fadeEnd = now + std::max(0.0, e.fadeOut);
        e.end = e.end < 0.0 ? fadeEnd : std::min(e.end, fadeEnd);
    }
    Entry e;
    e.motion = std::move(motion);
    e.options = options;
    e.start = now;
    e.fadeIn = options.fadeIn >= 0.0 ? options.fadeIn : (e.motion->fadeIn() >= 0.0 ? e.motion->fadeIn() : kDefaultFade);
    e.fadeOut = options.fadeOut >= 0.0 ? options.fadeOut : (e.motion->fadeOut() >= 0.0 ? e.motion->fadeOut() : kDefaultFade);
    e.end = options.loop || e.motion->duration() <= 0.0 ? (options.loop ? -1.0 : now) : now + e.motion->duration();
    m_entries.append(e);
    return true;
}

void MotionPlayer::stop(double now)
{
    for (Entry &e : m_entries) {
        if (e.stopped)
            continue;
        e.stopped = true;
        const double fadeEnd = now + std::max(0.0, e.fadeOut);
        e.end = e.end < 0.0 ? fadeEnd : std::min(e.end, fadeEnd);
    }
}

bool MotionPlayer::isPlaying(double now) const
{
    if (m_entries.isEmpty())
        return false;
    const Entry &e = m_entries.last();
    return !e.stopped && (e.end < 0.0 || now < e.end);
}

void MotionPlayer::update(double now, MotionTarget &target)
{
    for (const Entry &e : std::as_const(m_entries)) {
        const double weight = fadeWeight(now, e.start, e.end, e.fadeIn, e.fadeOut);
        if (weight <= 0.0)
            continue;
        const double duration = e.motion->duration();
        double t = now - e.start;
        if (duration > 0.0)
            t = e.options.loop ? std::fmod(std::max(0.0, t), duration) : std::clamp(t, 0.0, duration);
        double eyeBlink = -1.0, lipSync = -1.0;
        for (const MotionCurve &c : e.motion->curves()) {
            const double v = c.valueAt(t);
            switch (c.target) {
            case MotionCurve::Target::Parameter: {
                if (!target.hasParameter(c.id))
                    break;
                // Curves may carry their own fades (relative to the motion start).
                double w = weight;
                if (c.fadeIn >= 0.0 || c.fadeOut >= 0.0)
                    w = fadeWeight(now, e.start, e.end, c.fadeIn >= 0.0 ? c.fadeIn : e.fadeIn, c.fadeOut >= 0.0 ? c.fadeOut : e.fadeOut);
                const double source = target.parameter(c.id);
                target.setParameter(c.id, float(source + (v - source) * w));
                break;
            }
            case MotionCurve::Target::PartOpacity:
                target.setPartOpacity(c.id, float(v));
                break;
            case MotionCurve::Target::Model:
                if (c.id == QLatin1String("EyeBlink"))
                    eyeBlink = v;
                else if (c.id == QLatin1String("LipSync"))
                    lipSync = v;
                break;
            }
        }
        // Model curves drive the eye-blink (multiply) and lip-sync (add) parameter groups.
        if (eyeBlink >= 0.0) {
            for (const QString &id : target.eyeBlinkIds()) {
                if (target.hasParameter(id))
                    target.setParameter(id, float(target.parameter(id) * (1.0 + (eyeBlink - 1.0) * weight)));
            }
        }
        if (lipSync >= 0.0) {
            for (const QString &id : target.lipSyncIds()) {
                if (target.hasParameter(id))
                    target.setParameter(id, float(target.parameter(id) + lipSync * weight));
            }
        }
    }
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
                                   [now](const Entry &e) { return e.end >= 0.0 && now >= e.end; }),
                    m_entries.end());
}

// ---- Expressions ----------------------------------------------------------------------------------------

std::optional<Expression> Expression::load(const QString &path, const QString &name, QString *error)
{
    const auto json = readJson(path, error);
    if (!json)
        return std::nullopt;
    return fromJson(*json, name.isEmpty() ? QFileInfo(path).completeBaseName() : name, error);
}

std::optional<Expression> Expression::fromJson(const QJsonObject &json, const QString &name, QString *error)
{
    const QString type = json.value(QStringLiteral("Type")).toString(QStringLiteral("Live2D Expression"));
    if (type != QLatin1String("Live2D Expression")) {
        if (error)
            *error = QObject::tr("%1 is not a Live2D expression.").arg(name);
        return std::nullopt;
    }
    Expression e;
    e.name = name;
    e.fadeIn = json.value(QStringLiteral("FadeInTime")).toDouble(kDefaultFade);
    e.fadeOut = json.value(QStringLiteral("FadeOutTime")).toDouble(kDefaultFade);
    for (const QJsonValue &v : json.value(QStringLiteral("Parameters")).toArray()) {
        const QJsonObject p = v.toObject();
        Parameter param;
        param.id = p.value(QStringLiteral("Id")).toString();
        const QString blend = p.value(QStringLiteral("Blend")).toString(QStringLiteral("Add"));
        param.blend = blend == QLatin1String("Multiply")    ? Blend::Multiply
                      : blend == QLatin1String("Overwrite") ? Blend::Overwrite
                                                            : Blend::Add;
        param.value = p.value(QStringLiteral("Value")).toDouble(param.blend == Blend::Multiply ? 1.0 : 0.0);
        if (!param.id.isEmpty())
            e.parameters.append(param);
    }
    return e;
}

void ExpressionPlayer::set(std::shared_ptr<const Expression> expression, double now)
{
    for (Active &a : m_active) {
        if (a.end < 0.0)
            a.end = now + std::max(0.0, a.expression->fadeOut);
    }
    if (expression)
        m_active.append(Active{std::move(expression), now, -1.0});
}

QString ExpressionPlayer::current() const
{
    for (auto it = m_active.crbegin(); it != m_active.crend(); ++it) {
        if (it->end < 0.0)
            return it->expression->name;
    }
    return {};
}

void ExpressionPlayer::update(double now, MotionTarget &target)
{
    for (const Active &a : std::as_const(m_active)) {
        const double w = fadeWeight(now, a.start, a.end, a.expression->fadeIn, a.expression->fadeOut);
        if (w <= 0.0)
            continue;
        for (const Expression::Parameter &p : a.expression->parameters) {
            if (!target.hasParameter(p.id))
                continue;
            const double v = target.parameter(p.id);
            double out = v;
            switch (p.blend) {
            case Expression::Blend::Add:
                out = v + p.value * w;
                break;
            case Expression::Blend::Multiply:
                out = v * (1.0 + (p.value - 1.0) * w);
                break;
            case Expression::Blend::Overwrite:
                out = v * (1.0 - w) + p.value * w;
                break;
            }
            target.setParameter(p.id, float(out));
        }
    }
    m_active.erase(std::remove_if(m_active.begin(), m_active.end(), [now](const Active &a) { return a.end >= 0.0 && now >= a.end; }),
                   m_active.end());
}

// ---- Pose -----------------------------------------------------------------------------------------------

std::optional<Pose> Pose::load(const QString &path, QString *error)
{
    const auto json = readJson(path, error);
    if (!json)
        return std::nullopt;
    return fromJson(*json, error);
}

std::optional<Pose> Pose::fromJson(const QJsonObject &json, QString *error)
{
    Pose pose;
    pose.m_fadeTime = json.value(QStringLiteral("FadeInTime")).toDouble(0.5);
    if (pose.m_fadeTime <= 0.0)
        pose.m_fadeTime = 0.5;
    for (const QJsonValue &gv : json.value(QStringLiteral("Groups")).toArray()) {
        QList<Part> group;
        for (const QJsonValue &pv : gv.toArray()) {
            const QJsonObject po = pv.toObject();
            Part part;
            part.id = po.value(QStringLiteral("Id")).toString();
            for (const QJsonValue &l : po.value(QStringLiteral("Link")).toArray())
                part.links << l.toString();
            if (!part.id.isEmpty())
                group.append(part);
        }
        if (!group.isEmpty())
            pose.m_groups.append(group);
    }
    if (pose.m_groups.isEmpty()) {
        if (error)
            *error = QObject::tr("The pose file has no groups.");
        return std::nullopt;
    }
    return pose;
}

void Pose::reset(MotionTarget &target)
{
    for (QList<Part> &group : m_groups) {
        for (int i = 0; i < group.size(); ++i) {
            group[i].opacity = i == 0 ? 1.0 : 0.0;
            if (target.hasParameter(group[i].id))
                target.setParameter(group[i].id, i == 0 ? 1.0f : 0.0f);
            target.setPartOpacity(group[i].id, float(group[i].opacity));
            for (const QString &link : std::as_const(group[i].links))
                target.setPartOpacity(link, float(group[i].opacity));
        }
    }
}

void Pose::update(double deltaSeconds, MotionTarget &target)
{
    constexpr double kEpsilon = 0.001;
    constexpr double kPhi = 0.5;
    constexpr double kBackOpacityThreshold = 0.15;
    const double dt = std::max(0.0, deltaSeconds);
    for (QList<Part> &group : m_groups) {
        // The part whose parameter is set is shown; it fades in while the others fade out.
        int visible = -1;
        double newOpacity = 1.0;
        for (int i = 0; i < group.size(); ++i) {
            if (target.hasParameter(group[i].id) && target.parameter(group[i].id) > kEpsilon) {
                if (visible >= 0)
                    break;
                visible = i;
                newOpacity = std::min(1.0, group[i].opacity + dt / m_fadeTime);
            }
        }
        if (visible < 0) {
            visible = 0;
            newOpacity = 1.0;
        }
        for (int i = 0; i < group.size(); ++i) {
            Part &part = group[i];
            if (i == visible) {
                part.opacity = newOpacity;
                continue;
            }
            double opacity = newOpacity < kPhi ? newOpacity * (kPhi - 1.0) / kPhi + 1.0 : (1.0 - newOpacity) * kPhi / (1.0 - kPhi);
            const double backOpacity = (1.0 - opacity) * (1.0 - newOpacity);
            if (backOpacity > kBackOpacityThreshold)
                opacity = 1.0 - kBackOpacityThreshold / (1.0 - newOpacity);
            part.opacity = std::min(part.opacity, opacity);
        }
        for (const Part &part : std::as_const(group)) {
            target.setPartOpacity(part.id, float(part.opacity));
            for (const QString &link : part.links)
                target.setPartOpacity(link, float(part.opacity));
        }
    }
}

} // namespace lumen::live2d
