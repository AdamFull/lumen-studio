#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace lumen::live2d {

// Cubism model settings (.model3.json).
struct ModelSettings
{
    struct MotionRef
    {
        QString file;          // absolute .motion3.json path
        double fadeIn = -1.0;  // seconds; < 0 = the motion's own value
        double fadeOut = -1.0;
        QString sound;         // absolute path of an optional sound file
    };
    struct ExpressionRef
    {
        QString name;
        QString file;          // absolute .exp3.json path
    };

    QString directory;     // folder of the .model3.json (relative paths resolve against it)
    QString moc;           // absolute path of the .moc3
    QStringList textures;  // absolute paths
    QString physics;       // absolute path or empty
    QString pose;          // absolute path of the .pose3.json or empty
    QHash<QString, QList<MotionRef>> motions; // group -> motions
    QList<ExpressionRef> expressions;
    QStringList eyeBlinkIds;
    QStringList lipSyncIds;

    // Motion groups, "Idle" first then alphabetical.
    QStringList motionGroups() const;
    static bool isIdleGroup(const QString &group);

    static std::optional<ModelSettings> load(const QString &path, QString *error = nullptr);
    static std::optional<ModelSettings> fromJson(const QJsonObject &json, const QString &directory, QString *error = nullptr);
    // The idle motion if the model has one ("Idle" group, case-insensitive), else empty.
    QString idleMotion() const;
};

// One animation curve of a .motion3.json.
struct MotionCurve
{
    enum class Target { Parameter, PartOpacity, Model };
    enum class SegmentType { Linear = 0, Bezier = 1, Stepped = 2, InverseStepped = 3 };
    struct Point
    {
        double time;
        double value;
    };
    struct Segment
    {
        SegmentType type;
        Point p0, p1, p2, p3; // linear/stepped use p0 -> p1, bezier p0..p3
    };

    Target target = Target::Parameter;
    QString id;
    double fadeIn = -1.0;
    double fadeOut = -1.0;
    QList<Segment> segments;

    double valueAt(double time) const;
};

// A Cubism motion (.motion3.json): parameter and part-opacity curves over time.
class Motion
{
public:
    static std::optional<Motion> load(const QString &path, QString *error = nullptr);
    static std::optional<Motion> fromJson(const QJsonObject &json, QString *error = nullptr);

    double duration() const { return m_duration; }
    bool loops() const { return m_loop; }
    double fps() const { return m_fps; }
    double fadeIn() const { return m_fadeIn; }   // < 0 when the file does not set it
    double fadeOut() const { return m_fadeOut; }
    const QList<MotionCurve> &curves() const { return m_curves; }

    // Evaluates every curve at `time` (wrapped when looping, clamped otherwise).
    void evaluate(double time, const std::function<void(MotionCurve::Target, const QString &id, double value)> &sink) const;

private:
    double m_duration = 0.0;
    double m_fps = 30.0;
    bool m_loop = true;
    double m_fadeIn = -1.0;
    double m_fadeOut = -1.0;
    QList<MotionCurve> m_curves;
};

} // namespace lumen::live2d
