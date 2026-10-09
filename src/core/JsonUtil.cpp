#include "core/JsonUtil.h"

namespace lumen::json {

namespace {

// Finds the end index (inclusive) of a balanced JSON value that starts at `start` ('{' or '[').
qsizetype balancedEnd(const QString &text, qsizetype start)
{
    QList<QChar> stack;
    bool inString = false;
    bool escape = false;
    for (qsizetype i = start; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (inString) {
            if (escape)
                escape = false;
            else if (c == u'\\')
                escape = true;
            else if (c == u'"')
                inString = false;
            continue;
        }
        if (c == u'"') {
            inString = true;
        } else if (c == u'{' || c == u'[') {
            stack.append(c == u'{' ? u'}' : u']');
        } else if (c == u'}' || c == u']') {
            if (stack.isEmpty() || stack.last() != c)
                return -1;
            stack.removeLast();
            if (stack.isEmpty())
                return i;
        }
    }
    return -1;
}

QJsonValue tryParse(const QString &candidate, QString *error)
{
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(candidate.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError) {
        if (error)
            *error = err.errorString();
        return QJsonValue(QJsonValue::Undefined);
    }
    if (doc.isObject())
        return doc.object();
    if (doc.isArray())
        return doc.array();
    return QJsonValue(QJsonValue::Undefined);
}

} // namespace

QJsonValue extractJson(const QString &text, QString *error)
{
    QString body = text.trimmed();
    // Strip reasoning blocks emitted by some local models.
    const qsizetype thinkEnd = body.lastIndexOf(QStringLiteral("</think>"));
    if (thinkEnd >= 0)
        body = body.mid(thinkEnd + 8).trimmed();

    // Prefer fenced blocks.
    const qsizetype fence = body.indexOf(QStringLiteral("```"));
    if (fence >= 0) {
        qsizetype contentStart = body.indexOf(u'\n', fence);
        const qsizetype close = contentStart >= 0 ? body.indexOf(QStringLiteral("```"), contentStart) : -1;
        if (contentStart >= 0 && close > contentStart) {
            const QJsonValue v = tryParse(body.mid(contentStart + 1, close - contentStart - 1), error);
            if (!v.isUndefined())
                return v;
        }
    }

    const QJsonValue direct = tryParse(body, error);
    if (!direct.isUndefined())
        return direct;

    for (qsizetype i = 0; i < body.size(); ++i) {
        const QChar c = body.at(i);
        if (c != u'{' && c != u'[')
            continue;
        const qsizetype end = balancedEnd(body, i);
        if (end < 0)
            continue;
        const QJsonValue v = tryParse(body.mid(i, end - i + 1), error);
        if (!v.isUndefined())
            return v;
    }
    if (error && error->isEmpty())
        *error = QStringLiteral("no JSON object found in model output");
    return QJsonValue(QJsonValue::Undefined);
}

QJsonObject parseObject(const QByteArray &data, QString *error)
{
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError) {
        if (error)
            *error = err.errorString();
        return {};
    }
    if (!doc.isObject() && error)
        *error = QStringLiteral("JSON document is not an object");
    return doc.object();
}

QByteArray compact(const QJsonObject &obj)
{
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

QByteArray compact(const QJsonArray &arr)
{
    return QJsonDocument(arr).toJson(QJsonDocument::Compact);
}

QString pretty(const QJsonValue &value)
{
    if (value.isObject())
        return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Indented));
    if (value.isArray())
        return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Indented));
    return value.toVariant().toString();
}

QJsonValue at(const QJsonValue &root, const QString &dottedPath)
{
    QJsonValue cur = root;
    for (const QString &part : dottedPath.split(u'.', Qt::SkipEmptyParts)) {
        bool isIndex = false;
        const int idx = part.toInt(&isIndex);
        if (isIndex && cur.isArray())
            cur = cur.toArray().at(idx);
        else
            cur = cur.toObject().value(part);
        if (cur.isUndefined())
            break;
    }
    return cur;
}

QJsonObject merge(const QJsonObject &base, const QJsonObject &overlay)
{
    QJsonObject out = base;
    for (auto it = overlay.begin(); it != overlay.end(); ++it) {
        if (it.value().isObject() && out.value(it.key()).isObject())
            out.insert(it.key(), merge(out.value(it.key()).toObject(), it.value().toObject()));
        else
            out.insert(it.key(), it.value());
    }
    return out;
}

QJsonArray toArray(const QStringList &list)
{
    QJsonArray arr;
    for (const QString &s : list)
        arr.append(s);
    return arr;
}

QStringList toStringList(const QJsonArray &arr)
{
    QStringList list;
    for (const QJsonValue &v : arr)
        list << v.toString();
    return list;
}

} // namespace lumen::json
