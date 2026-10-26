#include "ui/filters/FindAreaNode.h"

#include "core/Error.h"
#include "core/ImageOps.h"
#include "filters/Registry.h"
#include "ui/SmartSelect.h"

#include <QCoreApplication>
#include <QLocale>
#include <QPointer>
#include <QPromise>

namespace lumen::ui {

namespace {

const QString kId = QStringLiteral("find-area");
const QString kImage = QStringLiteral("image");
const QString kMask = QStringLiteral("mask");
const QString kWords = QStringLiteral("what");

QPointer<SmartSelect> &currentFinder()
{
    static QPointer<SmartSelect> finder;
    return finder;
}

struct Text
{
    Q_DECLARE_TR_FUNCTIONS(lumen::ui::FindAreaNode)
};

class FindAreaNodeType final : public filters::NodeType
{
public:
    using filters::NodeType::NodeType;

    filters::NodeResult evaluate(const filters::NodeArgs &args, filters::EvalContext &) const override
    {
        const QImage image = args.image(kImage);
        const QString words = args.text(kWords).trimmed();
        if (words.isEmpty())
            throw Error(Text::tr("Say what to find (\"the cup\", \"people\")."));
        if (const QString problem = unavailableReason({}); !problem.isEmpty())
            throw Error(problem);
        QCoreApplication *app = QCoreApplication::instance();
        auto promise = std::make_shared<QPromise<filters::Values>>();
        promise->start();
        const QFuture<filters::Values> future = promise->future();
        // The finder lives on the GUI thread; the answer comes back through the promise.
        QMetaObject::invokeMethod(app, [promise, image, words] {
            SmartSelect *finder = currentFinder();
            if (!finder) {
                promise->setException(Error(Text::tr("Finding objects is not available here.")));
                promise->finish();
                return;
            }
            finder->findObjects(image, words).then(finder, [promise](const SmartSelect::FoundArea &area) {
                if (area.mask.isNull())
                    promise->setException(Error(area.error));
                else
                    promise->addResult(filters::Values{{kMask, area.mask}});
                promise->finish();
            });
        }, Qt::QueuedConnection);
        return filters::NodeResult(future);
    }

    QString unavailableReason(const QVariantMap &) const override
    {
        SmartSelect *finder = currentFinder();
        if (!finder)
            return Text::tr("Finding objects is not available here.");
        if (!finder->canFindObjects())
            return Text::tr("Finding objects needs the detection models (about %1 MB, once), or an image engine that finds objects "
                            "(ComfyUI, fal.ai, Google Gemini). Select → By description… offers the download.")
                .arg(QLocale().toString(finder->objectsMissingSize() / 1e6, 'f', 0));
        return {};
    }
};

} // namespace

void registerFindAreaNode(SmartSelect *finder)
{
    currentFinder() = finder;
    filters::NodeRegistry &registry = filters::NodeRegistry::global();
    if (registry.contains(kId))
        return;
    filters::NodeTypeInfo info;
    info.id = kId;
    info.name = Text::tr("Find area");
    info.category = filters::categories::Mask;
    info.description = Text::tr("Selects the objects you name (\"the cup\", \"people\"), found and cut out by detection models on this "
                            "computer. Runs on Run, like the AI steps.");
    info.inputs = {{kImage, Text::tr("Image"), filters::PortType::Image, false}};
    info.outputs = {{kMask, Text::tr("Mask"), filters::PortType::Mask, false}};
    filters::ParamSpec what;
    what.id = kWords;
    what.label = Text::tr("What to find");
    what.type = filters::PortType::Text;
    what.defaultValue = QString();
    what.help = Text::tr("The objects to select, in a few words.");
    info.params = {what};
    info.expensive = true;
    registry.add(std::make_shared<FindAreaNodeType>(info));
}

} // namespace lumen::ui
