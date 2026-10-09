#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>

namespace lumen::json {

// Validates JSON values against the subset of JSON Schema (draft 2020-12) that LLM structured-output
// APIs accept: type (incl. arrays of types), properties, required, additionalProperties (bool|schema),
// items, minItems, maxItems, enum, const, minimum, maximum, minLength, maxLength, anyOf, oneOf,
// $ref to "#/$defs/..." or "#/definitions/...".
class SchemaValidator
{
public:
    explicit SchemaValidator(QJsonObject schema);

    // Returns true when valid. Errors use JSON-pointer-like paths, e.g. "/shots/2/pose: missing required property".
    bool validate(const QJsonValue &value, QStringList *errors = nullptr) const;

    const QJsonObject &schema() const { return m_root; }

private:
    void check(const QJsonValue &value, const QJsonObject &schema, const QString &path, QStringList &errors, int depth) const;
    QJsonObject resolve(const QJsonObject &schema) const;
    static bool matchesType(const QJsonValue &value, const QString &type);

    QJsonObject m_root;
};

// Converts a schema to the strict form required by OpenAI structured outputs: every object gets
// additionalProperties=false and all properties listed as required.
QJsonObject toStrictSchema(const QJsonObject &schema);

} // namespace lumen::json
