#include "live2d/Physics.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>

#include <cmath>

namespace lumen::live2d {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMaximumWeight = 100.0;
constexpr double kAirResistance = 5.0;
constexpr double kMovementThreshold = 0.001;
constexpr double kMaxDeltaTime = 5.0;

double degreesToRadians(double d)
{
    return d * kPi / 180.0;
}

QPointF normalized(const QPointF &v)
{
    const double len = std::hypot(v.x(), v.y());
    return len > 0.0 ? v / len : QPointF();
}

Physics::Kind kindFromName(const QString &name)
{
    if (name == QLatin1String("Y"))
        return Physics::Kind::Y;
    if (name == QLatin1String("Angle"))
        return Physics::Kind::Angle;
    return Physics::Kind::X;
}

Physics::Range rangeFromJson(const QJsonObject &o)
{
    Physics::Range r;
    r.minimum = o.value(QStringLiteral("Minimum")).toDouble(r.minimum);
    r.defaultValue = o.value(QStringLiteral("Default")).toDouble(r.defaultValue);
    r.maximum = o.value(QStringLiteral("Maximum")).toDouble(r.maximum);
    return r;
}

QPointF vectorFromJson(const QJsonValue &v, const QPointF &fallback)
{
    if (!v.isObject())
        return fallback;
    const QJsonObject o = v.toObject();
    return QPointF(o.value(QStringLiteral("X")).toDouble(fallback.x()), o.value(QStringLiteral("Y")).toDouble(fallback.y()));
}

// Writes an output into a parameter: scale, clamp to the parameter range, blend by weight.
void applyOutput(float &parameter, double minimum, double maximum, double outputValue, const Physics::Output &o)
{
    double value = std::clamp(outputValue * o.scale, std::min(minimum, maximum), std::max(minimum, maximum));
    const double weight = o.weight / kMaximumWeight;
    if (weight < 1.0)
        value = parameter * (1.0 - weight) + value * weight;
    parameter = float(value);
}

// One integration step of a strand (Cubism's UpdateParticles).
void updateParticles(QVector<Physics::Particle> &strand, const QPointF &totalTranslation, double totalAngle, const QPointF &wind,
                     double dt)
{
    if (strand.isEmpty())
        return;
    strand[0].position = totalTranslation;
    const double totalRadian = degreesToRadians(totalAngle);
    const QPointF gravity = normalized(QPointF(std::sin(totalRadian), std::cos(totalRadian)));
    for (int i = 1; i < strand.size(); ++i) {
        Physics::Particle &p = strand[i];
        const Physics::Particle &parent = strand[i - 1];
        p.force = gravity * p.acceleration + wind;
        p.lastPosition = p.position;
        const double delay = p.delay * dt * 30.0;
        QPointF direction = p.position - parent.position;
        const double radian = Physics::directionToRadian(p.lastGravity, gravity) / kAirResistance;
        // Same evaluation order as the Cubism Framework (the y term uses the rotated x).
        const double dx = std::cos(radian) * direction.x() - direction.y() * std::sin(radian);
        const double dy = std::sin(radian) * dx + direction.y() * std::cos(radian);
        direction = QPointF(dx, dy);
        p.position = parent.position + direction;
        const QPointF velocity = p.velocity * delay;
        const QPointF force = p.force * delay * delay;
        p.position += velocity + force;
        const QPointF newDirection = normalized(p.position - parent.position);
        p.position = parent.position + newDirection * p.radius;
        if (std::abs(p.position.x()) < kMovementThreshold)
            p.position.setX(0.0);
        if (delay != 0.0)
            p.velocity = (p.position - p.lastPosition) / delay * p.mobility;
        p.force = QPointF();
        p.lastGravity = gravity;
    }
}

} // namespace

double Physics::normalize(double value, double parameterMinimum, double parameterMaximum, double normalizedMinimum,
                          double normalizedMaximum, double normalizedDefault, bool inverted)
{
    const double maxValue = std::max(parameterMaximum, parameterMinimum);
    const double minValue = std::min(parameterMaximum, parameterMinimum);
    value = std::clamp(value, minValue, maxValue);
    const double minNorm = std::min(normalizedMinimum, normalizedMaximum);
    const double maxNorm = std::max(normalizedMinimum, normalizedMaximum);
    const double middleNorm = normalizedDefault;
    const double middleValue = minValue + (maxValue - minValue) / 2.0;
    const double paramValue = value - middleValue;
    double result = middleNorm;
    if (paramValue > 0) {
        const double pLength = maxValue - middleValue;
        if (pLength != 0.0)
            result = paramValue * ((maxNorm - middleNorm) / pLength) + middleNorm;
    } else if (paramValue < 0) {
        const double pLength = minValue - middleValue;
        if (pLength != 0.0)
            result = paramValue * ((minNorm - middleNorm) / pLength) + middleNorm;
    }
    return inverted ? result : -result;
}

double Physics::directionToRadian(const QPointF &from, const QPointF &to)
{
    double ret = std::atan2(to.y(), to.x()) - std::atan2(from.y(), from.x());
    while (ret < -kPi)
        ret += 2.0 * kPi;
    while (ret > kPi)
        ret -= 2.0 * kPi;
    return ret;
}

std::optional<Physics> Physics::load(const QString &path, QString *error)
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
    return fromJson(doc.object(), error);
}

std::optional<Physics> Physics::fromJson(const QJsonObject &json, QString *error)
{
    Physics physics;
    const QJsonObject meta = json.value(QStringLiteral("Meta")).toObject();
    const QJsonObject forces = meta.value(QStringLiteral("EffectiveForces")).toObject();
    physics.m_gravity = vectorFromJson(forces.value(QStringLiteral("Gravity")), QPointF(0.0, -1.0));
    physics.m_wind = vectorFromJson(forces.value(QStringLiteral("Wind")), QPointF());
    physics.m_fps = meta.value(QStringLiteral("Fps")).toDouble(0.0);
    QHash<QString, QString> names;
    for (const QJsonValue &v : meta.value(QStringLiteral("PhysicsDictionary")).toArray())
        names.insert(v.toObject().value(QStringLiteral("Id")).toString(), v.toObject().value(QStringLiteral("Name")).toString());

    const QJsonArray settings = json.value(QStringLiteral("PhysicsSettings")).toArray();
    if (settings.isEmpty()) {
        if (error)
            *error = QObject::tr("The physics file has no PhysicsSettings.");
        return std::nullopt;
    }
    for (const QJsonValue &sv : settings) {
        const QJsonObject so = sv.toObject();
        Setting s;
        s.id = so.value(QStringLiteral("Id")).toString();
        s.name = names.value(s.id, s.id);
        for (const QJsonValue &iv : so.value(QStringLiteral("Input")).toArray()) {
            const QJsonObject io = iv.toObject();
            const QJsonObject source = io.value(QStringLiteral("Source")).toObject();
            if (source.value(QStringLiteral("Target")).toString(QStringLiteral("Parameter")) != QLatin1String("Parameter"))
                continue;
            Input in;
            in.id = source.value(QStringLiteral("Id")).toString();
            in.kind = kindFromName(io.value(QStringLiteral("Type")).toString());
            in.weight = io.value(QStringLiteral("Weight")).toDouble(100.0);
            in.reflect = io.value(QStringLiteral("Reflect")).toBool(false);
            s.inputs.append(in);
        }
        for (const QJsonValue &ov : so.value(QStringLiteral("Output")).toArray()) {
            const QJsonObject oo = ov.toObject();
            const QJsonObject destination = oo.value(QStringLiteral("Destination")).toObject();
            if (destination.value(QStringLiteral("Target")).toString(QStringLiteral("Parameter")) != QLatin1String("Parameter"))
                continue;
            Output out;
            out.id = destination.value(QStringLiteral("Id")).toString();
            out.kind = kindFromName(oo.value(QStringLiteral("Type")).toString());
            out.vertex = oo.value(QStringLiteral("VertexIndex")).toInt(1);
            out.scale = oo.value(QStringLiteral("Scale")).toDouble(1.0);
            out.weight = oo.value(QStringLiteral("Weight")).toDouble(100.0);
            out.reflect = oo.value(QStringLiteral("Reflect")).toBool(false);
            s.outputs.append(out);
        }
        for (const QJsonValue &vv : so.value(QStringLiteral("Vertices")).toArray()) {
            const QJsonObject vo = vv.toObject();
            Particle p;
            p.mobility = vo.value(QStringLiteral("Mobility")).toDouble(1.0);
            p.delay = vo.value(QStringLiteral("Delay")).toDouble(1.0);
            p.acceleration = vo.value(QStringLiteral("Acceleration")).toDouble(1.0);
            p.radius = vo.value(QStringLiteral("Radius")).toDouble(0.0);
            s.particles.append(p);
        }
        if (s.particles.size() < 2) {
            if (error)
                *error = QObject::tr("Physics setting %1 needs at least two vertices.").arg(s.id);
            return std::nullopt;
        }
        const QJsonObject norm = so.value(QStringLiteral("Normalization")).toObject();
        s.position = rangeFromJson(norm.value(QStringLiteral("Position")).toObject());
        s.angle = rangeFromJson(norm.value(QStringLiteral("Angle")).toObject());
        s.currentOutputs.fill(0.0, s.outputs.size());
        s.previousOutputs.fill(0.0, s.outputs.size());
        physics.m_settings.append(s);
    }
    physics.reset();
    return physics;
}

void Physics::bind(const QStringList &parameterIds)
{
    m_parameterCount = int(parameterIds.size());
    for (Setting &s : m_settings) {
        for (Input &in : s.inputs)
            in.index = int(parameterIds.indexOf(in.id));
        for (Output &out : s.outputs)
            out.index = int(parameterIds.indexOf(out.id));
    }
    reset();
}

int Physics::boundOutputs() const
{
    int n = 0;
    for (const Setting &s : m_settings) {
        for (const Output &o : s.outputs)
            n += o.index >= 0;
    }
    return n;
}

void Physics::reset()
{
    for (Setting &s : m_settings) {
        // The strand hangs straight down from the origin; positions come from the radii.
        for (int i = 0; i < s.particles.size(); ++i) {
            Particle &p = s.particles[i];
            p.initial = i == 0 ? QPointF() : s.particles[i - 1].initial + QPointF(0.0, p.radius);
            p.position = p.lastPosition = p.initial;
            p.lastGravity = QPointF(0.0, 1.0);
            p.velocity = p.force = QPointF();
        }
        s.currentOutputs.fill(0.0, s.outputs.size());
        s.previousOutputs.fill(0.0, s.outputs.size());
    }
    m_remaining = 0.0;
    m_cache.clear();
    m_inputCache.clear();
}

void Physics::step(const float *minimums, const float *maximums, const float *defaults, double dt)
{
    Q_UNUSED(defaults);
    for (Setting &s : m_settings) {
        QPointF translation;
        double angle = 0.0;
        for (const Input &in : std::as_const(s.inputs)) {
            if (in.index < 0 || in.index >= m_cache.size())
                continue;
            const double weight = in.weight / kMaximumWeight;
            const double v = m_cache[in.index];
            const Range &range = in.kind == Kind::Angle ? s.angle : s.position;
            const double n = normalize(v, minimums[in.index], maximums[in.index], range.minimum, range.maximum, range.defaultValue,
                                       in.reflect) * weight;
            if (in.kind == Kind::X)
                translation.rx() += n;
            else if (in.kind == Kind::Y)
                translation.ry() += n;
            else
                angle += n;
        }
        const double radian = degreesToRadians(-angle);
        const double tx = translation.x() * std::cos(radian) - translation.y() * std::sin(radian);
        const double ty = tx * std::sin(radian) + translation.y() * std::cos(radian);
        updateParticles(s.particles, QPointF(tx, ty), angle, m_wind, dt);

        for (int k = 0; k < s.outputs.size(); ++k) {
            const Output &o = s.outputs[k];
            if (o.vertex < 1 || o.vertex >= s.particles.size())
                continue;
            const QPointF delta = s.particles[o.vertex].position - s.particles[o.vertex - 1].position;
            double value = 0.0;
            switch (o.kind) {
            case Kind::X:
                value = delta.x();
                break;
            case Kind::Y:
                value = delta.y();
                break;
            case Kind::Angle: {
                const QPointF parent = o.vertex >= 2 ? s.particles[o.vertex - 1].position - s.particles[o.vertex - 2].position : -m_gravity;
                value = directionToRadian(parent, delta);
                break;
            }
            }
            if (o.reflect)
                value = -value;
            s.currentOutputs[k] = value;
            // Later settings may read this output as an input within the same step.
            if (o.index >= 0 && o.index < m_cache.size())
                applyOutput(m_cache[o.index], minimums[o.index], maximums[o.index], value, o);
        }
    }
}

void Physics::writeOutputs(float *values, const float *minimums, const float *maximums, double alpha) const
{
    for (const Setting &s : m_settings) {
        for (int k = 0; k < s.outputs.size(); ++k) {
            const Output &o = s.outputs[k];
            if (o.index < 0 || o.index >= m_parameterCount)
                continue;
            const double v = s.previousOutputs[k] * (1.0 - alpha) + s.currentOutputs[k] * alpha;
            applyOutput(values[o.index], minimums[o.index], maximums[o.index], v, o);
        }
    }
}

void Physics::evaluate(float *values, const float *minimums, const float *maximums, const float *defaults, double seconds)
{
    if (seconds <= 0.0 || m_settings.isEmpty() || m_parameterCount <= 0)
        return;
    if (m_cache.size() != m_parameterCount) {
        m_cache = QVector<float>(values, values + m_parameterCount);
        m_inputCache = m_cache;
    }
    m_remaining += seconds;
    if (m_remaining > kMaxDeltaTime)
        m_remaining = 0.0;
    const double dt = m_fps > 0.0 ? 1.0 / m_fps : seconds;
    while (m_remaining >= dt) {
        for (Setting &s : m_settings)
            s.previousOutputs = s.currentOutputs;
        // Inputs are interpolated between the previous and the current frame.
        const double inputWeight = dt / m_remaining;
        for (int j = 0; j < m_parameterCount; ++j) {
            m_cache[j] = float(m_inputCache[j] * (1.0 - inputWeight) + values[j] * inputWeight);
            m_inputCache[j] = m_cache[j];
        }
        step(minimums, maximums, defaults, dt);
        m_remaining -= dt;
    }
    writeOutputs(values, minimums, maximums, std::clamp(m_remaining / dt, 0.0, 1.0));
}

void Physics::stabilize(float *values, const float *minimums, const float *maximums, const float *defaults)
{
    if (m_parameterCount <= 0)
        return;
    reset();
    const QVector<float> input(values, values + m_parameterCount);
    const double dt = m_fps > 0.0 ? 1.0 / m_fps : 1.0 / 60.0;
    QVector<float> scratch;
    for (int i = 0; i < int(3.0 / dt); ++i) {
        scratch = input;
        evaluate(scratch.data(), minimums, maximums, defaults, dt);
    }
    std::copy(scratch.cbegin(), scratch.cend(), values);
}

} // namespace lumen::live2d
