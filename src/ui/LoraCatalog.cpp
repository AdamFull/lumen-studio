#include "ui/LoraCatalog.h"

#include "ai/Provider.h"
#include "ai/ProviderRegistry.h"
#include "runtime/ModelPresets.h"
#include "runtime/Supervisor.h"
#include "ui/Studio.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace lumen::ui {

namespace {

void sortChoices(QList<LoraChoice> &loras)
{
    std::stable_sort(loras.begin(), loras.end(), [](const LoraChoice &a, const LoraChoice &b) {
        if (a.mismatch.isEmpty() != b.mismatch.isEmpty())
            return a.mismatch.isEmpty();
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
}

} // namespace

QList<LoraChoice> lorasInFolder(const QString &folder, const QString &family, const QString &variant)
{
    QList<LoraChoice> out;
    const QDir root(folder);
    // The formats sd.cpp loads LoRAs from.
    QDirIterator it(folder, {QStringLiteral("*.safetensors"), QStringLiteral("*.ckpt"), QStringLiteral("*.pt"), QStringLiteral("*.gguf")},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = QDir::cleanPath(it.next());
        LoraChoice c;
        c.file = file;
        c.path = root.relativeFilePath(file);
        c.name = QFileInfo(file).completeBaseName();
        c.info = runtime::inspectModel(file);
        c.mismatch = runtime::loraMismatch(c.info, family, variant);
        // A file that is not a LoRA (a stray model) is not offered.
        if (c.info.kind != runtime::ModelKind::Lora && c.info.kind != runtime::ModelKind::Unknown)
            continue;
        out << c;
    }
    sortChoices(out);
    return out;
}

namespace {

// Words of three letters or more, in lower case, without those that say nothing about a look.
QStringList meaningfulWords(const QString &text)
{
    static const QSet<QString> filler{QStringLiteral("the"),   QStringLiteral("and"),     QStringLiteral("with"),   QStringLiteral("for"),
                                      QStringLiteral("from"),  QStringLiteral("into"),    QStringLiteral("make"),   QStringLiteral("add"),
                                      QStringLiteral("remove"), QStringLiteral("replace"), QStringLiteral("change"), QStringLiteral("this"),
                                      QStringLiteral("that"),  QStringLiteral("style"),   QStringLiteral("lora"),   QStringLiteral("model"),
                                      QStringLiteral("high"),  QStringLiteral("quality"), QStringLiteral("best"),   QStringLiteral("detailed"),
                                      QStringLiteral("sdxl"),  QStringLiteral("flux"),    QStringLiteral("klein"),  QStringLiteral("pony"),
                                      QStringLiteral("illustrious"), QStringLiteral("anima"), QStringLiteral("der"), QStringLiteral("die"),
                                      QStringLiteral("das"),   QStringLiteral("und"),     QStringLiteral("mit"),    QStringLiteral("ein"),
                                      QStringLiteral("eine")};
    // camelCase and digits split words apart: "WatercolorFox_v2" → watercolor, fox.
    QString spaced = text;
    static const QRegularExpression camel(QStringLiteral("(\\p{Ll})(\\p{Lu})"));
    spaced.replace(camel, QStringLiteral("\\1 \\2"));
    static const QRegularExpression split(QStringLiteral("[^\\p{L}]+"));
    QStringList out;
    for (const QString &w : spaced.toLower().split(split, Qt::SkipEmptyParts)) {
        if (w.size() >= 3 && !filler.contains(w) && !out.contains(w))
            out << w;
    }
    return out;
}

bool sameWord(const QString &a, const QString &b)
{
    if (a == b)
        return true;
    // "watercolor" and "watercolour", "cats" and "cat": one starts the other, both long enough.
    const QString &shorter = a.size() < b.size() ? a : b;
    const QString &longer = a.size() < b.size() ? b : a;
    return shorter.size() >= 4 && longer.startsWith(shorter.left(std::max<qsizetype>(4, shorter.size() - 1)));
}

} // namespace

QList<LoraChoice> suggestedLoras(const QList<LoraChoice> &loras, const QString &words, int max)
{
    const QStringList wanted = meaningfulWords(words);
    const QString lower = words.toLower();
    QList<std::pair<int, LoraChoice>> scored;
    for (const LoraChoice &c : loras) {
        if (!c.mismatch.isEmpty())
            continue;
        int score = 0;
        for (const QString &trigger : c.info.triggerWords) {
            if (trigger.trimmed().size() >= 3 && lower.contains(trigger.trimmed().toLower()))
                score += 3;
        }
        for (const QString &own : meaningfulWords(c.name)) {
            for (const QString &w : wanted) {
                if (sameWord(own, w)) {
                    ++score;
                    break;
                }
            }
        }
        if (score > 0)
            scored.append({score, c});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    QList<LoraChoice> out;
    for (const auto &[score, c] : std::as_const(scored)) {
        if (out.size() >= max)
            break;
        out << c;
    }
    return out;
}

QFuture<LoraListing> listLoras(Studio *studio, ai::ImageProvider *provider)
{
    LoraListing listing;
    if (!provider) {
        listing.problem = QCoreApplication::translate("lumen::ui::LoraCatalog", "There is no image engine to use LoRAs with.");
        return QtFuture::makeReadyValueFuture(listing);
    }
    if (!provider->capabilities().loras) {
        listing.problem = QCoreApplication::translate("lumen::ui::LoraCatalog", "%1 does not use LoRAs.").arg(provider->displayName());
        return QtFuture::makeReadyValueFuture(listing);
    }
    const QString profileId = provider->setting(QStringLiteral("managed_server")).toString();
    const std::optional<runtime::ServerProfile> profile = profileId.isEmpty() ? std::nullopt : studio->servers()->profile(profileId);
    if (profile) {
        listing.profileId = profileId;
        const QString kind = studio->servers()->kindOf(profileId);
        // The model the server runs, from its header, decides which LoRAs fit.
        QString model = profile->option(QStringLiteral("diffusion_model"), kind).toString().trimmed();
        if (model.isEmpty())
            model = profile->option(QStringLiteral("model"), kind).toString().trimmed();
        QString family;
        QString variant;
        if (!model.isEmpty()) {
            if (const auto info = runtime::modelInfoOf(studio->models(), QDir::cleanPath(QDir::fromNativeSeparators(model)))) {
                family = info->family;
                variant = info->variant;
            }
        }
        listing.model = runtime::modelFamilyName(family, variant);
        const QString folder = profile->option(QStringLiteral("lora_dir"), kind).toString().trimmed();
        if (folder.isEmpty()) {
            listing.problem = QCoreApplication::translate("lumen::ui::LoraCatalog", "This server has no LoRA folder. Choose one in Settings → Local servers.");
            return QtFuture::makeReadyValueFuture(listing);
        }
        if (!QFileInfo(folder).isDir()) {
            listing.problem = QCoreApplication::translate("lumen::ui::LoraCatalog", "The server's LoRA folder %1 is not there.").arg(QDir::toNativeSeparators(folder));
            return QtFuture::makeReadyValueFuture(listing);
        }
        return QtConcurrent::run([listing, folder, family, variant]() mutable {
            listing.loras = lorasInFolder(QDir::fromNativeSeparators(folder), family, variant);
            if (listing.loras.isEmpty())
                listing.problem = QCoreApplication::translate("lumen::ui::LoraCatalog", "There are no LoRAs in the server's LoRA folder (%1).").arg(QDir::toNativeSeparators(folder));
            return listing;
        });
    }
    // A server elsewhere: it names its LoRAs; what they were made for is not known here.
    const QString family = provider->setting(QStringLiteral("model_family")).toString();
    listing.model = runtime::modelFamilyName(family, QString());
    return provider->availableLoras()
        .then([listing](const QList<ai::ServerLora> &available) mutable {
            for (const ai::ServerLora &s : available) {
                LoraChoice c;
                c.name = s.name.isEmpty() ? QFileInfo(s.path).completeBaseName() : s.name;
                c.path = QDir::fromNativeSeparators(s.path);
                listing.loras << c;
            }
            sortChoices(listing.loras);
            if (listing.loras.isEmpty())
                listing.problem = QCoreApplication::translate("lumen::ui::LoraCatalog", "The server lists no LoRAs.");
            return listing;
        })
        .onFailed([listing](const std::exception_ptr &e) mutable {
            listing.problem = QCoreApplication::translate("lumen::ui::LoraCatalog", "The server's LoRAs could not be listed: %1").arg(ai::errorMessage(e));
            return listing;
        });
}

} // namespace lumen::ui
