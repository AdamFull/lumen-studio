#include "plugins/parts/ExpressionCheck.h"

#include "ai/Director.h"
#include "ai/StructuredChat.h"
#include "core/Async.h"
#include "core/ImageOps.h"
#include "doc/Parts.h"
#include "ui/Studio.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QPainter>
#include <QPointer>

namespace lumen::ui {

namespace {

struct Text
{
    Q_DECLARE_TR_FUNCTIONS(lumen::ui::ExpressionCheck)
};

} // namespace

std::pair<QImage, QImage> expressionCrops(const QImage &source, const QImage &layer, const QRect &rect)
{
    // Three times the variant's size around it, at least 256 px.
    QRect area = rect.adjusted(-rect.width(), -rect.height(), rect.width(), rect.height());
    const int side = std::max(256, std::max(area.width(), area.height()));
    area = QRect(0, 0, side, side);
    area.moveCenter(rect.center());
    area &= source.rect();
    QImage after = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    {
        QPainter p(&after);
        p.drawImage(rect.topLeft(), layer);
    }
    return {img::flatten(source.copy(area), Qt::white), img::flatten(after.copy(area), Qt::white)};
}

void checkExpressionVariant(Studio *studio, doc::PartSet *setIn, int label, const ai::CharacterContext &character,
                            const QString &characterSummary, const QString &expression)
{
    const doc::Part *part = setIn ? setIn->part(label) : nullptr;
    if (!part || !part->hasResult())
        return;
    const QImage layer = setIn->resultImage(label);
    const QRect rect = part->chosenRect();
    if (layer.isNull() || rect.isEmpty())
        return;
    QPointer<doc::PartSet> set(setIn);
    QPointer<Studio> s(studio);
    const QString assetId = part->chosenAssetId();
    const QImage source = setIn->source();
    async::run([source, layer, rect] { return expressionCrops(source, layer, rect); })
        .then(studio, [s, set, label, assetId, character, characterSummary, expression](std::pair<QImage, QImage> crops) {
            if (!s || !set)
                return;
            const QJsonObject sheet = character.sheet.isEmpty() ? QJsonObject{{QStringLiteral("tags"), characterSummary}} : character.sheet;
            const QJsonObject shot{{QStringLiteral("title"), QStringLiteral("Expression variant")},
                                   {QStringLiteral("description"), QStringLiteral("Close-up of the character's face. Planned expression: ") + expression},
                                   {QStringLiteral("framing"), QStringLiteral("close-up of the face")}};
            // The face before the change first: it is what the variant must match.
            QList<QImage> references{crops.first};
            references << character.references;
            ai::StructuredChatOperation *op = s->director()->checkIdentity(crops.second, shot, sheet, references);
            QObject::connect(op, &ai::Operation::finished, set, [s, set, op, label, assetId, sheet] {
                if (!s || !set || op->state() != ai::Operation::State::Succeeded)
                    return;
                const doc::Part *p = set->part(label);
                if (!p || p->chosenAssetId() != assetId)
                    return; // regenerated meanwhile
                const QStringList checklist = ai::Director::identityChecklist(sheet);
                const QJsonObject features = op->result().value(QStringLiteral("features")).toObject();
                QStringList changed;
                for (int i = 0; i < checklist.size(); ++i) {
                    if (features.value(QStringLiteral("f%1").arg(i + 1)).toString() == QLatin1String("different"))
                        changed << checklist[i];
                }
                QStringList concerns;
                if (!changed.isEmpty())
                    concerns << Text::tr("the Critic sees a change: %1").arg(changed.join(QStringLiteral(", ")));
                if (op->result().value(QStringLiteral("pose")).toInt(10) <= 4)
                    concerns << Text::tr("the expression barely shows");
                const QString warning = concerns.join(QStringLiteral("; "));
                set->setPartWarning(label, warning);
                if (!warning.isEmpty())
                    s->notify(Text::tr("%1: %2.").arg(p->displayName(), warning), QStringLiteral("warning"));
            });
        });
}

} // namespace lumen::ui
