#include "filters/NodeType.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>

namespace lumen::filters {

namespace {

struct Text
{
    Q_DECLARE_TR_FUNCTIONS(lumen::filters::Graph)
};

} // namespace

QString portTypeId(PortType type)
{
    switch (type) {
    case PortType::Image: return QStringLiteral("image");
    case PortType::Mask: return QStringLiteral("mask");
    case PortType::Number: return QStringLiteral("number");
    case PortType::Color: return QStringLiteral("color");
    case PortType::Text: return QStringLiteral("text");
    case PortType::Choice: return QStringLiteral("choice");
    }
    return {};
}

std::optional<PortType> portTypeFromId(const QString &id)
{
    for (PortType t : {PortType::Image, PortType::Mask, PortType::Number, PortType::Color, PortType::Text,
                       PortType::Choice}) {
        if (portTypeId(t) == id)
            return t;
    }
    return std::nullopt;
}

QString portTypeName(PortType type)
{
    switch (type) {
    case PortType::Image: return Text::tr("an image");
    case PortType::Mask: return Text::tr("a mask");
    case PortType::Number: return Text::tr("a number");
    case PortType::Color: return Text::tr("a colour");
    case PortType::Text: return Text::tr("text");
    case PortType::Choice: return Text::tr("a choice");
    }
    return {};
}

bool canConnect(PortType from, PortType to)
{
    return from == to || (from == PortType::Mask && to == PortType::Image);
}

const PortSpec *NodeTypeInfo::input(const QString &id) const
{
    for (const PortSpec &p : inputs) {
        if (p.id == id)
            return &p;
    }
    return nullptr;
}

const PortSpec *NodeTypeInfo::output(const QString &id) const
{
    for (const PortSpec &p : outputs) {
        if (p.id == id)
            return &p;
    }
    return nullptr;
}

const ParamSpec *NodeTypeInfo::param(const QString &id) const
{
    for (const ParamSpec &p : params) {
        if (p.id == id)
            return &p;
    }
    return nullptr;
}

QVariant coerceParam(const ParamSpec &spec, const QVariant &stored)
{
    const QVariant v = stored.isValid() ? stored : spec.defaultValue;
    switch (spec.type) {
    case PortType::Number: {
        bool ok = false;
        double d = v.toDouble(&ok);
        if (!ok || !std::isfinite(d))
            d = spec.defaultValue.toDouble();
        d = std::clamp(d, spec.minimum, spec.maximum);
        if (spec.decimals == 0)
            d = std::round(d);
        return d;
    }
    case PortType::Color: {
        QColor c = v.typeId() == QMetaType::QColor ? v.value<QColor>() : QColor::fromString(v.toString());
        if (!c.isValid())
            c = spec.defaultValue.value<QColor>();
        return c;
    }
    case PortType::Choice: {
        const QString id = v.toString();
        for (const ChoiceOption &o : spec.choices) {
            if (o.id == id)
                return id;
        }
        return spec.defaultValue.toString();
    }
    case PortType::Text:
        return v.toString();
    case PortType::Image:
    case PortType::Mask:
        break;
    }
    return v;
}

QVariant resolveParam(const ParamSpec &spec, const QVariant &stored, double scale)
{
    const QVariant v = coerceParam(spec, stored);
    if (spec.pixels && spec.type == PortType::Number && scale != 1.0)
        return v.toDouble() * scale; // not re-rounded: a node rounds where it needs whole pixels
    return v;
}

QVariantMap resolveParams(const NodeTypeInfo &info, const QVariantMap &stored, double scale)
{
    QVariantMap out;
    for (const ParamSpec &spec : info.params)
        out.insert(spec.id, resolveParam(spec, stored.value(spec.id), scale));
    return out;
}

NodeArgs::NodeArgs(Values inputs, QVariantMap params)
    : m_inputs(std::move(inputs))
    , m_params(std::move(params))
{
}

QImage NodeArgs::image(const QString &port) const
{
    return m_inputs.value(port).value<QImage>();
}

QImage NodeArgs::mask(const QString &port) const
{
    return m_inputs.value(port).value<QImage>();
}

QVariant NodeArgs::value(const QString &id) const
{
    const auto it = m_inputs.constFind(id);
    if (it != m_inputs.constEnd())
        return *it;
    return m_params.value(id);
}

double NodeArgs::number(const QString &id) const
{
    return value(id).toDouble();
}

int NodeArgs::integer(const QString &id) const
{
    return static_cast<int>(std::lround(number(id)));
}

QColor NodeArgs::color(const QString &id) const
{
    const QVariant v = value(id);
    return v.typeId() == QMetaType::QColor ? v.value<QColor>() : QColor::fromString(v.toString());
}

QString NodeArgs::text(const QString &id) const
{
    return value(id).toString();
}

EvalContext::EvalContext(Run *run, QString nodeId, double scale)
    : m_run(run)
    , m_nodeId(std::move(nodeId))
    , m_scale(scale)
{
}

bool EvalContext::isCancelled() const
{
    return m_run && m_run->isCancelled();
}

void EvalContext::onCancel(std::function<void()> handler)
{
    if (m_run)
        m_run->onCancel(std::move(handler));
}

QImage EvalContext::source() const
{
    return m_run ? m_run->source(m_scale) : QImage();
}

QImage EvalContext::selection() const
{
    return m_run ? m_run->selection(m_scale) : QImage();
}

QSize EvalContext::sourceSize() const
{
    return m_run ? m_run->sourceSize() : QSize();
}

std::function<bool()> EvalContext::cancelCheck() const
{
    Run *run = m_run;
    return [run] { return run && run->isCancelled(); };
}

NodeType::NodeType(NodeTypeInfo info)
    : m_info(std::move(info))
{
}

NodeType::~NodeType() = default;

namespace {

class FunctionNodeType final : public NodeType
{
public:
    FunctionNodeType(NodeTypeInfo info, NodeFunction function)
        : NodeType(std::move(info))
        , m_function(std::move(function))
    {
    }
    NodeResult evaluate(const NodeArgs &args, EvalContext &ctx) const override { return m_function(args, ctx); }

private:
    NodeFunction m_function;
};

} // namespace

NodeTypePtr makeNodeType(NodeTypeInfo info, NodeFunction function)
{
    return std::make_shared<FunctionNodeType>(std::move(info), std::move(function));
}

} // namespace lumen::filters
