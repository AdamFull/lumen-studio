#pragma once

#include <QColor>
#include <QFuture>
#include <QHash>
#include <QImage>
#include <QList>
#include <QSize>
#include <QString>
#include <QVariant>
#include <QVariantMap>

#include <functional>
#include <memory>
#include <optional>

// Node filter graphs (M17 §17.6): typed ports, node type descriptors and the evaluation interface
// that node types implement. No UI dependency; node types run on worker threads.
namespace lumen::filters {

// What flows along a link or is stored in a parameter. Images are ARGB32_Premultiplied and masks
// Grayscale8 (255 = selected), as in core/ImageOps; numbers are doubles, colours QColor, text and
// choices QString (a choice holds the id of one of its options).
enum class PortType { Image, Mask, Number, Color, Text, Choice };

QString portTypeId(PortType type); // "image", "mask", ... (stable; used in files)
std::optional<PortType> portTypeFromId(const QString &id);
// Plain words for messages: "an image", "a mask", ...
QString portTypeName(PortType type);
// Whether an output of type `from` can feed an input of type `to`: the same type, or a mask into
// an image input (it arrives as a grey image).
bool canConnect(PortType from, PortType to);

struct PortSpec
{
    QString id;    // stable; stored in links
    QString label; // shown in the node editor
    PortType type = PortType::Image;
    bool optional = false; // an optional input may stay unconnected
};

struct ChoiceOption
{
    QString id;    // stable; stored in files
    QString label;
};

struct ParamSpec
{
    QString id;    // stable; stored in files
    QString label; // shown next to the control
    PortType type = PortType::Number; // Number, Color, Text or Choice
    QVariant defaultValue;
    double minimum = 0.0;  // numbers only
    double maximum = 1.0;
    int decimals = 0;      // 0: whole numbers (values are rounded)
    QString unit;          // shown after the value ("px", "%", "°")
    // A length in full-resolution pixels: the evaluator multiplies it by the preview scale, so a
    // preview at half size blurs by half the radius and looks like the full-size result.
    bool pixels = false;
    QList<ChoiceOption> choices; // choices only; the default is one of their ids
    QString editor;              // text only: an editor hint ("curve": a list of "x y" points)
    QString help;                // one sentence in plain words (tooltip)
};

struct NodeTypeInfo
{
    QString id;          // stable; stored in files ("levels", "gaussian-blur")
    QString name;        // shown ("Levels")
    QString category;    // category id; categoryName() gives the label
    QString description; // one sentence in plain words
    QList<PortSpec> inputs;
    QList<PortSpec> outputs;
    QList<ParamSpec> params;
    bool readsSource = false; // reads the evaluation's source image and selection (the input node)
    bool isOutput = false;    // the graph's result; a valid graph has exactly one
    // Takes a queue job (the AI nodes of a later step): a graph with one previews only after an
    // explicit Run (EvalOptions::runExpensive), and its result is then reused from the cache.
    bool expensive = false;

    const PortSpec *input(const QString &id) const;
    const PortSpec *output(const QString &id) const;
    const ParamSpec *param(const QString &id) const;
};

// Port id -> value: the inputs given to a node, or the outputs it returns.
using Values = QHash<QString, QVariant>;

// A stored parameter value checked against its spec: a number clamped to its range (and rounded
// when it has no decimals), an unknown choice replaced by the default, a colour parsed from its
// "#aarrggbb" form. A missing value gives the default.
QVariant coerceParam(const ParamSpec &spec, const QVariant &stored);
// Every parameter of `info` from the stored values, coerced, with pixel lengths multiplied by
// `scale`. Values without a spec are dropped.
QVariantMap resolveParams(const NodeTypeInfo &info, const QVariantMap &stored, double scale);

// What a node reads when it is evaluated: its connected inputs and its parameters.
class NodeArgs
{
public:
    NodeArgs() = default;
    NodeArgs(Values inputs, QVariantMap params);

    bool isConnected(const QString &port) const { return m_inputs.contains(port); }
    QImage image(const QString &port) const; // null when not connected
    QImage mask(const QString &port) const;
    // A connected input of that id, else the parameter: a value port can drive a parameter.
    QVariant value(const QString &id) const;
    double number(const QString &id) const;
    int integer(const QString &id) const; // rounded
    QColor color(const QString &id) const;
    QString text(const QString &id) const; // also the id of a choice

    const Values &inputs() const { return m_inputs; }
    const QVariantMap &params() const { return m_params; }

private:
    Values m_inputs;
    QVariantMap m_params;
};

// The evaluation that runs a node, as the node sees it. Valid only during NodeType::evaluate();
// an asynchronous node copies what it needs before it returns.
class EvalContext
{
public:
    // Implemented by the evaluator (and by evaluateNode()).
    class Run
    {
    public:
        virtual ~Run() = default;
        virtual bool isCancelled() const = 0;
        virtual void onCancel(std::function<void()> handler) = 0;
        virtual QImage source() = 0;    // at the evaluation's scale
        virtual QImage selection() = 0; // at the evaluation's scale; all 255 without a selection
        virtual QSize sourceSize() const = 0; // full resolution
    };

    EvalContext(Run *run, QString nodeId, double scale);

    QString nodeId() const { return m_nodeId; }
    // Preview scale in (0, 1]; 1 when the filter is applied. Pixel-length parameters arrive
    // already scaled.
    double scale() const { return m_scale; }
    bool isPreview() const { return m_scale < 1.0; }
    // Long loops check it now and then and return early (the result is then discarded).
    bool isCancelled() const;
    // Runs `handler` once if the evaluation is cancelled while this node runs or waits for its
    // future (an AI node cancels its job); at once when it is already cancelled. Any thread.
    void onCancel(std::function<void()> handler);
    // The image the filter is applied to and the selection, at the evaluation's scale.
    QImage source() const;
    QImage selection() const;
    QSize sourceSize() const;
    // Checks for cancellation as a callable, for the pixel operations in Ops.h.
    std::function<bool()> cancelCheck() const;

private:
    Run *m_run = nullptr;
    QString m_nodeId;
    double m_scale = 1.0;
};

// The outputs of one evaluation of a node: ready at once, or delivered later by a future. An AI
// node submits its job and returns the job's future without waiting; the evaluator continues on a
// worker thread when the future finishes, so no thread blocks in between. A node fails by
// throwing lumen::Error (its message is shown with the node's name) or by an exception in the
// future; a cancelled future cancels the evaluation.
class NodeResult
{
public:
    NodeResult(Values outputs) : m_outputs(std::move(outputs)), m_ready(true) {}
    NodeResult(QFuture<Values> pending) : m_pending(std::move(pending)) {}

    bool isReady() const { return m_ready; }
    const Values &outputs() const { return m_outputs; }
    QFuture<Values> future() const { return m_pending; }

private:
    Values m_outputs;
    QFuture<Values> m_pending;
    bool m_ready = false;
};

// A kind of node. One object serves every node of its type, in every graph and on several worker
// threads at once, so implementations keep no mutable state.
class NodeType
{
public:
    explicit NodeType(NodeTypeInfo info);
    virtual ~NodeType();
    NodeType(const NodeType &) = delete;
    NodeType &operator=(const NodeType &) = delete;

    const NodeTypeInfo &info() const { return m_info; }
    QString id() const { return m_info.id; }

    // Computes the outputs (by output port id) from the connected inputs and the parameters
    // (coerced to their specs, pixel lengths scaled). Required inputs are always connected; image
    // and mask inputs arrive in the canonical formats and may differ in size (a node stretches a
    // mask to its image). Called on a worker thread.
    virtual NodeResult evaluate(const NodeArgs &args, EvalContext &ctx) const = 0;

private:
    NodeTypeInfo m_info;
};

using NodeTypePtr = std::shared_ptr<const NodeType>;
using NodeFunction = std::function<NodeResult(const NodeArgs &, EvalContext &)>;

// A node type that runs a function (the classic nodes; tests).
NodeTypePtr makeNodeType(NodeTypeInfo info, NodeFunction function);

} // namespace lumen::filters
