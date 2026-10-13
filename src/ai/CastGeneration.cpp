#include "ai/CastGeneration.h"

#include "ai/Director.h"
#include "ai/PromptStyle.h"
#include "core/ImageOps.h"
#include "core/JobQueue.h"
#include "pose/PoseRenderer.h"

#include <QCoreApplication>
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
            job->fail(Text::tr("No image provider is configured. Add one in Settings → Providers."));
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

        // What went in, for the metadata.
        struct State
        {
            QImage image;
            QJsonArray members;
            GenerationSpec baseSpec;
            int next = 0;
        };
        auto state = std::make_shared<State>();
        for (const CharacterContext &c : request.cast)
            state->members.append(QJsonObject{{QStringLiteral("character"), c.id}, {QStringLiteral("name"), c.name}});

        GenerationSpec base;
        base.providerId = request.providerId;
        base.request = castBaseRequest(sized, pose, style, caps);
        base.title = request.title;
        state->baseSpec = base;

        // Each refine pass in turn, back to front; then the result is stored.
        auto refineNext = std::make_shared<std::function<void()>>();
        *refineNext = [self, job, state, sized, pose, regions, style, caps, stages, refineNext]() {
            if (!self)
                return job->fail(Text::tr("The generation service is no longer available"));
            if (job->isCancelRequested())
                return job->reportCancelled();
            if (state->next >= regions.size()) {
                GenerationSpec store = state->baseSpec;
                store.title = sized.title;
                store.saveAssets = sized.saveAssets;
                store.meta = sized.meta;
                store.meta.insert(QStringLiteral("user_prompt"), sized.scene.prompt);
                store.meta.insert(QStringLiteral("cast"),
                                  QJsonObject{{QStringLiteral("members"), state->members},
                                              {QStringLiteral("arrangement"), pose::arrangementKey(sized.arrangement)},
                                              {QStringLiteral("refine_denoise"), sized.refineDenoise},
                                              {QStringLiteral("pose"), pose.toOpenPoseJson()}});
                GenerationOutput out;
                out.images << state->image;
                out.rawImages << state->image;
                self->storeOutput(job, store, out);
                return;
            }
            const pose::PersonRegion region = regions[state->next++];
            const int member = region.person;
            CharacterApplied applied;
            GenerationSpec spec;
            spec.providerId = sized.providerId;
            spec.request = castRefineRequest(sized, member, pose, style, caps, &applied);
            spec.region = RegionInput{state->image, region.mask, 48, 2, 12, true};
            QJsonObject m = state->members[member].toObject();
            m.insert(QStringLiteral("applied"), applied.toJson(sized.cast[member]));
            m.insert(QStringLiteral("region"), QJsonArray{region.box.x(), region.box.y(), region.box.width(), region.box.height()});
            m.insert(QStringLiteral("seed"), double(spec.request.seed));
            state->members[member] = m;
            job->setProgress(-1.0, Text::tr("Drawing %1 (%2 of %3)…").arg(sized.cast[member].name).arg(state->next + 1).arg(stages));
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
