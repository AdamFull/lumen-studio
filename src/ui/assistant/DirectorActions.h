#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>

namespace lumen::ui {

// The director workflow as the assistant drives it (implemented by the Create page). Every action
// reports through its callback; an empty error means success.
class DirectorActions
{
public:
    virtual ~DirectorActions() = default;

    // Session overview: references, character, purpose, shots with their results and critic scores.
    virtual QJsonObject directorState() const = 0;
    // Adds reference assets to the session and builds the character sheet from them.
    virtual void analyzeReferences(const QStringList &assetIds, const QString &notes,
                                   std::function<void(QJsonObject sheet, QString error)> done) = 0;
    // Plans (or revises) the shot plan; purpose = a purpose profile id, count 0 = as requested.
    virtual void planShots(const QString &request, const QString &purpose, int count,
                           std::function<void(QJsonObject plan, QString error)> done) = 0;
    virtual void revisePose(const QString &shotId, const QString &instruction,
                            std::function<void(QJsonObject shot, QString error)> done) = 0;
    // Generates the shots (all approved ones when empty) and reports the new assets once every shot
    // is finished, including Critic retries.
    virtual void generateShots(const QStringList &shotIds, std::function<void(QStringList assets, QString error)> done) = 0;
};

} // namespace lumen::ui
