#include "ai/PromptRules.h"

#include <QRegularExpression>

namespace lumen::ai {

namespace {

const QString kColours = QStringLiteral(
    "(?:(?:dark|light|bright|pale|deep|hell|dunkel)[ -]?)?(?:red|orange|yellow|green|blue|purple|violet|pink|brown|black|white|gr[ae]y|gold"
    "|golden|silver|beige|cyan|teal|turquoise|magenta|navy|rot|gelb|grün|blau|lila|violett|rosa|braun|schwarz|weiß|weiss|grau|silbern)");

QRegularExpression pattern(const QString &re)
{
    return QRegularExpression(QStringLiteral("^") + re + QStringLiteral("$"),
                              QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
}

// "it", "them", "das" … say nothing about what is there.
QString named(const QString &words)
{
    static const QRegularExpression pronoun = pattern(QStringLiteral("(?:it|them|this|that|these|those|es|sie|das|dies|ihn)"));
    const QString w = words.trimmed();
    return pronoun.match(w).hasMatch() ? QString() : w;
}

// Without the article in front: "the red cup" → "red cup".
QString bare(const QString &words)
{
    static const QRegularExpression article(QStringLiteral("^(?:a|an|the|some|der|die|das|den|dem|ein|eine|einen|einem)\\s+"),
                                            QRegularExpression::CaseInsensitiveOption);
    return QString(words).remove(article).trimmed();
}

} // namespace

QString emptyPlaceWords()
{
    return QStringLiteral("empty background, seamless continuation of the surroundings");
}

Instruction parseInstruction(const QString &words)
{
    QString w = words.simplified();
    static const QRegularExpression polite(QStringLiteral("^(?:please|bitte)\\s+|[.!]+$"), QRegularExpression::CaseInsensitiveOption);
    w.remove(polite);
    w = w.trimmed();
    Instruction in;
    if (w.isEmpty())
        return in;
    QRegularExpressionMatch m;
    auto matches = [&w, &m](const QRegularExpression &re) {
        m = re.match(w);
        return m.hasMatch();
    };

    // Recolouring first: "make the car red" would otherwise read as nothing, "turn it blue" as a
    // replacement.
    static const QRegularExpression colourOf =
        pattern(QStringLiteral("change the colou?r of (.+?) to (") + kColours + QStringLiteral(")"));
    static const QRegularExpression makeColour =
        pattern(QStringLiteral("(?:make|paint|colou?r|turn|dye|change|mach|mache|färbe|faerbe) (.+?) (?:to |in )?(") + kColours
                + QStringLiteral(")(?: an| ein)?"));
    if (matches(colourOf) || matches(makeColour)) {
        in.kind = Instruction::Kind::Recolor;
        in.target = named(m.captured(1));
        in.result = m.captured(2).toLower();
        return in;
    }
    static const QRegularExpression replace = pattern(QStringLiteral("(?:replace|swap|exchange) (.+?) (?:with|by|for) (.+)"));
    static const QRegularExpression into = pattern(QStringLiteral("(?:turn|change|transform|make) (.+?) into (.+)"));
    static const QRegularExpression ersetze = pattern(QStringLiteral("(?:ersetze|tausche) (.+?) (?:durch|mit|gegen) (.+)"));
    static const QRegularExpression machAus = pattern(QStringLiteral("(?:mach|mache|verwandle) (?:aus )?(.+?) (?:zu|in) (.+)"));
    if (matches(replace) || matches(into) || matches(ersetze) || matches(machAus)) {
        in.kind = Instruction::Kind::Replace;
        in.target = named(m.captured(1));
        in.result = m.captured(2).trimmed();
        return in;
    }
    static const QRegularExpression remove =
        pattern(QStringLiteral("(?:remove|delete|erase|get rid of|take away|take out|clean up|entferne|lösche|loesche|weg mit)(?: (.+))?"));
    static const QRegularExpression entfernen = pattern(QStringLiteral("(.+?) (?:entfernen|löschen|loeschen|weg)"));
    if (matches(remove) || matches(entfernen)) {
        in.kind = Instruction::Kind::Remove;
        in.target = named(m.captured(1));
        return in;
    }
    static const QRegularExpression add =
        pattern(QStringLiteral("(?:add|put|place|insert|draw)(?: in)? (.+?)(?: (?:here|there|on it|to it|in it|in the middle))?"));
    static const QRegularExpression hinzu = pattern(QStringLiteral("(?:füge|fuege|setze|zeichne) (.+?)(?: hinzu| ein| dazu)?"));
    static const QRegularExpression hinzufuegen = pattern(QStringLiteral("(.+?) (?:hinzufügen|hinzufuegen|einfügen|dazu)"));
    if (matches(add) || matches(hinzu) || matches(hinzufuegen)) {
        in.kind = Instruction::Kind::Add;
        in.result = m.captured(1).trimmed();
        return in;
    }
    return in;
}

FittedPrompt fitPrompt(const PromptStyle &style, GenMode mode, bool area, const QString &words, const QString &negative, bool literal)
{
    FittedPrompt f;
    const QString given = negative.trimmed();
    if (literal) {
        f.prompt = words;
        f.negative = given;
        return f;
    }
    f.negative = given.isEmpty() && style.supportsNegative ? style.defaultNegative : given;
    // Instruction models take the instruction; the family's tags would read as orders.
    if (mode == GenMode::Edit) {
        f.prompt = words;
        return f;
    }
    QString text = words;
    if (area && (mode == GenMode::Inpaint || mode == GenMode::ImageToImage)) {
        // An inpainting model draws what the words describe: what should be there, not the change.
        const Instruction in = parseInstruction(words);
        switch (in.kind) {
        case Instruction::Kind::Remove:
            text = emptyPlaceWords();
            if (!in.target.isEmpty() && style.supportsNegative)
                f.negative = f.negative.isEmpty() ? bare(in.target) : bare(in.target) + QStringLiteral(", ") + f.negative;
            break;
        case Instruction::Kind::Replace:
        case Instruction::Kind::Add:
            text = in.result;
            break;
        case Instruction::Kind::Recolor:
            text = in.target.isEmpty() ? in.result : in.result + u' ' + bare(in.target);
            break;
        case Instruction::Kind::None:
            break;
        }
        f.rewritten = in.kind != Instruction::Kind::None;
        if (f.rewritten && style.dialect == PromptStyle::Dialect::Tags)
            text = bare(text).toLower();
    }
    f.prompt = style.apply(text);
    return f;
}

} // namespace lumen::ai
