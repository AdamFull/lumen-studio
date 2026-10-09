#pragma once

#include "live2d/Motion.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>
#include <optional>

// Cubism motion playback, expressions and part poses, following the Cubism Framework's
// behaviour (sine-eased fades, crossfading motions, expression blend modes, pose part switching).
namespace lumen::live2d {

// What animations write to (a model, or a plain map in tests).
class MotionTarget
{
public:
    virtual ~MotionTarget() = default;
    virtual bool hasParameter(const QString &id) const = 0;
    virtual float parameter(const QString &id) const = 0;
    virtual void setParameter(const QString &id, float value) = 0;
    virtual void setPartOpacity(const QString &id, float value) = 0;
    virtual QStringList eyeBlinkIds() const { return {}; }
    virtual QStringList lipSyncIds() const { return {}; }
};

// Cubism's fade curve: 0 below 0, 1 above 1, half a cosine between.
double easeSine(double v);

class MotionPlayer
{
public:
    enum class Priority { None = 0, Idle = 1, Normal = 2, Force = 3 };

    struct Options
    {
        QString group;
        int index = -1;
        bool loop = false;
        double fadeIn = -1.0;  // < 0: the motion's value, else 1 s (Cubism default)
        double fadeOut = -1.0;
        Priority priority = Priority::Normal;
    };

    // Starts a motion at `now`; motions already playing fade out. Returns false when a playing
    // motion has a higher priority (Force always plays).
    bool play(std::shared_ptr<const Motion> motion, double now, const Options &options);
    // Fades every motion out.
    void stop(double now);
    // Applies the motions at `now` and drops the finished ones.
    void update(double now, MotionTarget &target);

    // The newest motion is still running (not finished and not fading out because of stop()).
    bool isPlaying(double now) const;
    QString currentGroup() const { return m_entries.isEmpty() ? QString() : m_entries.last().options.group; }
    int currentIndex() const { return m_entries.isEmpty() ? -1 : m_entries.last().options.index; }
    Priority currentPriority(double now) const;
    int activeCount() const { return int(m_entries.size()); }

private:
    struct Entry
    {
        std::shared_ptr<const Motion> motion;
        Options options;
        double start = 0.0;
        double fadeIn = 1.0;
        double fadeOut = 1.0;
        double end = -1.0;     // < 0: no end (looping)
        bool stopped = false;  // fading out because another motion started or stop() was called
    };
    QList<Entry> m_entries;
};

// An expression (.exp3.json): parameter offsets applied on top of motions.
struct Expression
{
    enum class Blend { Add, Multiply, Overwrite };
    struct Parameter
    {
        QString id;
        double value = 0.0;
        Blend blend = Blend::Add;
    };
    QString name;
    double fadeIn = 1.0;
    double fadeOut = 1.0;
    QList<Parameter> parameters;

    static std::optional<Expression> load(const QString &path, const QString &name, QString *error = nullptr);
    static std::optional<Expression> fromJson(const QJsonObject &json, const QString &name, QString *error = nullptr);
};

class ExpressionPlayer
{
public:
    // Crossfades to `expression` (null clears the expression with a fade out).
    void set(std::shared_ptr<const Expression> expression, double now);
    void update(double now, MotionTarget &target);
    QString current() const;
    int activeCount() const { return int(m_active.size()); }

private:
    struct Active
    {
        std::shared_ptr<const Expression> expression;
        double start = 0.0;
        double end = -1.0;
    };
    QList<Active> m_active;
};

// Part pose (.pose3.json): in each group exactly one part is shown, chosen by the parameter that
// has the part's id; switching fades between the parts. Linked parts follow their group part.
class Pose
{
public:
    struct Part
    {
        QString id;
        QStringList links;
        double opacity = 0.0;
    };

    static std::optional<Pose> load(const QString &path, QString *error = nullptr);
    static std::optional<Pose> fromJson(const QJsonObject &json, QString *error = nullptr);

    // Shows the first part of each group (and sets its parameter) without fading.
    void reset(MotionTarget &target);
    void update(double deltaSeconds, MotionTarget &target);
    const QList<QList<Part>> &groups() const { return m_groups; }
    double fadeTime() const { return m_fadeTime; }

private:
    QList<QList<Part>> m_groups;
    double m_fadeTime = 0.5;
};

} // namespace lumen::live2d
