#include "ai/LoraCaptions.h"

#include "ai/Director.h"

#include <QJsonArray>
#include <QRegularExpression>

namespace lumen::ai {

namespace {

const QSet<QString> &colourWords()
{
    static const QSet<QString> words{
        QStringLiteral("red"), QStringLiteral("orange"), QStringLiteral("yellow"), QStringLiteral("green"), QStringLiteral("blue"),
        QStringLiteral("aqua"), QStringLiteral("purple"), QStringLiteral("pink"), QStringLiteral("brown"), QStringLiteral("black"),
        QStringLiteral("white"), QStringLiteral("grey"), QStringLiteral("gray"), QStringLiteral("silver"), QStringLiteral("blonde"),
        QStringLiteral("blond"), QStringLiteral("gold"), QStringLiteral("golden"), QStringLiteral("violet"), QStringLiteral("light"),
        QStringLiteral("dark"), QStringLiteral("multicolored"), QStringLiteral("two-tone"), QStringLiteral("gradient"),
        QStringLiteral("streaked"), QStringLiteral("colored"), QStringLiteral("platinum"), QStringLiteral("crimson"),
        QStringLiteral("magenta"), QStringLiteral("teal"), QStringLiteral("navy"), QStringLiteral("beige"), QStringLiteral("cyan")};
    return words;
}

const QSet<QString> &hairWords()
{
    static const QSet<QString> words{QStringLiteral("long"), QStringLiteral("short"), QStringLiteral("medium"), QStringLiteral("very"),
                                     QStringLiteral("absurdly"), QStringLiteral("straight"), QStringLiteral("wavy"), QStringLiteral("curly"),
                                     QStringLiteral("messy"), QStringLiteral("spiked"), QStringLiteral("shiny"), QStringLiteral("thick")};
    return words;
}

// Hairstyles and body features that never change for a character.
const QSet<QString> &fixedFeatures()
{
    static const QSet<QString> tags{
        QStringLiteral("ponytail"), QStringLiteral("high ponytail"), QStringLiteral("low ponytail"), QStringLiteral("side ponytail"),
        QStringLiteral("twintails"), QStringLiteral("low twintails"), QStringLiteral("braid"), QStringLiteral("single braid"),
        QStringLiteral("twin braids"), QStringLiteral("french braid"), QStringLiteral("crown braid"), QStringLiteral("hair bun"),
        QStringLiteral("single hair bun"), QStringLiteral("double bun"), QStringLiteral("bangs"), QStringLiteral("blunt bangs"),
        QStringLiteral("swept bangs"), QStringLiteral("parted bangs"), QStringLiteral("sidelocks"), QStringLiteral("ahoge"),
        QStringLiteral("hair between eyes"), QStringLiteral("hair over one eye"), QStringLiteral("hime cut"), QStringLiteral("bob cut"),
        QStringLiteral("drill hair"), QStringLiteral("hair intakes"), QStringLiteral("heterochromia"), QStringLiteral("slit pupils"),
        QStringLiteral("symbol-shaped pupils"), QStringLiteral("dark skin"), QStringLiteral("dark-skinned female"),
        QStringLiteral("dark-skinned male"), QStringLiteral("pale skin"), QStringLiteral("tan"), QStringLiteral("freckles"),
        QStringLiteral("mole"), QStringLiteral("mole under eye"), QStringLiteral("animal ears"), QStringLiteral("cat ears"),
        QStringLiteral("fox ears"), QStringLiteral("wolf ears"), QStringLiteral("rabbit ears"), QStringLiteral("dog ears"),
        QStringLiteral("pointy ears"), QStringLiteral("elf"), QStringLiteral("tail"), QStringLiteral("cat tail"), QStringLiteral("fox tail"),
        QStringLiteral("horns"), QStringLiteral("wings"), QStringLiteral("halo"), QStringLiteral("fang"), QStringLiteral("scar"),
        QStringLiteral("facial mark"), QStringLiteral("tattoo")};
    return tags;
}

// Eye states and gazes are expressions, not identity.
bool isEyeColour(const QString &tag)
{
    if (!tag.endsWith(QLatin1String(" eyes")))
        return false;
    const QStringList words = tag.chopped(5).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    return !words.isEmpty() && std::all_of(words.begin(), words.end(), [](const QString &w) { return colourWords().contains(w); });
}

bool isHairLook(const QString &tag)
{
    if (!tag.endsWith(QLatin1String(" hair")))
        return false;
    const QStringList words = tag.chopped(5).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    return !words.isEmpty()
           && std::all_of(words.begin(), words.end(), [](const QString &w) { return colourWords().contains(w) || hairWords().contains(w); });
}

bool isBodyTag(const QString &tag)
{
    return isEyeColour(tag) || isHairLook(tag) || fixedFeatures().contains(tag);
}

// Nouns of the character's outfit and accessories ("cloak", "hood", "choker"), to catch the vision
// model's own wording of the same clothes.
QSet<QString> outfitWords(const QJsonObject &sheet)
{
    static const QSet<QString> skip{QStringLiteral("with"), QStringLiteral("and"), QStringLiteral("the"), QStringLiteral("trim"),
                                    QStringLiteral("long"), QStringLiteral("short"), QStringLiteral("open")};
    QSet<QString> words;
    for (const QString &tag : characterTags(sheet)) {
        if (isBodyTag(tag))
            continue;
        for (const QString &w : tag.split(QRegularExpression(QStringLiteral("[ -]")), Qt::SkipEmptyParts)) {
            if (w.size() >= 3 && !colourWords().contains(w) && !skip.contains(w))
                words << w;
        }
    }
    return words;
}

} // namespace

QString normalizeTag(const QString &tag)
{
    QString t = tag.trimmed().toLower();
    t.remove(QStringLiteral("\\"));
    static const QRegularExpression spaces(QStringLiteral("\\s+"));
    t.replace(spaces, QStringLiteral(" "));
    return t.trimmed();
}

QString modelTag(const QString &tag)
{
    QString t = tag;
    t.replace(QLatin1Char('_'), QLatin1Char(' '));
    return normalizeTag(t);
}

QStringList splitTags(const QString &text)
{
    QStringList out;
    for (const QString &part : text.split(QLatin1Char(','))) {
        const QString t = normalizeTag(part);
        if (!t.isEmpty() && !out.contains(t))
            out << t;
    }
    return out;
}

QString joinTags(const QStringList &tags)
{
    return tags.join(QStringLiteral(", "));
}

QSet<QString> characterTags(const QJsonObject &sheet)
{
    QSet<QString> out;
    for (const QString &t : splitTags(Director::appearanceTags(sheet)))
        out << modelTag(t);
    for (const char *key : {"accessories", "distinctive_features"}) {
        for (const QJsonValue &v : sheet.value(QLatin1String(key)).toArray()) {
            const QString t = modelTag(v.toString());
            if (!t.isEmpty())
                out << t;
        }
    }
    for (const QJsonValue &v : sheet.value(QStringLiteral("outfit")).toArray()) {
        const QJsonObject o = v.toObject();
        const QString item = modelTag(o.value(QStringLiteral("item")).toString());
        if (!item.isEmpty()) {
            out << item;
            const QString colour = modelTag(o.value(QStringLiteral("color")).toString());
            if (!colour.isEmpty())
                out << colour + QLatin1Char(' ') + item;
        }
    }
    return out;
}

bool isIdentityTag(const QString &tagIn, const QJsonObject &sheet, bool outfitIsIdentity)
{
    const QString tag = modelTag(tagIn);
    if (isBodyTag(tag))
        return true;
    const QSet<QString> own = characterTags(sheet);
    if (own.contains(tag)) {
        // A tag of the sheet that is not about the body is clothing or an accessory.
        return outfitIsIdentity;
    }
    if (!outfitIsIdentity)
        return false;
    // The model's own words for the character's clothes: "cloak" or "hood" for "black hooded cloak"
    // (one word starting with the other covers hood / hooded / hoodie).
    const QSet<QString> words = outfitWords(sheet);
    for (const QString &w : tag.split(QRegularExpression(QStringLiteral("[ -]")), Qt::SkipEmptyParts)) {
        if (w.size() < 3 || colourWords().contains(w))
            continue;
        for (const QString &own : words) {
            // Same word, or the same stem: a common start of 4+ letters that leaves at most two
            // letters of the shorter word (hood / hooded / hoodie, cloak / cloaked).
            qsizetype common = 0;
            while (common < w.size() && common < own.size() && w[common] == own[common])
                ++common;
            const qsizetype shorter = std::min(w.size(), own.size());
            if (w == own || (common >= 4 && common >= shorter - 2))
                return true;
        }
    }
    return false;
}

QString buildCaption(const QStringList &tags, const QJsonObject &sheet, const QString &trigger, bool outfitIsIdentity)
{
    QStringList out;
    const QString t = trigger.trimmed();
    if (!t.isEmpty())
        out << t;
    for (const QString &raw : tags) {
        const QString tag = normalizeTag(raw);
        if (tag.isEmpty() || tag == normalizeTag(t) || out.contains(tag) || isIdentityTag(tag, sheet, outfitIsIdentity))
            continue;
        out << tag;
    }
    return joinTags(out);
}

QString suggestTrigger(const QString &name)
{
    QString base = name.toLower();
    base.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("_"));
    base = base.section(QLatin1Char('_'), 0, 0, QString::SectionSkipEmpty); // the first name is enough
    if (base.isEmpty())
        base = QStringLiteral("char");
    return QStringLiteral("lmn_") + base;
}

QJsonObject imageTagsSchema()
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"),
         QJsonObject{{QStringLiteral("tags"), QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                                                           {QStringLiteral("maxItems"), 50},
                                                           {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                                                                                 {QStringLiteral("maxLength"), 40}}}}}}},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("tags")}},
        {QStringLiteral("additionalProperties"), false}};
}

} // namespace lumen::ai
