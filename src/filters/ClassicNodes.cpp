#include "filters/ClassicNodes.h"

#include "core/Error.h"
#include "core/ImageOps.h"
#include "filters/Ops.h"
#include "filters/Registry.h"

#include <QCoreApplication>

#include <cmath>

namespace lumen::filters {

namespace {

struct Text
{
    Q_DECLARE_TR_FUNCTIONS(lumen::filters::ClassicNodes)
};

const QString ImagePort = QStringLiteral("image");
const QString MaskPort = QStringLiteral("mask");

PortSpec imagePort(const QString &id = ImagePort, const QString &label = Text::tr("Image"), bool optional = false)
{
    return {id, label, PortType::Image, optional};
}

PortSpec maskPort(const QString &id = MaskPort, const QString &label = Text::tr("Mask"), bool optional = false)
{
    return {id, label, PortType::Mask, optional};
}

ParamSpec number(const QString &id, const QString &label, double defaultValue, double minimum, double maximum,
                 const QString &unit = {}, int decimals = 0, const QString &help = {})
{
    ParamSpec p;
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

// A length in pixels: scaled with the preview.
ParamSpec pixels(const QString &id, const QString &label, double defaultValue, double minimum, double maximum,
                 int decimals = 0, const QString &help = {})
{
    ParamSpec p = number(id, label, defaultValue, minimum, maximum, Text::tr("px"), decimals, help);
    p.pixels = true;
    return p;
}

ParamSpec choice(const QString &id, const QString &label, const QString &defaultValue, const QList<ChoiceOption> &choices,
                 const QString &help = {})
{
    ParamSpec p;
    p.id = id;
    p.label = label;
    p.type = PortType::Choice;
    p.defaultValue = defaultValue;
    p.choices = choices;
    p.help = help;
    return p;
}

ParamSpec channelsParam()
{
    return choice(QStringLiteral("channel"), Text::tr("Channels"), QStringLiteral("all"),
                  {{QStringLiteral("all"), Text::tr("All colours")},
                   {QStringLiteral("red"), Text::tr("Red")},
                   {QStringLiteral("green"), Text::tr("Green")},
                   {QStringLiteral("blue"), Text::tr("Blue")}});
}

ops::Channels channelsOf(const NodeArgs &args)
{
    const QString c = args.text(QStringLiteral("channel"));
    if (c == QLatin1String("red"))
        return ops::Channels::Red;
    if (c == QLatin1String("green"))
        return ops::Channels::Green;
    if (c == QLatin1String("blue"))
        return ops::Channels::Blue;
    return ops::Channels::All;
}

NodeTypeInfo describe(const QString &id, const QString &name, const QString &category, const QString &description)
{
    NodeTypeInfo info;
    info.id = id;
    info.name = name;
    info.category = category;
    info.description = description;
    return info;
}

// The common shape: one image in, one image out.
NodeTypeInfo imageFilter(const QString &id, const QString &name, const QString &category, const QString &description,
                         const QList<ParamSpec> &params = {})
{
    NodeTypeInfo info = describe(id, name, category, description);
    info.inputs = {imagePort()};
    info.outputs = {imagePort()};
    info.params = params;
    return info;
}

Values image(const QImage &result)
{
    return {{ImagePort, result}};
}

Values mask(const QImage &result)
{
    return {{MaskPort, result}};
}

void add(NodeRegistry &registry, NodeTypeInfo info, NodeFunction function)
{
    registry.add(makeNodeType(std::move(info), std::move(function)));
}

void addInputOutput(NodeRegistry &r)
{
    NodeTypeInfo input = describe(QStringLiteral("input"), Text::tr("Input"), categories::InputOutput,
                                  Text::tr("The picture the filter is applied to (the layer or the whole image) and "
                                           "the selection."));
    input.outputs = {imagePort(), maskPort(QStringLiteral("selection"), Text::tr("Selection"))};
    input.readsSource = true;
    add(r, input, [](const NodeArgs &, EvalContext &ctx) -> NodeResult {
        const QImage source = ctx.source();
        if (source.isNull())
            throw Error(Text::tr("There is no image to apply the filter to."));
        return Values{{ImagePort, source}, {QStringLiteral("selection"), ctx.selection()}};
    });

    NodeTypeInfo output = describe(QStringLiteral("output"), Text::tr("Output"), categories::InputOutput,
                                   Text::tr("The result of the filter. Connect a mask to apply the result only there."));
    output.inputs = {imagePort(), maskPort(MaskPort, Text::tr("Where to apply"), true)};
    output.isOutput = true;
    add(r, output, [](const NodeArgs &args, EvalContext &) -> NodeResult {
        const QImage image = args.image(ImagePort);
        Values v{{ImagePort, image}};
        // Stretched to the image, so the result's image and mask fit after a resize or rotation.
        if (args.isConnected(MaskPort))
            v.insert(MaskPort, img::scaled(args.mask(MaskPort), image.size(), true));
        return v;
    });
}

void addColorNodes(NodeRegistry &r)
{
    using namespace categories;
    add(r,
        imageFilter(QStringLiteral("levels"), Text::tr("Levels"), Color,
                    Text::tr("Sets the darkest and brightest values and the brightness of the midtones."),
                    {number(QStringLiteral("inBlack"), Text::tr("Input black"), 0, 0, 255, {}, 0,
                            Text::tr("Values up to this one become black.")),
                     number(QStringLiteral("inWhite"), Text::tr("Input white"), 255, 0, 255, {}, 0,
                            Text::tr("Values from this one up become white.")),
                     number(QStringLiteral("gamma"), Text::tr("Midtones"), 1, 0.1, 10, {}, 2,
                            Text::tr("Above 1 brightens the midtones, below 1 darkens them.")),
                     number(QStringLiteral("outBlack"), Text::tr("Output black"), 0, 0, 255),
                     number(QStringLiteral("outWhite"), Text::tr("Output white"), 255, 0, 255), channelsParam()}),
        [](const NodeArgs &a, EvalContext &) -> NodeResult {
            return image(ops::levels(a.image(ImagePort), channelsOf(a), a.number(QStringLiteral("inBlack")),
                                     a.number(QStringLiteral("inWhite")), a.number(QStringLiteral("gamma")),
                                     a.number(QStringLiteral("outBlack")), a.number(QStringLiteral("outWhite"))));
        });

    ParamSpec curve;
    curve.id = QStringLiteral("points");
    curve.label = Text::tr("Curve");
    curve.type = PortType::Text;
    curve.defaultValue = QStringLiteral("0 0, 255 255");
    curve.editor = QStringLiteral("curve");
    curve.help = Text::tr("Points as input and output values from 0 to 255, for example “0 0, 128 160, 255 255”.");
    add(r,
        imageFilter(QStringLiteral("curves"), Text::tr("Curves"), Color,
                    Text::tr("Maps every brightness value through a smooth curve."), {curve, channelsParam()}),
        [](const NodeArgs &a, EvalContext &) -> NodeResult {
            return image(ops::curves(a.image(ImagePort), channelsOf(a), ops::parseCurve(a.text(QStringLiteral("points")))));
        });

    add(r,
        imageFilter(QStringLiteral("brightness-contrast"), Text::tr("Brightness and contrast"), Color,
                    Text::tr("Makes the image brighter or darker, and its contrast stronger or weaker."),
                    {number(QStringLiteral("brightness"), Text::tr("Brightness"), 0, -100, 100),
                     number(QStringLiteral("contrast"), Text::tr("Contrast"), 0, -100, 100)}),
        [](const NodeArgs &a, EvalContext &) -> NodeResult {
            return image(ops::brightnessContrast(a.image(ImagePort), a.number(QStringLiteral("brightness")),
                                                 a.number(QStringLiteral("contrast"))));
        });

    add(r,
        imageFilter(QStringLiteral("hue-saturation"), Text::tr("Hue and saturation"), Color,
                    Text::tr("Turns the colours around the colour wheel and makes them stronger or paler."),
                    {number(QStringLiteral("hue"), Text::tr("Hue"), 0, -180, 180, Text::tr("°")),
                     number(QStringLiteral("saturation"), Text::tr("Saturation"), 0, -100, 100),
                     number(QStringLiteral("lightness"), Text::tr("Lightness"), 0, -100, 100)}),
        [](const NodeArgs &a, EvalContext &) -> NodeResult {
            return image(ops::hueSaturation(a.image(ImagePort), a.number(QStringLiteral("hue")),
                                            a.number(QStringLiteral("saturation")), a.number(QStringLiteral("lightness"))));
        });

    add(r,
        imageFilter(QStringLiteral("color-balance"), Text::tr("Colour balance"), Color,
                    Text::tr("Tints the shadows, midtones or highlights."),
                    {choice(QStringLiteral("tones"), Text::tr("Tones"), QStringLiteral("midtones"),
                            {{QStringLiteral("shadows"), Text::tr("Shadows")},
                             {QStringLiteral("midtones"), Text::tr("Midtones")},
                             {QStringLiteral("highlights"), Text::tr("Highlights")}}),
                     number(QStringLiteral("cyanRed"), Text::tr("Cyan – Red"), 0, -100, 100),
                     number(QStringLiteral("magentaGreen"), Text::tr("Magenta – Green"), 0, -100, 100),
                     number(QStringLiteral("yellowBlue"), Text::tr("Yellow – Blue"), 0, -100, 100),
                     choice(QStringLiteral("keepBrightness"), Text::tr("Keep brightness"), QStringLiteral("yes"),
                            {{QStringLiteral("yes"), Text::tr("Yes")}, {QStringLiteral("no"), Text::tr("No")}})}),
        [](const NodeArgs &a, EvalContext &) -> NodeResult {
            const QString t = a.text(QStringLiteral("tones"));
            const ops::Tones tones = t == QLatin1String("shadows")      ? ops::Tones::Shadows
                                   : t == QLatin1String("highlights") ? ops::Tones::Highlights
                                                                      : ops::Tones::Midtones;
            return image(ops::colorBalance(a.image(ImagePort), tones, a.number(QStringLiteral("cyanRed")),
                                           a.number(QStringLiteral("magentaGreen")), a.number(QStringLiteral("yellowBlue")),
                                           a.text(QStringLiteral("keepBrightness")) == QLatin1String("yes")));
        });

    add(r, imageFilter(QStringLiteral("invert"), Text::tr("Invert"), Color, Text::tr("Turns the image into its negative.")),
        [](const NodeArgs &a, EvalContext &) -> NodeResult { return image(ops::invert(a.image(ImagePort))); });

    add(r,
        imageFilter(QStringLiteral("desaturate"), Text::tr("Black and white"), Color,
                    Text::tr("Removes the colours and keeps the brightness."),
                    {choice(QStringLiteral("method"), Text::tr("Brightness from"), QStringLiteral("luminosity"),
                            {{QStringLiteral("luminosity"), Text::tr("How bright the eye sees each colour")},
                             {QStringLiteral("average"), Text::tr("Average of red, green and blue")},
                             {QStringLiteral("lightness"), Text::tr("Middle of the strongest and weakest colour")}})}),
        [](const NodeArgs &a, EvalContext &) -> NodeResult {
            const QString m = a.text(QStringLiteral("method"));
            const ops::GreyMethod method = m == QLatin1String("average")     ? ops::GreyMethod::Average
                                         : m == QLatin1String("lightness") ? ops::GreyMethod::Lightness
                                                                           : ops::GreyMethod::Luminosity;
            return image(ops::desaturate(a.image(ImagePort), method));
        });

    add(r,
        imageFilter(QStringLiteral("threshold"), Text::tr("Threshold"), Color,
                    Text::tr("Turns bright pixels white and the others black."),
                    {number(QStringLiteral("level"), Text::tr("Level"), 128, 0, 255, {}, 0,
                            Text::tr("Pixels at least this bright become white."))}),
        [](const NodeArgs &a, EvalContext &) -> NodeResult {
            return image(ops::threshold(a.image(ImagePort), a.integer(QStringLiteral("level"))));
        });

    add(r,
        imageFilter(QStringLiteral("posterize"), Text::tr("Posterize"), Color,
                    Text::tr("Reduces every colour channel to a few flat steps."),
                    {number(QStringLiteral("levels"), Text::tr("Steps per channel"), 4, 2, 64)}),
        [](const NodeArgs &a, EvalContext &) -> NodeResult {
            return image(ops::posterize(a.image(ImagePort), a.integer(QStringLiteral("levels"))));
        });
}

void addDetailNodes(NodeRegistry &r)
{
    using namespace categories;
    add(r,
        imageFilter(QStringLiteral("gaussian-blur"), Text::tr("Blur"), Detail, Text::tr("Softens the image evenly."),
                    {pixels(QStringLiteral("radius"), Text::tr("Radius"), 4, 0, 250, 1)}),
        [](const NodeArgs &a, EvalContext &ctx) -> NodeResult {
            return image(ops::gaussianBlur(a.image(ImagePort), a.number(QStringLiteral("radius")), ctx.cancelCheck()));
        });

    add(r,
        imageFilter(QStringLiteral("sharpen"), Text::tr("Sharpen"), Detail, Text::tr("Makes fine detail crisper."),
                    {number(QStringLiteral("amount"), Text::tr("Amount"), 100, 0, 500, Text::tr("%"))}),
        [](const NodeArgs &a, EvalContext &) -> NodeResult {
            return image(ops::sharpen(a.image(ImagePort), a.number(QStringLiteral("amount")) / 100.0));
        });

    add(r,
        imageFilter(QStringLiteral("unsharp-mask"), Text::tr("Unsharp mask"), Detail,
                    Text::tr("Sharpens edges by increasing the contrast along them."),
                    {number(QStringLiteral("amount"), Text::tr("Amount"), 100, 0, 500, Text::tr("%")),
                     pixels(QStringLiteral("radius"), Text::tr("Radius"), 2, 0.1, 250, 1),
                     number(QStringLiteral("threshold"), Text::tr("Threshold"), 0, 0, 255, {}, 0,
                            Text::tr("Edges with less contrast than this stay as they are."))}),
        [](const NodeArgs &a, EvalContext &ctx) -> NodeResult {
            return image(ops::unsharpMask(a.image(ImagePort), a.number(QStringLiteral("radius")),
                                          a.number(QStringLiteral("amount")) / 100.0, a.integer(QStringLiteral("threshold")),
                                          ctx.cancelCheck()));
        });

    add(r,
        imageFilter(QStringLiteral("noise"), Text::tr("Noise"), Detail, Text::tr("Adds film grain."),
                    {number(QStringLiteral("amount"), Text::tr("Amount"), 10, 0, 100, Text::tr("%")),
                     choice(QStringLiteral("type"), Text::tr("Grain"), QStringLiteral("color"),
                            {{QStringLiteral("color"), Text::tr("Coloured")}, {QStringLiteral("mono"), Text::tr("Grey")}}),
                     choice(QStringLiteral("distribution"), Text::tr("Spread"), QStringLiteral("gaussian"),
                            {{QStringLiteral("gaussian"), Text::tr("Soft (most grains small)")},
                             {QStringLiteral("uniform"), Text::tr("Even")}}),
                     number(QStringLiteral("seed"), Text::tr("Pattern"), 1, 0, 999999, {}, 0,
                            Text::tr("Another number gives another pattern of the same strength."))}),
        [](const NodeArgs &a, EvalContext &) -> NodeResult {
            return image(ops::addNoise(a.image(ImagePort), a.number(QStringLiteral("amount")) / 100.0,
                                       static_cast<quint32>(a.integer(QStringLiteral("seed"))),
                                       a.text(QStringLiteral("type")) == QLatin1String("mono"),
                                       a.text(QStringLiteral("distribution")) != QLatin1String("uniform")));
        });

    add(r,
        imageFilter(QStringLiteral("median"), Text::tr("Remove specks"), Detail,
                    Text::tr("Replaces each pixel by the middle value of its neighbours (a median filter)."),
                    {pixels(QStringLiteral("radius"), Text::tr("Radius"), 1, 1, 50)}),
        [](const NodeArgs &a, EvalContext &ctx) -> NodeResult {
            return image(ops::median(a.image(ImagePort), a.integer(QStringLiteral("radius")), ctx.cancelCheck()));
        });
}

void addCombineNodes(NodeRegistry &r)
{
    using namespace categories;
    // Labels as the Layers panel shows doc::blendModeNames().
    const QStringList labels{Text::tr("Normal"),      Text::tr("Multiply"),   Text::tr("Screen"),     Text::tr("Overlay"),
                             Text::tr("Darken"),      Text::tr("Lighten"),    Text::tr("Color Dodge"), Text::tr("Color Burn"),
                             Text::tr("Hard Light"),  Text::tr("Soft Light"), Text::tr("Difference"), Text::tr("Exclusion"),
                             Text::tr("Add")};
    QList<ChoiceOption> modes;
    const QStringList ids = ops::blendModeIds();
    for (qsizetype i = 0; i < ids.size(); ++i)
        modes.append({ids[i], labels.value(i)});

    NodeTypeInfo blend = describe(QStringLiteral("blend"), Text::tr("Blend"), Combine,
                                  Text::tr("Puts one image over another with a layer blend mode."));
    blend.inputs = {imagePort(QStringLiteral("base"), Text::tr("Background")),
                    imagePort(QStringLiteral("top"), Text::tr("Foreground")),
                    maskPort(MaskPort, Text::tr("Where"), true)};
    blend.outputs = {imagePort()};
    blend.params = {choice(QStringLiteral("mode"), Text::tr("Blend mode"), QStringLiteral("normal"), modes),
                    number(QStringLiteral("opacity"), Text::tr("Opacity"), 100, 0, 100, Text::tr("%"))};
    add(r, blend, [](const NodeArgs &a, EvalContext &) -> NodeResult {
        return image(ops::blend(a.image(QStringLiteral("base")), a.image(QStringLiteral("top")), a.text(QStringLiteral("mode")),
                                a.number(QStringLiteral("opacity")) / 100.0, a.mask(MaskPort)));
    });

    NodeTypeInfo mix = describe(QStringLiteral("mix"), Text::tr("Mix by mask"), Combine,
                                Text::tr("Takes one image where the mask is black and the other where it is white."));
    mix.inputs = {imagePort(QStringLiteral("outside"), Text::tr("Outside the mask")),
                  imagePort(QStringLiteral("inside"), Text::tr("Inside the mask")), maskPort()};
    mix.outputs = {imagePort()};
    add(r, mix, [](const NodeArgs &a, EvalContext &) -> NodeResult {
        return image(ops::mix(a.image(QStringLiteral("outside")), a.image(QStringLiteral("inside")), a.mask(MaskPort)));
    });
}

void addMaskNodes(NodeRegistry &r)
{
    using namespace categories;
    NodeTypeInfo fromImage = describe(QStringLiteral("mask-from-image"), Text::tr("Mask from image"), Mask,
                                      Text::tr("Makes a mask from the brightness or the opacity of an image."));
    fromImage.inputs = {imagePort()};
    fromImage.outputs = {maskPort()};
    fromImage.params = {choice(QStringLiteral("source"), Text::tr("From"), QStringLiteral("brightness"),
                               {{QStringLiteral("brightness"), Text::tr("Brightness (white is selected)")},
                                {QStringLiteral("alpha"), Text::tr("Opacity (opaque is selected)")}})};
    add(r, fromImage, [](const NodeArgs &a, EvalContext &) -> NodeResult {
        const QImage src = a.image(ImagePort);
        return mask(a.text(QStringLiteral("source")) == QLatin1String("alpha") ? img::maskFromAlpha(src)
                                                                                : img::maskFromLuminance(src));
    });

    NodeTypeInfo grow = describe(QStringLiteral("grow-shrink-feather"), Text::tr("Grow, shrink and feather"), Mask,
                                 Text::tr("Makes a mask larger or smaller and softens its edge."));
    grow.inputs = {maskPort()};
    grow.outputs = {maskPort()};
    grow.params = {pixels(QStringLiteral("grow"), Text::tr("Grow or shrink"), 0, -200, 200, 0,
                          Text::tr("Positive values grow the mask, negative values shrink it.")),
                   pixels(QStringLiteral("feather"), Text::tr("Feather"), 0, 0, 200)};
    add(r, grow, [](const NodeArgs &a, EvalContext &) -> NodeResult {
        return mask(ops::growShrinkFeather(a.mask(MaskPort), a.integer(QStringLiteral("grow")),
                                           a.integer(QStringLiteral("feather"))));
    });

    NodeTypeInfo invert = describe(QStringLiteral("invert-mask"), Text::tr("Invert mask"), Mask,
                                   Text::tr("Selects what was not selected."));
    invert.inputs = {maskPort()};
    invert.outputs = {maskPort()};
    add(r, invert, [](const NodeArgs &a, EvalContext &) -> NodeResult { return mask(img::invertMask(a.mask(MaskPort))); });
}

void addTransformNodes(NodeRegistry &r)
{
    using namespace categories;
    const QList<ChoiceOption> quality{{QStringLiteral("smooth"), Text::tr("Smooth")},
                                      {QStringLiteral("nearest"), Text::tr("Hard pixel edges")}};
    add(r,
        imageFilter(QStringLiteral("resize"), Text::tr("Resize"), Transform, Text::tr("Makes the image larger or smaller."),
                    {choice(QStringLiteral("by"), Text::tr("Size by"), QStringLiteral("percent"),
                            {{QStringLiteral("percent"), Text::tr("Percent")}, {QStringLiteral("size"), Text::tr("Pixels")}}),
                     number(QStringLiteral("percent"), Text::tr("Percent"), 50, 1, 800, Text::tr("%")),
                     pixels(QStringLiteral("width"), Text::tr("Width"), 1024, 1, 16384),
                     pixels(QStringLiteral("height"), Text::tr("Height"), 0, 0, 16384, 0,
                            Text::tr("0 keeps the proportions.")),
                     choice(QStringLiteral("quality"), Text::tr("Quality"), QStringLiteral("smooth"), quality)}),
        [](const NodeArgs &a, EvalContext &) -> NodeResult {
            const QImage src = a.image(ImagePort);
            QSize size;
            if (a.text(QStringLiteral("by")) == QLatin1String("size")) {
                const int w = std::max(1, a.integer(QStringLiteral("width")));
                // The height is a whole number before the preview scales it, so only 0 is "keep the
                // proportions"; a fixed height of 1 px may arrive as 0.25 and stays fixed.
                const int h = a.number(QStringLiteral("height")) > 0.0
                                ? std::max(1, a.integer(QStringLiteral("height")))
                                : std::max(1, static_cast<int>(std::lround(double(w) * src.height() / src.width())));
                size = {w, h};
            } else {
                const double f = a.number(QStringLiteral("percent")) / 100.0;
                size = {std::max(1, static_cast<int>(std::lround(src.width() * f))),
                        std::max(1, static_cast<int>(std::lround(src.height() * f)))};
            }
            return image(ops::resize(src, size, a.text(QStringLiteral("quality")) != QLatin1String("nearest")));
        });

    add(r,
        imageFilter(QStringLiteral("crop"), Text::tr("Crop"), Transform, Text::tr("Keeps a rectangle of the image."),
                    {pixels(QStringLiteral("x"), Text::tr("Left"), 0, 0, 16384),
                     pixels(QStringLiteral("y"), Text::tr("Top"), 0, 0, 16384),
                     pixels(QStringLiteral("width"), Text::tr("Width"), 512, 1, 16384),
                     pixels(QStringLiteral("height"), Text::tr("Height"), 512, 1, 16384)}),
        [](const NodeArgs &a, EvalContext &ctx) -> NodeResult {
            const QImage src = a.image(ImagePort);
            // The pixels the rectangle touches (whole numbers at full size; in a preview the edges
            // are scaled and the rectangle covers the preview pixels they fall in). The small
            // margin keeps 30 * 0.1 = 3.0000000000000004 from reaching the next pixel.
            constexpr double margin = 1e-6;
            const double x = a.number(QStringLiteral("x")), y = a.number(QStringLiteral("y"));
            int left = static_cast<int>(std::floor(x + margin));
            int top = static_cast<int>(std::floor(y + margin));
            const int right = static_cast<int>(std::ceil(x + a.number(QStringLiteral("width")) - margin));
            const int bottom = static_cast<int>(std::ceil(y + a.number(QStringLiteral("height")) - margin));
            // A rectangle from the last column or row at full size can start just past the edge of
            // the preview (8 px at 0.3 previews at 2 px, column 7 at 2.1): it keeps the last one.
            if (ctx.isPreview() && left == src.width())
                left = src.width() - 1;
            if (ctx.isPreview() && top == src.height())
                top = src.height() - 1;
            const QRect rect(left, top, std::max(1, right - left), std::max(1, bottom - top));
            const QImage out = ops::crop(src, rect);
            if (out.isNull())
                throw Error(Text::tr("The crop area is outside the image."));
            return image(out);
        });

    add(r,
        imageFilter(QStringLiteral("flip"), Text::tr("Flip"), Transform, Text::tr("Mirrors the image."),
                    {choice(QStringLiteral("direction"), Text::tr("Direction"), QStringLiteral("horizontal"),
                            {{QStringLiteral("horizontal"), Text::tr("Left to right")},
                             {QStringLiteral("vertical"), Text::tr("Upside down")},
                             {QStringLiteral("both"), Text::tr("Both")}})}),
        [](const NodeArgs &a, EvalContext &) -> NodeResult {
            const QString d = a.text(QStringLiteral("direction"));
            const Qt::Orientations o = d == QLatin1String("vertical") ? Qt::Vertical
                                     : d == QLatin1String("both")     ? (Qt::Horizontal | Qt::Vertical)
                                                                      : Qt::Horizontal;
            return image(ops::flip(a.image(ImagePort), o));
        });

    add(r,
        imageFilter(QStringLiteral("rotate"), Text::tr("Rotate"), Transform,
                    Text::tr("Turns the image clockwise; the canvas grows to hold the corners."),
                    {number(QStringLiteral("angle"), Text::tr("Angle"), 90, -360, 360, Text::tr("°"), 1),
                     choice(QStringLiteral("quality"), Text::tr("Quality"), QStringLiteral("smooth"), quality)}),
        [](const NodeArgs &a, EvalContext &) -> NodeResult {
            return image(ops::rotate(a.image(ImagePort), a.number(QStringLiteral("angle")),
                                     a.text(QStringLiteral("quality")) != QLatin1String("nearest")));
        });
}

} // namespace

void registerClassicNodes(NodeRegistry &registry)
{
    addInputOutput(registry);
    addColorNodes(registry);
    addDetailNodes(registry);
    addCombineNodes(registry);
    addMaskNodes(registry);
    addTransformNodes(registry);
}

} // namespace lumen::filters
