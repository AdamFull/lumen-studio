#include "core/JsonSchema.h"

#include <QJsonArray>

#include <cmath>

namespace lumen::json {

SchemaValidator::SchemaValidator(QJsonObject schema)
    : m_root(std::move(schema))
{
}

bool SchemaValidator::validate(const QJsonValue &value, QStringList *errors) const
{
    QStringList local;
    check(value, m_root, QString(), local, 0);
    if (errors)
        *errors = local;
    return local.isEmpty();
}

QJsonObject SchemaValidator::resolve(const QJsonObject &schema) const
{
    QJsonObject current = schema;
    for (int guard = 0; guard < 16 && current.contains(QStringLiteral("$ref")); ++guard) {
        const QString ref = current.value(QStringLiteral("$ref")).toString();
        if (!ref.startsWith(QStringLiteral("#/")))
            return {};
        const QStringList parts = ref.mid(2).split(u'/');
        QJsonValue node = m_root;
        for (const QString &part : parts)
            node = node.toObject().value(part);
        current = node.toObject();
    }
    return current;
}

bool SchemaValidator::matchesType(const QJsonValue &value, const QString &type)
{
    if (type == QLatin1String("object"))
        return value.isObject();
    if (type == QLatin1String("array"))
        return value.isArray();
    if (type == QLatin1String("string"))
        return value.isString();
    if (type == QLatin1String("boolean"))
        return value.isBool();
    if (type == QLatin1String("null"))
        return value.isNull();
    if (type == QLatin1String("number"))
        return value.isDouble();
    if (type == QLatin1String("integer")) {
        if (!value.isDouble())
            return false;
        const double d = value.toDouble();
        return std::isfinite(d) && std::floor(d) == d;
    }
    return true;
}

void SchemaValidator::check(const QJsonValue &value, const QJsonObject &rawSchema, const QString &path,
                            QStringList &errors, int depth) const
{
    if (depth > 64) {
        errors << path + QStringLiteral(": schema nesting too deep");
        return;
    }
    const QJsonObject schema = resolve(rawSchema);
    const QString where = path.isEmpty() ? QStringLiteral("/") : path;

    if (schema.contains(QStringLiteral("anyOf")) || schema.contains(QStringLiteral("oneOf"))) {
        const bool one = schema.contains(QStringLiteral("oneOf"));
        const QJsonArray options = schema.value(one ? QStringLiteral("oneOf") : QStringLiteral("anyOf")).toArray();
        int matches = 0;
        for (const QJsonValue &opt : options) {
            QStringList sub;
            check(value, opt.toObject(), path, sub, depth + 1);
            matches += sub.isEmpty() ? 1 : 0;
        }
        if (matches == 0 || (one && matches > 1)) {
            errors << where + (matches == 0 ? QStringLiteral(": value does not match any allowed alternative")
                                            : QStringLiteral(": value matches more than one alternative"));
            return;
        }
    }

    const QJsonValue typeValue = schema.value(QStringLiteral("type"));
    if (!typeValue.isUndefined()) {
        bool ok = false;
        QStringList names;
        if (typeValue.isArray()) {
            for (const QJsonValue &t : typeValue.toArray()) {
                names << t.toString();
                ok = ok || matchesType(value, t.toString());
            }
        } else {
            names << typeValue.toString();
            ok = matchesType(value, typeValue.toString());
        }
        if (!ok) {
            errors << where + QStringLiteral(": expected ") + names.join(QStringLiteral(" or "));
            return;
        }
    }

    if (schema.contains(QStringLiteral("const")) && schema.value(QStringLiteral("const")) != value)
        errors << where + QStringLiteral(": value must equal the constant");

    if (schema.contains(QStringLiteral("enum"))) {
        const QJsonArray allowed = schema.value(QStringLiteral("enum")).toArray();
        if (!allowed.contains(value)) {
            QStringList opts;
            for (const QJsonValue &a : allowed)
                opts << (a.isString() ? a.toString() : QString::number(a.toDouble()));
            errors << where + QStringLiteral(": must be one of [") + opts.join(QStringLiteral(", ")) + u']';
        }
    }

    if (value.isDouble()) {
        const double d = value.toDouble();
        if (schema.contains(QStringLiteral("minimum")) && d < schema.value(QStringLiteral("minimum")).toDouble())
            errors << where + QStringLiteral(": must be >= ") + QString::number(schema.value(QStringLiteral("minimum")).toDouble());
        if (schema.contains(QStringLiteral("maximum")) && d > schema.value(QStringLiteral("maximum")).toDouble())
            errors << where + QStringLiteral(": must be <= ") + QString::number(schema.value(QStringLiteral("maximum")).toDouble());
    }

    if (value.isString()) {
        const qsizetype len = value.toString().size();
        if (schema.contains(QStringLiteral("minLength")) && len < schema.value(QStringLiteral("minLength")).toInt())
            errors << where + QStringLiteral(": string too short");
        if (schema.contains(QStringLiteral("maxLength")) && len > schema.value(QStringLiteral("maxLength")).toInt())
            errors << where + QStringLiteral(": string too long");
    }

    if (value.isArray()) {
        const QJsonArray arr = value.toArray();
        if (schema.contains(QStringLiteral("minItems")) && arr.size() < schema.value(QStringLiteral("minItems")).toInt())
            errors << where + QStringLiteral(": needs at least %1 items").arg(schema.value(QStringLiteral("minItems")).toInt());
        if (schema.contains(QStringLiteral("maxItems")) && arr.size() > schema.value(QStringLiteral("maxItems")).toInt())
            errors << where + QStringLiteral(": allows at most %1 items").arg(schema.value(QStringLiteral("maxItems")).toInt());
        const QJsonObject items = schema.value(QStringLiteral("items")).toObject();
        if (!items.isEmpty()) {
            for (qsizetype i = 0; i < arr.size(); ++i)
                check(arr.at(i), items, path + u'/' + QString::number(i), errors, depth + 1);
        }
    }

    if (value.isObject()) {
        const QJsonObject obj = value.toObject();
        const QJsonObject props = schema.value(QStringLiteral("properties")).toObject();
        for (const QJsonValue &req : schema.value(QStringLiteral("required")).toArray()) {
            if (!obj.contains(req.toString()))
                errors << where + QStringLiteral(": missing required property \"") + req.toString() + u'"';
        }
        const QJsonValue additional = schema.value(QStringLiteral("additionalProperties"));
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            const QString childPath = path + u'/' + it.key();
            if (props.contains(it.key())) {
                check(it.value(), props.value(it.key()).toObject(), childPath, errors, depth + 1);
            } else if (additional.isBool() && !additional.toBool()) {
                errors << childPath + QStringLiteral(": property is not allowed");
            } else if (additional.isObject()) {
                check(it.value(), additional.toObject(), childPath, errors, depth + 1);
            }
        }
    }
}

QJsonObject toStrictSchema(const QJsonObject &schema)
{
    QJsonObject out = schema;
    if (out.value(QStringLiteral("type")) == QLatin1String("object") || out.contains(QStringLiteral("properties"))) {
        QJsonObject props = out.value(QStringLiteral("properties")).toObject();
        QJsonArray required;
        for (auto it = props.begin(); it != props.end(); ++it) {
            it.value() = toStrictSchema(it.value().toObject());
            required.append(it.key());
        }
        out.insert(QStringLiteral("properties"), props);
        out.insert(QStringLiteral("required"), required);
        out.insert(QStringLiteral("additionalProperties"), false);
    }
    if (out.contains(QStringLiteral("items")))
        out.insert(QStringLiteral("items"), toStrictSchema(out.value(QStringLiteral("items")).toObject()));
    for (const QString &key : {QStringLiteral("anyOf"), QStringLiteral("oneOf")}) {
        if (!out.contains(key))
            continue;
        QJsonArray arr = out.value(key).toArray();
        for (qsizetype i = 0; i < arr.size(); ++i)
            arr[i] = toStrictSchema(arr.at(i).toObject());
        out.insert(key, arr);
    }
    for (const QString &key : {QStringLiteral("$defs"), QStringLiteral("definitions")}) {
        if (!out.contains(key))
            continue;
        QJsonObject defs = out.value(key).toObject();
        for (auto it = defs.begin(); it != defs.end(); ++it)
            it.value() = toStrictSchema(it.value().toObject());
        out.insert(key, defs);
    }
    // Strict mode does not support these keywords.
    for (const char *k : {"minItems", "maxItems", "minimum", "maximum", "minLength", "maxLength"})
        out.remove(QLatin1String(k));
    return out;
}

} // namespace lumen::json
