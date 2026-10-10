#pragma once

#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>

namespace lumen::ai {

// Captions for character LoRA training (M13 §13.4). The vision model tags each training image;
// tags that describe the character's fixed look (hair and eye colour, hairstyle, body features and,
// unless switched off, the outfit) are removed, so the LoRA ties them to the trigger word. The
// tags that change from image to image (view, framing, pose, expression, background) stay, so they
// remain controllable in prompts.

// Lower case, no escaped brackets, single spaces. Underscores stay (trigger words use them).
QString normalizeTag(const QString &tag);
// A tag as models write it ("long_red_hair") in the spelling captions use ("long red hair").
QString modelTag(const QString &tag);
// Comma-separated text → normalised tags without duplicates (order kept).
QStringList splitTags(const QString &text);
QString joinTags(const QStringList &tags);

// The character's own tags from its sheet (appearance tags, outfit, accessories, features).
QSet<QString> characterTags(const QJsonObject &sheet);
// Whether a tag belongs to the character's fixed look and is left out of captions.
bool isIdentityTag(const QString &tag, const QJsonObject &sheet, bool outfitIsIdentity);
// "trigger, tag, tag, …" with the identity tags pruned.
QString buildCaption(const QStringList &tags, const QJsonObject &sheet, const QString &trigger, bool outfitIsIdentity);
// A trigger word for a character name that models do not know yet: "lmn_mira".
QString suggestTrigger(const QString &name);

// Structured answer of the tagging request: {"tags": [...]}.
QJsonObject imageTagsSchema();

} // namespace lumen::ai
