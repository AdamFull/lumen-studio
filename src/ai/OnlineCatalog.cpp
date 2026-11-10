#include "ai/OnlineCatalog.h"

#include "core/Log.h"

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace lumen::ai::online {

namespace {

struct Service
{
    QString defaultModel;
    QList<Model> models;
    QList<Retired> retired;
};

const QHash<QString, Service> &catalog()
{
    static const QHash<QString, Service> services = [] {
        QHash<QString, Service> out;
        QFile f(QStringLiteral(":/providers/online.json"));
        if (!f.open(QIODevice::ReadOnly)) {
            qCWarning(lcAi) << "The online services catalogue is missing";
            return out;
        }
        const QJsonObject all = QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("services")).toObject();
        for (auto it = all.begin(); it != all.end(); ++it) {
            const QJsonObject o = it.value().toObject();
            Service s;
            s.defaultModel = o.value(QStringLiteral("default")).toString();
            for (const QJsonValue &v : o.value(QStringLiteral("models")).toArray()) {
                const QJsonObject m = v.toObject();
                s.models.append({m.value(QStringLiteral("id")).toString(), m.value(QStringLiteral("freeSize")).toBool(),
                                 m.value(QStringLiteral("inputFidelity")).toBool()});
            }
            for (const QJsonValue &v : o.value(QStringLiteral("retired")).toArray()) {
                const QJsonObject r = v.toObject();
                s.retired.append({r.value(QStringLiteral("id")).toString(),
                                  QDate::fromString(r.value(QStringLiteral("shutdown")).toString(), Qt::ISODate),
                                  r.value(QStringLiteral("replacement")).toString()});
            }
            out.insert(it.key(), s);
        }
        return out;
    }();
    return services;
}

// "gpt-image-2.5-flare-2026-09-08" → "gpt-image-2.5-flare" when that is known; else the id itself.
QString baseId(const QList<QString> &known, const QString &id)
{
    QString best;
    for (const QString &k : known) {
        if ((id == k || id.startsWith(k + u'-')) && k.size() > best.size())
            best = k;
    }
    return best.isEmpty() ? id : best;
}

} // namespace

QList<Model> models(const QString &typeId)
{
    return catalog().value(typeId).models;
}

QStringList modelIds(const QString &typeId)
{
    QStringList out;
    for (const Model &m : models(typeId))
        out << m.id;
    return out;
}

QString defaultModel(const QString &typeId)
{
    return catalog().value(typeId).defaultModel;
}

std::optional<Model> model(const QString &typeId, const QString &id)
{
    const QList<Model> all = models(typeId);
    QList<QString> known;
    for (const Model &m : all)
        known << m.id;
    const QString base = baseId(known, id);
    for (const Model &m : all) {
        if (m.id == base)
            return m;
    }
    return std::nullopt;
}

std::optional<Retired> retired(const QString &typeId, const QString &id)
{
    const QList<Retired> all = catalog().value(typeId).retired;
    QList<QString> known;
    for (const Retired &r : all)
        known << r.id;
    const QString base = baseId(known, id);
    for (const Retired &r : all) {
        if (r.id == base)
            return r;
    }
    return std::nullopt;
}

QString replacement(const QString &typeId, const QString &id, const QDate &today)
{
    QString current = id;
    for (int hops = 0; hops < 8; ++hops) {
        const std::optional<Retired> r = retired(typeId, current);
        if (!r || !r->shutdown.isValid() || today < r->shutdown || r->replacement.isEmpty())
            break;
        current = r->replacement;
    }
    return current == id ? QString() : current;
}

} // namespace lumen::ai::online
