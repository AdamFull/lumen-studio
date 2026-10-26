#include "ai/FilterNodes.h"

#include "ai/GenerationService.h"
#include "ai/PromptStyle.h"
#include "core/Async.h"
#include "core/Error.h"
#include "core/ImageOps.h"
#include "core/Job.h"
#include "filters/Registry.h"

#include <QCoreApplication>
#include <QLocale>
#include <QPainter>
#include <QPointer>
#include <QPromise>

#include <cmath>
#include <memory>

namespace lumen::ai {

namespace {

struct Text
{
    Q_DECLARE_TR_FUNCTIONS(lumen::ai::FilterNodes)
};

using filters::NodeArgs;
using filters::PortType;
using filters::Values;

const QString ImagePort = QStringLiteral("image");
const QString MaskPort = QStringLiteral("mask");
const QString PromptParam = QStringLiteral("prompt");

enum class Step { Edit, Inpaint, Upscale, RemoveBackground };

// The services AI nodes can run through, newest last (one per Studio). GUI thread only.
QList<QPointer<GenerationService>> &services()
{
    static QList<QPointer<GenerationService>> list;
    return list;
}

QString noEngine()
{
    return Text::tr("There is no image engine to run this AI step. Add one in Settings → Providers.");
}

// The image engine a step runs on, or why there is none.
struct Engine
{
    ImageProvider *provider = nullptr;
    QString problem;
};

// GUI thread: providers are only used there.
Engine engineFor(Step step, const QString &chosen)
{
    GenerationService *service = filterNodeService();
    if (!service)
        return {nullptr, Text::tr("AI steps cannot run here: Lumen's image engines are not available.")};
    ProviderRegistry *registry = service->registry();
    ImageProvider *provider = nullptr;
    if (!chosen.isEmpty()) {
        provider = registry->imageProvider(chosen);
        if (!provider)
            return {nullptr, Text::tr("The image engine chosen for this step is no longer set up. Choose another one for it in the "
                                      "filter editor, or “The one in use”.")};
    }
    switch (step) {
    case Step::Upscale:
    case Step::RemoveBackground: {
        const Utility utility = step == Step::Upscale ? Utility::Upscale : Utility::RemoveBackground;
        if (provider) {
            if (provider->supportsUtility(utility))
                return {provider, {}};
            return {nullptr, step == Step::Upscale
                                 ? Text::tr("“%1” has no upscaler. Choose another engine for this step, or “The one in use”.")
                                       .arg(provider->displayName())
                                 : Text::tr("“%1” cannot remove backgrounds. Choose another engine for this step, or “The one in "
                                            "use”.")
                                       .arg(provider->displayName())};
        }
        if (ImageProvider *p = service->providerForUtility(utility))
            return {p, {}};
        if (registry->imageProviders().isEmpty())
            return {nullptr, noEngine()};
        return {nullptr, step == Step::Upscale
                             ? Text::tr("None of your image engines has an upscaler. A stable-diffusion.cpp server needs an upscale "
                                        "model in its settings; ComfyUI, Stable Diffusion WebUI, fal.ai and Stability AI have one.")
                             : Text::tr("None of your image engines can remove a background. Stable Diffusion WebUI (with its "
                                        "background removal extension), fal.ai and Stability AI can.")};
    }
    case Step::Edit:
    case Step::Inpaint: {
        // A change of a picture: the engine for changes of Settings → AI (the image engine unless
        // another is chosen there), as for the editor's prompt bar and AI panel.
        if (!provider)
            provider = registry->imageProviderFor(true);
        if (!provider)
            return {nullptr, noEngine()};
        ImageRequest probe;
        probe.mode = step == Step::Edit ? GenMode::Edit : GenMode::Inpaint;
        const ImageCapabilities caps = provider->capabilitiesFor(probe);
        if (step == Step::Edit && !caps.edit)
            return {nullptr, Text::tr("“%1” cannot change a picture from a description. Choose an engine with an edit model for "
                                      "this step (FLUX.2 klein, FLUX.1 Kontext or Qwen-Image-Edit on this computer, or OpenAI, "
                                      "Gemini or fal.ai online), or use “Fill area with AI”.")
                                 .arg(provider->displayName())};
        if (step == Step::Inpaint && !caps.inpaint && !caps.edit && !caps.imageToImage)
            return {nullptr, Text::tr("“%1” cannot change part of a picture. Choose another engine for this step.")
                                 .arg(provider->displayName())};
        return {provider, {}};
    }
    }
    return {nullptr, noEngine()};
}

// What a node gives its job, copied on the worker that evaluates it.
struct StepInput
{
    Step step = Step::Edit;
    QString name;   // the node's (for the Queue)
    QString engine; // chosen provider id, empty: the one in use
    QImage image;   // ARGB32_Premultiplied
    QImage mask;    // the area at the image's size; null: the whole picture (Edit)
    QString prompt;
    double strength = 0.9;   // Inpaint
    double factor = 2.0;     // Upscale
    bool matchColors = true; // an area's colours are matched to the picture around it
};

// One AI step on its way: from the worker that evaluates the node to the GUI thread, which submits
// the job, and back through the promise. Everything but the promise's future is touched on the GUI
// thread only.
struct Pending
{
    QPromise<Values> promise;
    std::function<void(double, const QString &)> report;
    QPointer<Job> job;
    // The step's connections to its job. They hold this and the step's input, so they go once the
    // step is done: a finished job stays in the Queue until it is cleared.
    QList<QMetaObject::Connection> connections;
    bool cancelled = false;
    bool done = false; // the promise is finished: later news from the job is ignored

    void release()
    {
        for (const QMetaObject::Connection &c : std::as_const(connections))
            QObject::disconnect(c);
        connections.clear();
    }
    void finish(const Values &outputs)
    {
        if (done)
            return;
        done = true;
        release();
        promise.addResult(outputs);
        promise.finish();
    }
    void fail(const QString &message)
    {
        if (done)
            return;
        done = true;
        release();
        promise.setException(Error(message));
        promise.finish();
    }
    void finishCancelled()
    {
        if (done)
            return;
        done = true;
        release();
        promise.future().cancel();
        promise.finish();
    }
    void cancel()
    {
        cancelled = true;
        if (job && !job->isFinished())
            job->cancel(); // finishes the job as cancelled, which finishes the promise
        else
            finishCancelled();
    }
};

bool hasTransparency(const QImage &image)
{
    return image.hasAlphaChannel() && !img::maskIsEmpty(img::invertMask(img::maskFromAlpha(image)), 1);
}

// The node's outputs from its job's output. Worker thread.
Values resultOf(const StepInput &in, const GenerationOutput &out)
{
    const QImage generated = img::normalized(out.images.first());
    switch (in.step) {
    case Step::Edit:
    case Step::Inpaint: {
        if (!in.mask.isNull()) {
            // The region pipeline gives a layer of the picture's size, transparent outside the
            // feathered area: laid over the picture, the rest stays exactly as it was.
            QImage result = in.image.copy();
            QPainter p(&result);
            p.drawImage(QPoint(0, 0), img::scaled(generated, in.image.size(), true));
            p.end();
            return {{ImagePort, result}};
        }
        // The whole picture was edited at the model's working size: back to the picture's size,
        // keeping its transparent parts (the engine saw it on white).
        QImage result = img::normalized(img::scaled(generated, in.image.size(), true));
        if (hasTransparency(in.image))
            result = img::applyMaskToAlpha(result, img::maskFromAlpha(in.image));
        return {{ImagePort, result}};
    }
    case Step::Upscale: {
        const QSize target(std::max(1, int(std::lround(in.image.width() * in.factor))),
                           std::max(1, int(std::lround(in.image.height() * in.factor))));
        // An upscaler has a scale of its own (often 4×); the step gives the factor it was asked for.
        QImage result = generated.size() == target ? generated : img::normalized(img::scaled(generated, target, true));
        if (hasTransparency(in.image))
            result = img::applyMaskToAlpha(result, img::scaled(img::maskFromAlpha(in.image), target, true));
        return {{ImagePort, result}};
    }
    case Step::RemoveBackground: {
        QImage cut = generated.size() == in.image.size() ? generated : img::normalized(img::scaled(generated, in.image.size(), true));
        // What was transparent stays so (the engine saw the picture on white).
        if (hasTransparency(in.image))
            cut = img::applyMaskToAlpha(cut, img::maskFromAlpha(in.image));
        return {{ImagePort, cut}, {MaskPort, img::maskFromAlpha(cut)}};
    }
    }
    return {};
}

// Follows the step's job to the end. GUI thread.
void track(const std::shared_ptr<Pending> &pending, Job *job, const StepInput &in)
{
    pending->job = job;
    // Retry in the Queue would run the step again with nowhere for its result to go: the filter
    // runs again from the Filters panel instead.
    job->setRetryable(false);
    // A job removed from the Queue before it finished would leave the evaluation waiting.
    pending->connections << QObject::connect(job, &QObject::destroyed, QCoreApplication::instance(), [pending] {
        pending->fail(Text::tr("The step's job was removed from the Queue."));
    });
    pending->connections << QObject::connect(job, &Job::progressChanged, job, [pending](double fraction, const QString &status) {
        if (pending->report && !pending->done)
            pending->report(fraction, status);
    });
    pending->connections << QObject::connect(job, &Job::finished, job, [pending, in](Job *job) mutable {
        if (pending->done)
            return;
        // What the result needs moves out of this connection, which goes now (Qt keeps the
        // function alive until it returns).
        StepInput input = std::move(in);
        pending->release();
        if (job->state() == Job::State::Cancelled)
            return pending->finishCancelled();
        if (job->state() == Job::State::Failed)
            return pending->fail(job->errorString());
        const GenerationOutput out = GenerationService::outputOf(job);
        if (out.images.isEmpty() || out.images.first().isNull())
            return pending->fail(Text::tr("The image engine gave no picture."));
        async::run([input = std::move(input), out] { return resultOf(input, out); })
            .then(QCoreApplication::instance(), [pending](const Values &outputs) { pending->finish(outputs); })
            .onFailed(QCoreApplication::instance(), [pending](const std::exception &e) { pending->fail(describeException(e)); });
    });
}

QString queueTitle(const StepInput &in)
{
    switch (in.step) {
    case Step::Edit:
    case Step::Inpaint:
        return Text::tr("%1: %2").arg(in.name, in.prompt.left(48));
    case Step::Upscale:
        return Text::tr("%1 %2×").arg(in.name, QLocale().toString(in.factor, 'g', 3));
    case Step::RemoveBackground:
        return in.name;
    }
    return in.name;
}

// Submits the step's job. GUI thread.
void start(const std::shared_ptr<Pending> &pending, const StepInput &in)
{
    if (pending->done || pending->cancelled)
        return pending->finishCancelled();
    const Engine engine = engineFor(in.step, in.engine);
    if (!engine.provider)
        return pending->fail(engine.problem);
    GenerationService *service = filterNodeService();
    const QString providerId = engine.provider->instanceId();
    const QString title = queueTitle(in);
    switch (in.step) {
    case Step::Upscale:
        return track(pending,
                     service->submitUtility(providerId, Utility::Upscale, in.image, QJsonObject{{QStringLiteral("factor"), in.factor}},
                                            title, false),
                     in);
    case Step::RemoveBackground:
        return track(pending, service->submitUtility(providerId, Utility::RemoveBackground, img::flatten(in.image, Qt::white), {}, title, false),
                     in);
    case Step::Edit:
    case Step::Inpaint:
        break;
    }

    GenerationSpec spec;
    spec.providerId = providerId;
    spec.title = title;
    spec.assetKind = doc::AssetKind::Edit;
    spec.saveAssets = false;
    spec.meta = QJsonObject{{QStringLiteral("filter_step"), in.name}};
    spec.request.prompt = in.prompt;
    if (in.step == Step::Inpaint) {
        const PromptStyle style = promptStyleFor(engine.provider);
        spec.request.mode = GenMode::Inpaint;
        spec.request.prompt = style.apply(in.prompt);
        if (style.supportsNegative)
            spec.request.negativePrompt = style.defaultNegative;
        spec.request.denoise = in.strength;
    } else {
        spec.request.mode = GenMode::Edit;
    }
    if (!in.mask.isNull()) {
        RegionInput region{in.image, in.mask};
        region.colorMatch = in.matchColors;
        spec.region = region;
        return track(pending, service->submit(spec), in);
    }
    // The whole picture, at the model's working resolution (as the editor's AI panel sends it),
    // prepared on a worker; the job starts when it is ready.
    ImageRequest probe;
    probe.mode = GenMode::Edit;
    const ImageCapabilities caps = engine.provider->capabilitiesFor(probe);
    QPointer<GenerationService> guard(service);
    async::run([image = in.image, caps] { return wholeImageInput(image, caps); })
        .then(QCoreApplication::instance(), [pending, in, spec, guard](const QImage &initImage) mutable {
            if (pending->done || pending->cancelled)
                return pending->finishCancelled();
            if (!guard)
                return pending->fail(Text::tr("AI steps cannot run here: Lumen's image engines are not available."));
            spec.request.initImage = initImage;
            spec.request.size = initImage.size();
            track(pending, guard->submit(spec), in);
        })
        .onFailed(QCoreApplication::instance(), [pending](const std::exception &e) { pending->fail(describeException(e)); });
}

bool isFull(const QImage &mask)
{
    return img::maskIsEmpty(img::invertMask(mask), 1);
}

class AiNodeType final : public filters::NodeType
{
public:
    AiNodeType(Step step, filters::NodeTypeInfo info)
        : NodeType(std::move(info))
        , m_step(step)
    {
    }

    filters::NodeResult evaluate(const NodeArgs &args, filters::EvalContext &ctx) const override
    {
        StepInput in;
        in.step = m_step;
        in.name = info().name;
        in.engine = args.text(filter_nodes::EngineParam);
        in.image = args.image(ImagePort);
        switch (m_step) {
        case Step::Edit:
        case Step::Inpaint: {
            in.prompt = args.text(PromptParam).trimmed();
            if (in.prompt.isEmpty())
                throw Error(m_step == Step::Edit ? Text::tr("Describe the change first: its “Instruction” is empty.")
                                                 : Text::tr("Describe what should appear first: its “Description” is empty."));
            if (args.isConnected(MaskPort)) {
                QImage mask = args.mask(MaskPort);
                if (mask.size() != in.image.size())
                    mask = img::scaled(mask, in.image.size(), true);
                // Nothing to change (the region pipeline takes an area from 8 up, as the selection's
                // bounds do).
                if (img::maskIsEmpty(mask, 8))
                    return Values{{ImagePort, in.image}};
                // An area that covers everything (no selection) is an edit of the whole picture.
                if (m_step == Step::Inpaint || !isFull(mask))
                    in.mask = mask;
            }
            in.strength = args.number(QStringLiteral("strength"));
            in.matchColors = m_step == Step::Inpaint || args.text(QStringLiteral("colors")) != QLatin1String("keep");
            break;
        }
        case Step::Upscale:
            in.factor = args.number(QStringLiteral("factor"));
            break;
        case Step::RemoveBackground:
            break;
        }
        QCoreApplication *app = QCoreApplication::instance();
        if (!app)
            throw Error(Text::tr("AI steps cannot run here: Lumen's image engines are not available."));
        auto pending = std::make_shared<Pending>();
        pending->promise.start();
        pending->report = ctx.progressReporter();
        const QFuture<Values> future = pending->promise.future();
        // Both hops are queued to the GUI thread in this order, so a cancel never overtakes the start.
        ctx.onCancel([pending, app] { QMetaObject::invokeMethod(app, [pending] { pending->cancel(); }, Qt::QueuedConnection); });
        QMetaObject::invokeMethod(app, [pending, in] { start(pending, in); }, Qt::QueuedConnection);
        return filters::NodeResult(future);
    }

    QString unavailableReason(const QVariantMap &params) const override
    {
        return engineFor(m_step, params.value(filter_nodes::EngineParam).toString()).problem;
    }

private:
    Step m_step;
};

filters::PortSpec imagePort(const QString &label = Text::tr("Image"))
{
    return {ImagePort, label, PortType::Image, false};
}

filters::ParamSpec textParam(const QString &id, const QString &label, const QString &defaultValue, const QString &editor,
                             const QString &help)
{
    filters::ParamSpec p;
    p.id = id;
    p.label = label;
    p.type = PortType::Text;
    p.defaultValue = defaultValue;
    p.editor = editor;
    p.help = help;
    return p;
}

filters::ParamSpec engineParam()
{
    return textParam(filter_nodes::EngineParam, Text::tr("Image engine"), QString(), filter_nodes::EngineEditor,
                     Text::tr("Which of your image engines runs this step. “The one in use” is the one Settings → AI uses for "
                              "changes of a picture."));
}

filters::ParamSpec numberParam(const QString &id, const QString &label, double defaultValue, double minimum, double maximum,
                               int decimals, const QString &unit, const QString &help)
{
    filters::ParamSpec p;
    p.id = id;
    p.label = label;
    p.type = PortType::Number;
    p.defaultValue = defaultValue;
    p.minimum = minimum;
    p.maximum = maximum;
    p.decimals = decimals;
    p.unit = unit;
    p.help = help;
    return p;
}

filters::NodeTypeInfo describe(const QString &id, const QString &name, const QString &description)
{
    filters::NodeTypeInfo info;
    info.id = id;
    info.name = name;
    info.category = filters::categories::Ai;
    info.description = description;
    info.expensive = true;
    return info;
}

} // namespace

GenerationService *filterNodeService()
{
    QList<QPointer<GenerationService>> &list = services();
    list.removeIf([](const QPointer<GenerationService> &s) { return s.isNull(); });
    return list.isEmpty() ? nullptr : list.last().data();
}

void registerFilterNodes(GenerationService *service)
{
    filters::NodeRegistry &registry = filters::NodeRegistry::global();
    if (!registry.contains(filter_nodes::Edit)) {
        filters::NodeTypeInfo edit = describe(filter_nodes::Edit, Text::tr("Edit with AI"),
                                              Text::tr("Changes the picture as you describe it (“colorize it”, “make it night”). With "
                                                       "an area connected, only that area changes and the rest stays as it was."));
        edit.inputs = {imagePort(), {MaskPort, Text::tr("Area"), PortType::Mask, true}};
        edit.outputs = {imagePort()};
        filters::ParamSpec colors;
        colors.id = QStringLiteral("colors");
        colors.label = Text::tr("Colours of an area");
        colors.type = PortType::Choice;
        colors.defaultValue = QStringLiteral("match");
        colors.choices = {{QStringLiteral("match"), Text::tr("Match the picture around it")},
                          {QStringLiteral("keep"), Text::tr("As the AI made them")}};
        colors.help = Text::tr("With an area connected: “Match” removes the colour drift a model adds around an edit. Choose “As "
                               "the AI made them” for changes of colour on purpose, such as colorizing.");
        edit.params = {textParam(PromptParam, Text::tr("Instruction"), QString(), filter_nodes::PromptEditor,
                                 Text::tr("What to change, in your own words.")),
                       colors, engineParam()};
        registry.add(std::make_shared<AiNodeType>(Step::Edit, edit));

        filters::NodeTypeInfo inpaint = describe(filter_nodes::Inpaint, Text::tr("Fill area with AI"),
                                                 Text::tr("Paints the area anew from a description (“a vase of flowers”), blended "
                                                          "into what is around it."));
        inpaint.inputs = {imagePort(), {MaskPort, Text::tr("Area"), PortType::Mask, false}};
        inpaint.outputs = {imagePort()};
        inpaint.params = {textParam(PromptParam, Text::tr("Description"), QString(), filter_nodes::PromptEditor,
                                    Text::tr("What should appear in the area.")),
                          numberParam(QStringLiteral("strength"), Text::tr("How much to change"), 0.9, 0.1, 1.0, 2, QString(),
                                      Text::tr("Low values keep more of what is there; 1 paints the area anew.")),
                          engineParam()};
        registry.add(std::make_shared<AiNodeType>(Step::Inpaint, inpaint));

        filters::NodeTypeInfo upscale = describe(filter_nodes::Upscale, Text::tr("Upscale with AI"),
                                                 Text::tr("Makes the picture larger with an AI upscaler, which adds detail instead "
                                                          "of blurring it."));
        upscale.inputs = {imagePort()};
        upscale.outputs = {imagePort()};
        upscale.params = {numberParam(QStringLiteral("factor"), Text::tr("Factor"), 2.0, 1.0, 4.0, 1, QStringLiteral("×"),
                                      Text::tr("How many times larger the picture becomes.")),
                          engineParam()};
        registry.add(std::make_shared<AiNodeType>(Step::Upscale, upscale));

        filters::NodeTypeInfo background = describe(filter_nodes::RemoveBackground, Text::tr("Remove background"),
                                                    Text::tr("Cuts out the main subject: the background becomes transparent. "
                                                             "“Subject” is its outline as a mask."));
        background.inputs = {imagePort()};
        background.outputs = {imagePort(), {MaskPort, Text::tr("Subject"), PortType::Mask, false}};
        background.params = {engineParam()};
        registry.add(std::make_shared<AiNodeType>(Step::RemoveBackground, background));
    }
    if (service) {
        QList<QPointer<GenerationService>> &list = services();
        list.removeIf([service](const QPointer<GenerationService> &s) { return s.isNull() || s == service; });
        list.append(service);
    }
}

QList<filters::ChoiceOption> filterNodeEngines()
{
    QList<filters::ChoiceOption> out{{QString(), Text::tr("The one in use")}};
    if (GenerationService *service = filterNodeService()) {
        for (ImageProvider *p : service->registry()->imageProviders())
            out.append({p->instanceId(), p->displayName()});
    }
    return out;
}

QString filterNodeEngineName(const QString &id)
{
    for (const filters::ChoiceOption &o : filterNodeEngines()) {
        if (o.id == id)
            return o.label;
    }
    return Text::tr("an engine that is no longer set up");
}

} // namespace lumen::ai
