#pragma once

#include <QJsonObject>
#include <QList>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

namespace lumen::live2d {

// Cubism physics (.physics3.json): every setting is a strand of particles (a pendulum chain)
// driven by normalised input parameters (translation X/Y and angle); the motion of chosen
// particles is written back to output parameters (hair, ribbons, accessories...).
// Follows the Cubism Framework's algorithm: fixed-step integration at the file's FPS with
// interpolation of the outputs between steps.
class Physics
{
public:
    enum class Kind { X, Y, Angle };

    struct Range
    {
        double minimum = -10.0, defaultValue = 0.0, maximum = 10.0;
    };
    struct Input
    {
        QString id;
        Kind kind = Kind::X;
        double weight = 100.0;
        bool reflect = false;
        int index = -1;
    };
    struct Output
    {
        QString id;
        Kind kind = Kind::Angle;
        int vertex = 1;
        double scale = 1.0;
        double weight = 100.0;
        bool reflect = false;
        int index = -1;
    };
    struct Particle
    {
        double mobility = 1.0, delay = 1.0, acceleration = 1.0, radius = 0.0;
        QPointF initial, position, lastPosition, lastGravity{0.0, 1.0}, velocity, force;
    };
    struct Setting
    {
        QString id;
        QString name;
        QList<Input> inputs;
        QList<Output> outputs;
        QVector<Particle> particles;
        Range position, angle;
        QVector<double> currentOutputs, previousOutputs;
    };

    static std::optional<Physics> load(const QString &path, QString *error = nullptr);
    static std::optional<Physics> fromJson(const QJsonObject &json, QString *error = nullptr);

    // Resolves parameter ids to indices (unknown ids are ignored) and resets the strands.
    void bind(const QStringList &parameterIds);
    void reset();
    void setWind(const QPointF &wind) { m_wind = wind; }
    // Advances the simulation by `seconds` and writes the outputs into `values` (in place).
    void evaluate(float *values, const float *minimums, const float *maximums, const float *defaults, double seconds);
    // Runs the simulation for a while with the current values so a model starts at rest.
    void stabilize(float *values, const float *minimums, const float *maximums, const float *defaults);

    const QList<Setting> &settings() const { return m_settings; }
    QPointF gravity() const { return m_gravity; }
    QPointF wind() const { return m_wind; }
    double fps() const { return m_fps; }
    int boundOutputs() const;

    // Maps a parameter value onto the normalised range (Cubism's NormalizeParameterValue).
    static double normalize(double value, double parameterMinimum, double parameterMaximum, double normalizedMinimum,
                            double normalizedMaximum, double normalizedDefault, bool inverted);
    // Signed angle (radians) from one direction to another, in (-pi, pi].
    static double directionToRadian(const QPointF &from, const QPointF &to);

private:
    void step(const float *minimums, const float *maximums, const float *defaults, double dt);
    void writeOutputs(float *values, const float *minimums, const float *maximums, double alpha) const;

    QList<Setting> m_settings;
    QPointF m_gravity{0.0, -1.0};
    QPointF m_wind;
    double m_fps = 0.0;
    double m_remaining = 0.0;
    QVector<float> m_cache;      // parameter values seen by the simulation (interpolated inputs)
    QVector<float> m_inputCache;
    int m_parameterCount = 0;
};

} // namespace lumen::live2d
