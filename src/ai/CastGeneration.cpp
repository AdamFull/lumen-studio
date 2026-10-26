#include "ai/CastGeneration.h"

#include "ai/Director.h"
#include "ai/PromptStyle.h"
#include "ai/StructuredChat.h"
#include "core/ImageOps.h"
#include "core/JobQueue.h"
#include "pose/PoseRenderer.h"

#include <QCoreApplication>
#include <QHash>
#include <QJsonArray>
#include <QPainter>
#include <QRegularExpression>

#include <memory>

namespace lumen::ai {

namespace {

struct Text
{
    Q_DECLARE_TR_FUNCTIONS(lumen::ai::CastGeneration)
};

QString joinTags(const QStringList &parts)
{
    QStringList out;
    for (const QString &p : parts) {
        if (!p.trimmed().isEmpty())
            out << p.trimmed();
    }
    return out.join(QStringLiteral(", "));
}

ControlInput poseControl(const pose::Pose &pose, const QSize &size)
{
    ControlInput c;
    c.type = ControlType::OpenPose;
    c.image = pose::renderOpenPose(pose, size);
    c.weight = 1.0;
    return c;
}

// Where member `index` stands, for the base prompt.
QString side(const pose::Arrangement arrangement, int index, int count)
{
    if (count < 2)
        return {};
    if (arrangement == pose::Arrangement::FrontBack)
        return index == 0 ? QStringLiteral("in front") : QStringLiteral("behind");
    return index == 0 ? QStringLiteral("on the left") : QStringLiteral("on the right");
}

} // namespace

QString castSubject(const QJsonObject &sheet)
{
    const QString g = sheet.value(QStringLiteral("gender_presentation")).toString().toLower();
    // "female" contains "male": the feminine words first.
    if (g.contains(QLatin1String("female")) || g.contains(QLatin1String("girl")) || g.contains(QLatin1String("woman"))
        || g.contains(QLatin1String("feminine")))
        return QStringLiteral("girl");
    if (g.contains(QLatin1String("male")) || g.contains(QLatin1String("boy")) || g.contains(QLatin1String("man"))
        || g.contains(QLatin1String("masculine")))
        return QStringLiteral("boy");
    return QStringLiteral("other");
}

QString castCountTags(const QList<CharacterContext> &cast)
{
    int girls = 0, boys = 0, others = 0;
    for (const CharacterContext &c : cast) {
        const QString s = castSubject(c.sheet);
        (s == QLatin1String("girl") ? girls : s == QLatin1String("boy") ? boys : others)++;
    }
    if (others > 0)
        return {}; // no reliable count tag: the sides and descriptions carry it
    QStringList tags;
    if (boys > 0)
        tags << (boys == 1 ? QStringLiteral("1boy") : QStringLiteral("%1boys").arg(boys));
    if (girls > 0)
        tags << (girls == 1 ? QStringLiteral("1girl") : QStringLiteral("%1girls").arg(girls));
    return tags.join(QStringLiteral(", "));
}

QString castAppearance(const QJsonObject &sheet, const PromptStyle &style)
{
    if (style.dialect != PromptStyle::Dialect::Tags) {
        const QString prose = sheet.value(QStringLiteral("description")).toString().trimmed();
        if (!prose.isEmpty())
            return prose;
    }
    static const QRegularExpression subject(QStringLiteral("^(\\d\\+?(girl|boy|other)s?|solo|solo focus|multiple \\w+)$"),
                                            QRegularExpression::CaseInsensitiveOption);
    QStringList out;
    for (const QString &t : Director::appearanceTags(sheet).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString tag = t.trimmed();
        if (!tag.isEmpty() && !subject.match(tag).hasMatch())
            out << tag;
    }
    return out.join(QStringLiteral(", "));
}

QJsonObject castDistinguishingSheet(const QJsonObject &of, const QJsonObject &from)
{
    PromptStyle tags;
    tags.dialect = PromptStyle::Dialect::Tags;
    QStringList theirs;
    for (const QString &t : castAppearance(from, tags).split(QLatin1Char(','), Qt::SkipEmptyParts))
        theirs << t.trimmed().toLower().replace(QLatin1Char('_'), QLatin1Char(' '));
    QStringList mine;
    for (const QString &t : castAppearance(of, tags).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString tag = t.trimmed();
        if (!theirs.contains(tag.toLower().replace(QLatin1Char('_'), QLatin1Char(' '))))
            mine << tag;
    }
    QJsonObject sheet = of;
    sheet.insert(QStringLiteral("tags"), mine.join(QStringLiteral(", ")));
    return sheet;
}

QString castVerdict(double own, double other, double minScore)
{
    if (own < 0)
        return QStringLiteral("unchecked");
    if (other >= 0 && other >= own + 2)
        return QStringLiteral("swap");
    if (other >= 5 && own - other < 3)
        return QStringLiteral("blend");
    if (own < minScore)
        return QStringLiteral("weak");
    return QStringLiteral("ok");
}

pose::Pose castPose(const CastRequest &request, const QSize &size)
{
    if (request.pose && request.pose->people.size() >= request.cast.size()) {
        pose::Pose p = *request.pose;
        p.resizeCanvas(size, true);
        return p;
    }
    QList<pose::RigParams> rigs;
    for (int i = 0; i < request.cast.size(); ++i) {
        pose::RigParams r;
        r.framing = QStringLiteral("cowboy_shot");
        rigs << r;
    }
    return pose::solvePose(pose::arrangePeople(rigs, request.arrangement), size);
}

ImageRequest castBaseRequest(const CastRequest &request, const pose::Pose &pose, const PromptStyle &style, const ImageCapabilities &caps)
{
    ImageRequest r = request.scene;
    r.mode = GenMode::TextToImage;
    r.references.clear();
    r.controls.clear();
    r.batchCount = 1;
    const int n = int(request.cast.size());
    const bool tags = style.dialect == PromptStyle::Dialect::Tags;
    QStringList people;
    for (int i = 0; i < n; ++i) {
        const QString looks = castAppearance(request.cast[i].sheet, style);
        const QString where = side(request.arrangement, i, n);
        if (looks.isEmpty())
            continue;
        people << (tags ? (where.isEmpty() ? looks : where + QStringLiteral(": ") + looks)
                        : (where.isEmpty() ? looks : QStringLiteral("%1%2, %3").arg(where.left(1).toUpper(), where.mid(1), looks)));
    }
    const QString scene = request.scene.prompt.trimmed();
    if (tags)
        r.prompt = style.apply(joinTags({castCountTags(request.cast), scene, people.join(QStringLiteral(", "))}));
    else
        r.prompt = style.apply(QStringList{scene, people.join(QStringLiteral(". "))}.join(QStringLiteral(". ")).trimmed());
    if (r.negativePrompt.trimmed().isEmpty() && style.supportsNegative)
        r.negativePrompt = style.defaultNegative;
    if (!style.supportsNegative)
        r.negativePrompt.clear();
    if (caps.poseControl && !pose.isEmpty())
        r.controls << poseControl(pose, r.size);
    return r;
}

ImageRequest castRefineRequest(const CastRequest &request, int index, const pose::Pose &pose, const PromptStyle &style,
                               const ImageCapabilities &caps, CharacterApplied *applied)
{
    const CharacterContext &c = request.cast[index];
    ImageRequest r = request.scene;
    r.mode = GenMode::Inpaint;
    r.denoise = request.refineDenoise;
    r.references.clear();
    r.controls.clear();
    r.batchCount = 1;
    if (r.seed >= 0)
        r.seed += 1 + index;
    const QString looks = castAppearance(c.sheet, style);
    const QString scene = request.scene.prompt.trimmed();
    if (style.dialect == PromptStyle::Dialect::Tags) {
        const QString subject = castSubject(c.sheet);
        r.prompt = style.apply(joinTags({subject == QLatin1String("other") ? QString() : QStringLiteral("1") + subject, looks, scene}));
    } else {
        r.prompt = style.apply(QStringList{looks, scene}.join(QStringLiteral(". ")).trimmed());
    }
    if (r.negativePrompt.trimmed().isEmpty() && style.supportsNegative)
        r.negativePrompt = style.defaultNegative;
    const CharacterApplied a = applyCharacter(r, c, caps, CharacterUse::Region, style, request.modelFamily);
    if (applied)
        *applied = a;
    if (caps.poseControl && index < pose.people.size())
        r.controls << poseControl(pose::onlyPerson(pose, index), r.size);
    return r;
}

Job *startCastGeneration(GenerationService *service, const CastRequest &requestIn)
{
    ProviderRegistry *registry = service->registry();
    ImageProvider *provider = requestIn.providerId.isEmpty() ? registry->defaultImageProvider() : registry->imageProvider(requestIn.providerId);
    CastRequest request = requestIn;
    if (provider)
        request.providerId = provider->instanceId();
    QStringList names;
    for (const CharacterContext &c : request.cast)
        names << c.name;
    const QString title = !request.title.isEmpty() ? request.title
                                                   : Text::tr("%1: %2").arg(names.join(QStringLiteral(" & ")), request.scene.prompt.left(40));
    QPointer<GenerationService> self(service);
    Job *job = service->queue()->submit(title, service->groupFor(request.providerId), [self, request](Job *job) {
        if (!self) {
            job->fail(Text::tr("The generation service is no longer available"));
            return;
        }
        ImageProvider *provider = self->registry()->imageProvider(request.providerId);
        if (!provider) {
            job->fail(Text::tr("There is no image engine. Set one up with Help → Set up AI…"));
            return;
        }
        if (request.cast.isEmpty()) {
            job->fail(Text::tr("No characters to draw."));
            return;
        }
        const ImageCapabilities caps = provider->capabilities();
        if (request.cast.size() > 1 && !caps.inpaint) {
            job->fail(Text::tr("%1 cannot repaint a region, which drawing two characters needs.").arg(provider->displayName()));
            return;
        }
        const PromptStyle style = promptStyleFor(provider, request.scene.model);
        const QSize size = request.scene.size.isValid() ? request.scene.size : QSize(1216, 832);
        CastRequest sized = request;
        sized.scene.size = size;
        const pose::Pose pose = castPose(sized, size);
        const QList<pose::PersonRegion> regions = pose::personRegions(pose, size);
        const int stages = 1 + int(regions.size());

        // What went in, for the metadata, and where the pipeline is.
        struct State
        {
            QImage image;
            QJsonArray members;
            GenerationSpec baseSpec;
            QList<int> refineQueue;   // members to repaint, in refine order
            QList<int> checkQueue;    // members to check
            QList<int> attempts;      // repaints per member beyond the first
            QHash<int, QRect> boxes;
            bool checkFailed = false; // no vision model: stop checking
        };
        auto state = std::make_shared<State>();
        for (const CharacterContext &c : request.cast)
            state->members.append(QJsonObject{{QStringLiteral("character"), c.id}, {QStringLiteral("name"), c.name}});
        state->attempts = QList<int>(request.cast.size(), 0);
        QList<int> order;
        for (const pose::PersonRegion &r : regions) {
            order << r.person;
            state->boxes.insert(r.person, r.box);
        }
        state->refineQueue = order;

        GenerationSpec base;
        base.providerId = request.providerId;
        base.request = castBaseRequest(sized, pose, style, caps);
        base.title = request.title;
        state->baseSpec = base;
        auto *director = new Director(self->registry(), job);

        auto store = [self, job, state, sized, pose]() {
            GenerationSpec spec = state->baseSpec;
            spec.title = sized.title;
            spec.saveAssets = sized.saveAssets;
            spec.meta = sized.meta;
            spec.meta.insert(QStringLiteral("user_prompt"), sized.scene.prompt);
            spec.meta.insert(QStringLiteral("cast"),
                             QJsonObject{{QStringLiteral("members"), state->members},
                                         {QStringLiteral("arrangement"), pose::arrangementKey(sized.arrangement)},
                                         {QStringLiteral("refine_denoise"), sized.refineDenoise},
                                         {QStringLiteral("pose"), pose.toOpenPoseJson()}});
            // What each member got (applied, region, seed, check), also for callers that do not store.
            job->metadata().insert(QStringLiteral("cast_members"), QVariant::fromValue(state->members));
            GenerationOutput out;
            out.images << state->image;
            out.rawImages << state->image;
            self->storeOutput(job, spec, out);
        };

        // The checks, one member after the other; failing members are repainted (refine) and
        // checked again until they pass or have no retries left.
        auto refineNext = std::make_shared<std::function<void()>>();
        auto checkNext = std::make_shared<std::function<void()>>();
        *checkNext = [self, job, state, sized, director, order, store, refineNext, checkNext]() {
            if (!self)
                return job->fail(Text::tr("The generation service is no longer available"));
            if (job->isCancelRequested())
                return job->reportCancelled();
            if (state->checkQueue.isEmpty() || state->checkFailed) {
                // Members to repaint again, in refine order.
                QList<int> again;
                for (int m : order) {
                    const QJsonObject check = state->members[m].toObject().value(QStringLiteral("check")).toObject();
                    const QString verdict = check.value(QStringLiteral("verdict")).toString();
                    if (!verdict.isEmpty() && verdict != QLatin1String("ok") && verdict != QLatin1String("unchecked")
                        && state->attempts[m] < sized.maxRetries)
                        again << m;
                }
                if (again.isEmpty() || state->checkFailed)
                    return store();
                for (int m : again)
                    ++state->attempts[m];
                state->refineQueue = again;
                state->checkQueue = again;
                return (*refineNext)();
            }
            const int member = state->checkQueue.takeFirst();
            const CharacterContext &c = sized.cast[member];
            const QImage crop = state->image.copy(state->boxes.value(member));
            const QJsonObject shot{{QStringLiteral("title"), c.name}, {QStringLiteral("prompt"), sized.scene.prompt}};
            job->setProgress(-1.0, Text::tr("Checking %1…").arg(c.name));
            const int other = sized.cast.size() > 1 ? 1 - member : -1;
            auto result = std::make_shared<CastCheck>();
            auto finish = [state, member, sized, result, checkNext]() {
                result->verdict = castVerdict(result->own, result->other, sized.minScore);
                QJsonObject m = state->members[member].toObject();
                QJsonObject check{{QStringLiteral("own"), result->own},
                                  {QStringLiteral("verdict"), result->verdict},
                                  {QStringLiteral("attempts"), state->attempts[member] + 1},
                                  {QStringLiteral("issues"), QJsonArray::fromStringList(result->issues)}};
                if (result->other >= 0)
                    check.insert(QStringLiteral("other"), result->other);
                m.insert(QStringLiteral("check"), check);
                state->members[member] = m;
                (*checkNext)();
            };
            auto unavailable = [state, member, checkNext](const QString &error) {
                // No vision model (or it failed): the image stands as drawn.
                QJsonObject m = state->members[member].toObject();
                m.insert(QStringLiteral("check"), QJsonObject{{QStringLiteral("verdict"), QStringLiteral("unchecked")},
                                                              {QStringLiteral("error"), error}});
                state->members[member] = m;
                state->checkFailed = true;
                (*checkNext)();
            };
            StructuredChatOperation *own = director->checkIdentity(crop, shot, c.sheet, c.references);
            QObject::connect(own, &Operation::finished, job, [own, director, crop, shot, sized, other, result, finish, unavailable] {
                if (own->state() != Operation::State::Succeeded)
                    return unavailable(own->errorString());
                result->own = Director::checklistIdentity(own->result());
                for (const QJsonValue &v : own->result().value(QStringLiteral("issues")).toArray())
                    result->issues << v.toString();
                if (other < 0)
                    return finish();
                const QJsonObject theirs = castDistinguishingSheet(sized.cast[other].sheet, sized.cast[1 - other].sheet);
                StructuredChatOperation *op = director->checkIdentity(crop, shot, theirs, sized.cast[other].references);
                QObject::connect(op, &Operation::finished, director, [op, result, finish, unavailable] {
                    if (op->state() != Operation::State::Succeeded)
                        return unavailable(op->errorString());
                    result->other = Director::checklistIdentity(op->result());
                    finish();
                });
            });
        };

        // The repaints, back to front; then the checks (or the result).
        *refineNext = [self, job, state, sized, pose, regions, style, caps, stages, refineNext, checkNext, store]() {
            if (!self)
                return job->fail(Text::tr("The generation service is no longer available"));
            if (job->isCancelRequested())
                return job->reportCancelled();
            if (state->refineQueue.isEmpty()) {
                if (!sized.check)
                    return store();
                if (state->checkQueue.isEmpty()) {
                    for (const pose::PersonRegion &r : regions)
                        state->checkQueue << r.person;
                }
                return (*checkNext)();
            }
            const int member = state->refineQueue.takeFirst();
            pose::PersonRegion region;
            for (const pose::PersonRegion &r : regions) {
                if (r.person == member)
                    region = r;
            }
            CharacterApplied applied;
            GenerationSpec spec;
            spec.providerId = sized.providerId;
            spec.request = castRefineRequest(sized, member, pose, style, caps, &applied);
            const int attempt = state->attempts[member];
            if (attempt > 0 && spec.request.seed >= 0)
                spec.request.seed += 1000 * attempt; // a retry draws the member anew
            spec.region = RegionInput{state->image, region.mask, 48, 2, 12, true};
            QJsonObject m = state->members[member].toObject();
            m.insert(QStringLiteral("applied"), applied.toJson(sized.cast[member]));
            m.insert(QStringLiteral("region"), QJsonArray{region.box.x(), region.box.y(), region.box.width(), region.box.height()});
            m.insert(QStringLiteral("seed"), double(spec.request.seed));
            state->members[member] = m;
            job->setProgress(-1.0, attempt > 0 ? Text::tr("Drawing %1 again…").arg(sized.cast[member].name)
                                               : Text::tr("Drawing %1 (%2 of %3)…").arg(sized.cast[member].name).arg(stages - int(state->refineQueue.size())).arg(stages));
            self->execute(job, spec, [state, refineNext](Job *job, const GenerationSpec &, GenerationOutput out) {
                if (out.images.isEmpty())
                    return job->fail(Text::tr("The provider returned no images"));
                QPainter painter(&state->image);
                painter.drawImage(0, 0, out.images.first()); // the region layer, transparent elsewhere
                painter.end();
                (*refineNext)();
            });
        };

        job->setProgress(-1.0, Text::tr("Drawing the scene (1 of %1)…").arg(stages));
        self->execute(job, base, [state, refineNext](Job *job, const GenerationSpec &used, GenerationOutput out) {
            if (out.images.isEmpty())
                return job->fail(Text::tr("The provider returned no images"));
            state->baseSpec.request = used.request;
            state->image = img::normalized(out.images.first()).convertToFormat(QImage::Format_ARGB32_Premultiplied);
            (*refineNext)();
        });
    }, QStringLiteral("generation"));
    job->metadata().insert(QStringLiteral("prompt"), request.scene.prompt);
    job->metadata().insert(QStringLiteral("provider"), provider ? provider->displayName() : QString());
    job->metadata().insert(QStringLiteral("cast"), names);
    return job;
}

} // namespace lumen::ai
