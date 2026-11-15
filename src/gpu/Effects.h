#pragma once

#include "gpu/Gpu.h"

#include <QFuture>
#include <QMutex>

// Effects and chains of them (plan M20 step 2). An effect is a Slang file (src/gpu/shaders/effect.slang
// says how one is written): its passes, its title and group, and its parameters with their labels,
// ranges and defaults, all from the shader. A chain runs effects over a picture on the graphics card.
namespace lumen::gpu {

// One parameter of an effect. A vector has a value per component.
struct EffectParam
{
    QString name;
    QString label;            // empty: not shown (such as "size")
    QString unit;
    QString section;          // its heading, from [Section] on it or a parameter before it
    QString control;          // how it is set, from [Control]: "" a slider, "wheel" a colour wheel (hue°, saturation %)
    QString scalar;           // "float32", "int32", "uint32"
    int components = 1;
    double minimum = 0;
    double maximum = 0;
    bool hasRange = false;
    QList<double> defaults;   // one per component

    bool shown() const { return !label.isEmpty(); }
};

class Effect
{
public:
    // The effects compiled with Lumen (":/gpu/<id>.passes").
    static QStringList builtIn();
    // A built-in effect, ready to run; null with the reason on errors.
    static Effect load(const QString &id, Context &context = Context::instance(), QString *error = nullptr);
    // What a built-in effect is (title, parameters, inputs), from its shader's reflection alone: no
    // graphics card needed, and passes() is empty. For menus and node types.
    static Effect describe(const QString &id, QString *error = nullptr);

    bool isNull() const { return m_id.isEmpty(); }
    bool canRun() const { return !m_passes.isEmpty(); }
    QString id() const { return m_id; }
    QString title() const { return m_title; }
    QString group() const { return m_group; }
    QList<EffectParam> params() const { return m_params; }
    QList<Kernel> passes() const { return m_passes; }
    // What the effect reads besides the picture, from [Input(kind)] on a texture: name → kind
    // ("curve", "lut"). Each comes as a half-float picture with the step (EffectStep::inputs).
    QHash<QString, QString> inputs() const { return m_inputs; }
    // The size of a texture the effect keeps between its passes, for a picture of `picture`:
    // [Scale(n)] on it makes it n times smaller, [Fit(n)] its long side n pixels (at most the
    // picture's), [Size(w, h)] a fixed size; else the picture's size.
    QSize textureSize(const QString &name, const QSize &picture) const;
    // What a pass runs over: [Over("name")] on its entry, a texture of the effect's; else the picture.
    QSize passSize(int pass, const QSize &picture) const;

    // Every shown parameter at its default.
    QVariantMap defaults() const;
    // `values` over the defaults, numbers kept in range; an unknown name or a wrong count of values
    // is an error.
    QVariantMap resolve(const QVariantMap &values, QString *error = nullptr) const;

private:
    QString m_id;
    QString m_title;
    QString m_group;
    QList<EffectParam> m_params;
    QList<Kernel> m_passes;
    QStringList m_entries;
    QList<KernelInterface> m_interfaces; // one per pass, from the reflection
    QHash<QString, QString> m_inputs;
    QHash<QString, QHash<QString, QVariantList>> m_textures; // [Scale] and [Size] by texture
};

// One effect in a chain, with its values.
struct EffectStep
{
    QString effect;
    QVariantMap values;
    bool enabled = true;
    QHash<QString, QImage> inputs; // the effect's inputs (Effect::inputs), half-float pictures
};

// Runs effects one after another over a picture: up to the graphics card, into linear light, through
// the effects, back to 8-bit sRGB with dithering. It keeps its textures between runs: the same picture
// again (a slider moved) skips the upload. One chain per use, so a preview at screen size and the
// full size do not take each other's textures. Not for two threads at once, except through
// renderLater(), which takes turns.
class Chain
{
public:
    explicit Chain(Context &context = Context::instance());
    ~Chain();

    // Loads the steps' effects; false with the reason when one is unknown or its values are wrong.
    bool setSteps(const QList<EffectStep> &steps, QString *error = nullptr);
    QList<EffectStep> steps() const;

    // The picture with the effects, RGBA8888 with straight alpha, the picture's size; null on errors.
    QImage render(const QImage &picture);
    // The same on a worker thread (a full-size result while a preview is shown).
    QFuture<QImage> renderLater(const QImage &picture);

    QString error() const;
    // Whether the last render could use the picture already on the graphics card.
    bool reusedPicture() const;
    double gpuMilliseconds() const;

private:
    struct Prepared
    {
        Effect effect;
        QVariantMap values;
        QHash<QString, QImage> inputs;
    };

    Context &m_context;
    mutable QMutex m_lock;
    QList<EffectStep> m_steps;
    QList<Prepared> m_prepared;
    QString m_error;
    bool m_reused = false;
    double m_gpuMs = 0;
    // On the graphics card between runs.
    qint64 m_pictureKey = 0;
    QSize m_size;
    Texture m_base;     // the picture in linear light
    Texture m_ping;
    Texture m_pong;
    Texture m_output;   // 8-bit, for reading back
    QHash<QString, Texture> m_scratch;
    struct Input
    {
        qint64 key = 0;
        Texture texture;
    };
    QHash<QString, Input> m_inputs; // sent once, again when the picture changes
};

} // namespace lumen::gpu
