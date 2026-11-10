#pragma once

#include <QDate>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace lumen::ai {

// What the online services' APIs do not say about their models, shipped with Lumen
// (resources/providers/online.json) and kept current by Lumen's updates (plan M19).
namespace online {

struct Model
{
    QString id;
    bool freeSize = false;      // any width × height (multiples of 16) instead of a few fixed sizes
    bool inputFidelity = false; // takes OpenAI's input_fidelity
};

struct Retired
{
    QString id;
    QDate shutdown;
    QString replacement;
};

// The models to offer for a provider type ("openai_image"), newest first, and its default.
QList<Model> models(const QString &typeId);
QStringList modelIds(const QString &typeId);
QString defaultModel(const QString &typeId);

// A known model; a dated snapshot ("gpt-image-2.5-flare-2026-09-08") counts as its model.
std::optional<Model> model(const QString &typeId, const QString &id);

// The model to use instead of `id` when the service has shut it down by `today` (empty: `id` is
// fine). Follows chains, so a replacement that was retired in turn is replaced too.
QString replacement(const QString &typeId, const QString &id, const QDate &today = QDate::currentDate());
std::optional<Retired> retired(const QString &typeId, const QString &id);

} // namespace online
} // namespace lumen::ai
