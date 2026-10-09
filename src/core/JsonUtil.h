#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QVariant>

namespace lumen::json {

// Extracts the first complete JSON object/array from free-form model output. Handles ```json fences,
// leading prose and trailing commentary. Returns an undefined value when nothing parses.
QJsonValue extractJson(const QString &text, QString *error = nullptr);

QJsonObject parseObject(const QByteArray &data, QString *error = nullptr);
QByteArray compact(const QJsonObject &obj);
QByteArray compact(const QJsonArray &arr);
QString pretty(const QJsonValue &value);

// Reads a nested value with a dotted path, e.g. "choices.0.message.content".
QJsonValue at(const QJsonValue &root, const QString &dottedPath);

// Deep merge: values from `overlay` replace or extend `base`.
QJsonObject merge(const QJsonObject &base, const QJsonObject &overlay);

QJsonArray toArray(const QStringList &list);
QStringList toStringList(const QJsonArray &arr);

} // namespace lumen::json
