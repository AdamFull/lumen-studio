// lumen_gpu (plan M20 step 1): the device, textures, kernels from Slang's reflection, uploads,
// dispatches and read-backs, with barriers from the textures' states. Runs under Vulkan's validation
// layer where the Vulkan SDK provides it, and checks it said nothing; skips without a usable device.
#include "gpu/Compiler.h"
#include "gpu/Effects.h"
#include "gpu/Gpu.h"

#include <QElapsedTimer>
#include <QDir>
#include <QPainter>
#include <QFloat16>
#include <QRandomGenerator>
#include <QVector3D>
#include <QtTest>

#include <cmath>

using namespace lumen;
using namespace lumen::gpu;

class TestGpu : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void reflection();
    void unavailable();
    void roundTrip();
    void decodeEncode();
    void parameters();
    void dependentDispatches();
    void mistakesSayWhat();
    void largePicture();
    void effectFromShader();
    void effectValues();
    void chainRuns();
    void chainTwoPasses();
    void chainKeepsThePicture();
    void chainLater();
    void developEffect();
    void curveEffect();
    void lutEffect();
    void mixerEffect();
    void gradingEffect();
    void scaledTextures();
    void localEffect();
    void detailEffect();
    void finishEffect();
    void developAtFullSize();
    void localShadowsAndHighlights();
    void lookSheet();
    void compilerMessages();
    void compileOwnEffect();
    void compileMistakes();
    void cleanup();

private:
    bool m_available = false;
};

void TestGpu::initTestCase()
{
    // Before the device is made: the validation layer checks every call of these tests.
    qputenv("LUMEN_GPU_VALIDATE", "1");
    m_available = Context::instance().isAvailable();
    if (m_available)
        qInfo() << "GPU:" << Context::instance().deviceName();
    else
        qInfo() << "No GPU:" << Context::instance().unavailableReason();
}

void TestGpu::cleanup()
{
    QCOMPARE(Context::instance().validationMessages(), 0);
}

// The reflection slangc writes, read without a device.
void TestGpu::reflection()
{
    QFile f(QStringLiteral(":/gpu/gpu_test.fill.json"));
    QVERIFY(f.open(QIODevice::ReadOnly));
    QString error;
    const KernelInterface k = parseReflection(f.readAll(), QStringLiteral("fill"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(k.entry, QStringLiteral("fill"));
    QCOMPARE(k.groupSize[0], 8);
    QCOMPARE(k.groupSize[1], 8);
    QCOMPARE(k.bindings.value(QStringLiteral("target")), 0);
    QCOMPARE(k.formats.value(QStringLiteral("target")), QStringLiteral("rgba16f"));
    QVERIFY(k.param(QStringLiteral("color")));
    QCOMPARE(k.param(QStringLiteral("color"))->components, 4);
    QCOMPARE(k.param(QStringLiteral("shift"))->scalar, QStringLiteral("int32"));
    QCOMPARE(k.param(QStringLiteral("size"))->scalar, QStringLiteral("uint32"));
    QCOMPARE(k.param(QStringLiteral("size"))->components, 2);
    QVERIFY(k.pushSize >= 32);

    // slangc's octal escapes for text outside ASCII; an escaped backslash before digits stays.
    const QByteArray escaped = R"({"parameters": [{"name": "p", "binding": {"kind": "pushConstantBuffer", "index": 0}, "type": {"kind": "constantBuffer",
        "elementType": {"kind": "struct", "fields": [{"name": "gain", "type": {"kind": "scalar", "scalarType": "float32"},
        "binding": {"kind": "uniform", "offset": 0, "size": 4}, "userAttribs": [{"name": "Unit", "arguments": ["\303\227"]},
        {"name": "Label", "arguments": ["C:\\303 \"x\""]}]}]}}}],
        "entryPoints": [{"name": "main", "stage": "compute", "threadGroupSize": [16, 16, 1], "bindings": []}]})";
    const KernelInterface u = parseReflection(escaped, QStringLiteral("main"), &error);
    QCOMPARE(u.params.size(), 1);
    QCOMPARE(u.params.first().attributes.value(QStringLiteral("Unit")).value(0).toString(), QStringLiteral("\u00D7"));
    QCOMPARE(u.params.first().attributes.value(QStringLiteral("Label")).value(0).toString(), QStringLiteral("C:\\303 \"x\""));

    QVERIFY(parseReflection(f.readAll(), QStringLiteral("nothing"), &error).entry.isEmpty());
    QVERIFY(parseReflection("not json", QStringLiteral("fill"), &error).entry.isEmpty());
}

// Without a device everything says why, and nothing crashes.
void TestGpu::unavailable()
{
    Context::Options off;
    off.disabled = true;
    Context context(off);
    QVERIFY(!context.isAvailable());
    QVERIFY(!context.unavailableReason().isEmpty());
    QString error;
    QVERIFY(context.texture(QSize(4, 4), Format::RGBA8, &error).isNull());
    QCOMPARE(error, context.unavailableReason());
    QVERIFY(context.kernel(QStringLiteral("color.decode"), &error).isNull());
    Run run(context);
    QVERIFY(run.upload(QImage(4, 4, QImage::Format_ARGB32)).isNull());
    QVERIFY(!run.submit());
    QCOMPARE(run.error(), context.unavailableReason());
}

static QImage randomPicture(const QSize &size, bool opaque)
{
    QImage img(size, QImage::Format_RGBA8888);
    QRandomGenerator rng(11);
    for (int y = 0; y < size.height(); ++y) {
        uchar *line = img.scanLine(y);
        for (int x = 0; x < size.width() * 4; ++x)
            line[x] = uchar(rng.bounded(256));
        if (opaque) {
            for (int x = 0; x < size.width(); ++x)
                line[x * 4 + 3] = 255;
        }
    }
    return img;
}

void TestGpu::roundTrip()
{
    if (!m_available)
        QSKIP("no Vulkan device");
    const QImage picture = randomPicture(QSize(301, 157), false); // odd sizes: rows are not padded
    Run run;
    const Texture t = run.upload(picture);
    QCOMPARE(t.size(), picture.size());
    const int back = run.readBack(t);
    QVERIFY2(run.submit(), qPrintable(run.error()));
    QCOMPARE(run.image(back), picture);
}

// sRGB in, linear half floats, sRGB out: the 8-bit values come back, alpha too.
void TestGpu::decodeEncode()
{
    if (!m_available)
        QSKIP("no Vulkan device");
    Context &gpu = Context::instance();
    QString error;
    const Kernel decode = gpu.kernel(QStringLiteral("color.decode"), &error);
    QVERIFY2(!decode.isNull(), qPrintable(error));
    const Kernel encode = gpu.kernel(QStringLiteral("color.encode"), &error);
    QVERIFY2(!encode.isNull(), qPrintable(error));

    QImage picture = randomPicture(QSize(256, 64), false);
    // A known value: sRGB 188 is about 0.5 in linear light.
    picture.setPixelColor(0, 0, QColor(188, 188, 188, 255));
    Run run;
    const Texture in = run.upload(picture);
    const Texture light = gpu.texture(picture.size(), Format::RGBA16F);
    const Texture out = gpu.texture(picture.size(), Format::RGBA8);
    run.dispatch(decode, {{QStringLiteral("picture"), in}, {QStringLiteral("light"), light}}, {}, picture.size());
    run.dispatch(encode, {{QStringLiteral("picture"), out}, {QStringLiteral("light"), light}}, {{QStringLiteral("dither"), 0.0}},
                 picture.size());
    const int linear = run.readBack(light);
    const int back = run.readBack(out);
    QVERIFY2(run.submit(), qPrintable(run.error()));

    const QImage half = run.image(linear);
    QCOMPARE(half.format(), QImage::Format_RGBA16FPx4);
    QVERIFY(std::abs(half.pixelColor(0, 0).redF() - 0.5) < 0.01);
    const QImage result = run.image(back);
    int worst = 0;
    for (int y = 0; y < picture.height(); ++y) {
        const uchar *a = picture.constScanLine(y);
        const uchar *b = result.constScanLine(y);
        for (int x = 0; x < picture.width() * 4; ++x)
            worst = std::max(worst, std::abs(int(a[x]) - int(b[x])));
    }
    QVERIFY2(worst <= 1, qPrintable(QStringLiteral("off by %1").arg(worst)));
    QVERIFY(run.gpuMilliseconds() >= 0);
}

// Floats, ints and vectors by name; "size" is filled in from the threads.
void TestGpu::parameters()
{
    if (!m_available)
        QSKIP("no Vulkan device");
    Context &gpu = Context::instance();
    const Kernel fill = gpu.kernel(QStringLiteral("gpu_test.fill"));
    QVERIFY(!fill.isNull());
    const Texture t = gpu.texture(QSize(40, 24), Format::RGBA16F);
    Run run;
    run.dispatch(fill, {{QStringLiteral("target"), t}}, {{QStringLiteral("color"), QVariantList{0.25, 0.5, 0.75, 1.0}}, {QStringLiteral("shift"), 1}},
                 QSize(40, 24));
    const int back = run.readBack(t);
    QVERIFY2(run.submit(), qPrintable(run.error()));
    const QImage img = run.image(back);
    for (const QPoint at : {QPoint(0, 0), QPoint(39, 23), QPoint(17, 9)}) {
        // The half floats themselves: QColor takes no alpha above 1.
        const qfloat16 *px = reinterpret_cast<const qfloat16 *>(img.constScanLine(at.y())) + at.x() * 4;
        QCOMPARE(float(px[0]), 1.25f);
        QCOMPARE(float(px[1]), 1.5f);
        QCOMPARE(float(px[2]), 1.75f);
        QCOMPARE(float(px[3]), 2.0f);
    }
}

// Each dispatch reads what the one before wrote: without the barriers the sum would come out short
// (and the validation layer would say so).
void TestGpu::dependentDispatches()
{
    if (!m_available)
        QSKIP("no Vulkan device");
    Context &gpu = Context::instance();
    const Kernel fill = gpu.kernel(QStringLiteral("gpu_test.fill"));
    const Kernel add = gpu.kernel(QStringLiteral("gpu_test.add"));
    const QSize size(512, 512);
    const Texture t = gpu.texture(size, Format::RGBA16F);
    Run run;
    run.dispatch(fill, {{QStringLiteral("target"), t}}, {{QStringLiteral("color"), QVariantList{0, 0, 0, 1}}}, size);
    for (int i = 0; i < 40; ++i)
        run.dispatch(add, {{QStringLiteral("target"), t}}, {{QStringLiteral("amount"), 0.25}}, size);
    const int back = run.readBack(t);
    QVERIFY2(run.submit(), qPrintable(run.error()));
    const QImage img = run.image(back);
    QCOMPARE(img.pixelColor(0, 0).redF(), 10.0f);
    QCOMPARE(img.pixelColor(511, 511).blueF(), 10.0f);

    // The texture keeps its state across runs.
    Run again;
    again.dispatch(add, {{QStringLiteral("target"), t}}, {{QStringLiteral("amount"), 1.0}}, size);
    const int second = again.readBack(t);
    QVERIFY2(again.submit(), qPrintable(again.error()));
    QCOMPARE(again.image(second).pixelColor(100, 100).greenF(), 11.0f);
}

void TestGpu::mistakesSayWhat()
{
    if (!m_available)
        QSKIP("no Vulkan device");
    Context &gpu = Context::instance();
    QString error;
    QVERIFY(gpu.kernel(QStringLiteral("nothing.here"), &error).isNull());
    QVERIFY(error.contains(QLatin1String("nothing.here")));

    const Kernel fill = gpu.kernel(QStringLiteral("gpu_test.fill"));
    const Texture eight = gpu.texture(QSize(8, 8), Format::RGBA8);
    {
        Run run;
        run.dispatch(fill, {{QStringLiteral("target"), eight}}, {}, QSize(8, 8));
        QVERIFY(!run.submit());
        QVERIFY2(run.error().contains(QLatin1String("rgba16f")), qPrintable(run.error()));
    }
    const Texture half = gpu.texture(QSize(8, 8), Format::RGBA16F);
    {
        Run run;
        run.dispatch(fill, {}, {}, QSize(8, 8));
        QVERIFY(!run.submit());
        QVERIFY2(run.error().contains(QLatin1String("target")), qPrintable(run.error()));
    }
    {
        Run run;
        run.dispatch(fill, {{QStringLiteral("target"), half}}, {{QStringLiteral("brightness"), 1}}, QSize(8, 8));
        QVERIFY(!run.submit());
        QVERIFY2(run.error().contains(QLatin1String("brightness")), qPrintable(run.error()));
    }
    {
        Run run;
        run.dispatch(fill, {{QStringLiteral("target"), half}}, {{QStringLiteral("color"), 1.0}}, QSize(8, 8));
        QVERIFY(!run.submit());
        QVERIFY2(run.error().contains(QLatin1String("4 value")), qPrintable(run.error()));
    }
}

// 24 megapixels up, decoded, encoded and back.
void TestGpu::largePicture()
{
    if (!m_available)
        QSKIP("no Vulkan device");
    Context &gpu = Context::instance();
    const QSize size(6000, 4000);
    QImage picture(size, QImage::Format_ARGB32_Premultiplied);
    picture.fill(QColor(120, 160, 200));
    QElapsedTimer timer;
    timer.start();
    Run run;
    const Texture in = run.upload(picture);
    const Texture light = gpu.texture(size, Format::RGBA16F);
    run.dispatch(gpu.kernel(QStringLiteral("color.decode")), {{QStringLiteral("picture"), in}, {QStringLiteral("light"), light}}, {}, size);
    run.dispatch(gpu.kernel(QStringLiteral("color.encode")), {{QStringLiteral("picture"), in}, {QStringLiteral("light"), light}},
                 {{QStringLiteral("dither"), 1.0}, {QStringLiteral("seed"), 3}}, size);
    const int back = run.readBack(in);
    QVERIFY2(run.submit(), qPrintable(run.error()));
    const QImage result = run.image(back);
    qInfo() << "6000 x 4000 up, decoded, encoded and back:" << timer.elapsed() << "ms, on the GPU" << run.gpuMilliseconds() << "ms";
    QCOMPARE(result.size(), size);
    const QColor c = result.pixelColor(2999, 1999);
    QVERIFY(std::abs(c.red() - 120) <= 1 && std::abs(c.green() - 160) <= 1 && std::abs(c.blue() - 200) <= 1);
    QVERIFY(timer.elapsed() < 5000);
}

// The effect describes itself: passes, title, group, sliders, from the shader alone.
void TestGpu::effectFromShader()
{
    if (!m_available)
        QSKIP("no Vulkan device");
    const QStringList builtIn = Effect::builtIn();
    QVERIFY(builtIn.contains(QStringLiteral("gain")));
    QVERIFY(builtIn.contains(QStringLiteral("box")));
    QVERIFY(!builtIn.contains(QStringLiteral("color"))); // a shader, not an effect

    QString error;
    const Effect gain = Effect::load(QStringLiteral("gain"), Context::instance(), &error);
    QVERIFY2(!gain.isNull(), qPrintable(error));
    QCOMPARE(gain.title(), QStringLiteral("Gain"));
    QCOMPARE(gain.group(), QStringLiteral("Test"));
    QCOMPARE(gain.passes().size(), 1);
    const QList<EffectParam> params = gain.params();
    QCOMPARE(params.size(), 3);
    QCOMPARE(params[0].label, QStringLiteral("Gain"));
    QCOMPARE(params[0].unit, QStringLiteral("×"));
    QVERIFY(params[0].hasRange);
    QCOMPARE(params[0].minimum, 0.0);
    QCOMPARE(params[0].maximum, 4.0);
    QCOMPARE(params[0].defaults, QList<double>{1.0});
    QCOMPARE(params[1].components, 3);
    QCOMPARE(params[1].defaults, (QList<double>{0.0, 0.0, 0.0})); // no default: 0, inside the range
    QVERIFY(!params[2].shown());                                   // size
    QCOMPARE(gain.defaults().keys(), (QStringList{QStringLiteral("gain"), QStringLiteral("lift")}));

    const Effect box = Effect::load(QStringLiteral("box"), Context::instance(), &error);
    QVERIFY2(!box.isNull(), qPrintable(error));
    QCOMPARE(box.passes().size(), 2);
    QCOMPARE(box.passes()[0].info().entry, QStringLiteral("across"));
    QCOMPARE(box.params().first().scalar, QStringLiteral("int32"));

    QVERIFY(Effect::load(QStringLiteral("eightbit"), Context::instance(), &error).isNull());
    QVERIFY2(error.contains(QLatin1String("rgba16f")), qPrintable(error));
    QVERIFY(Effect::load(QStringLiteral("nothing"), Context::instance(), &error).isNull());
}

void TestGpu::effectValues()
{
    if (!m_available)
        QSKIP("no Vulkan device");
    const Effect gain = Effect::load(QStringLiteral("gain"));
    QString error;
    // Kept in range; the rest at its default.
    QVariantMap v = gain.resolve({{QStringLiteral("gain"), 9.0}}, &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(v.value(QStringLiteral("gain")).toDouble(), 4.0);
    QCOMPARE(v.value(QStringLiteral("lift")).toList().size(), 3);
    v = gain.resolve({{QStringLiteral("lift"), QVariantList{-5, 0.5, 2}}}, &error);
    QCOMPARE(v.value(QStringLiteral("lift")).toList(), (QVariantList{-1.0, 0.5, 1.0}));
    // A whole number for an int.
    const Effect box = Effect::load(QStringLiteral("box"));
    QCOMPARE(box.resolve({{QStringLiteral("radius"), 2.6}}).value(QStringLiteral("radius")).toDouble(), 3.0);
    // Mistakes are refused, with what is wrong.
    QVERIFY(gain.resolve({{QStringLiteral("size"), QVariantList{1, 1}}}, &error).isEmpty()); // not shown, not set
    QVERIFY(error.contains(QLatin1String("size")));
    QVERIFY(gain.resolve({{QStringLiteral("lift"), 0.5}}, &error).isEmpty());
    QVERIFY(error.contains(QLatin1String("3 value")));
    Chain chain;
    QVERIFY(!chain.setSteps({{QStringLiteral("gain"), {{QStringLiteral("brightness"), 1}}}}, &error));
    QVERIFY(error.contains(QLatin1String("brightness")));
    QVERIFY(!chain.setSteps({{QStringLiteral("nothing"), {}}}, &error));
}

static double toLinear(double c)
{
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

static double toSrgb(double c)
{
    c = std::clamp(c, 0.0, 1.0);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1 / 2.4) - 0.055;
}

// The effects work in linear light: a gain of 2 doubles the light, not the 8-bit value.
void TestGpu::chainRuns()
{
    if (!m_available)
        QSKIP("no Vulkan device");
    QImage picture(64, 32, QImage::Format_RGBA8888);
    picture.fill(QColor(128, 64, 32, 200));
    Chain chain;
    QString error;
    // No effects: the picture comes back (dithering moves a value by one at most).
    QVERIFY2(chain.setSteps({}, &error), qPrintable(error));
    QImage same = chain.render(picture);
    QVERIFY2(!same.isNull(), qPrintable(chain.error()));
    QVERIFY(std::abs(same.pixelColor(5, 5).red() - 128) <= 1);
    QCOMPARE(same.pixelColor(5, 5).alpha(), 200);

    QVERIFY(chain.setSteps({{QStringLiteral("gain"), {{QStringLiteral("gain"), 2.0}}},
                            {QStringLiteral("gain"), {{QStringLiteral("gain"), 4.0}}, false}}, // off: skipped
                           &error));
    const QImage out = chain.render(picture);
    QVERIFY2(!out.isNull(), qPrintable(chain.error()));
    QCOMPARE(out.size(), picture.size());
    const QColor c = out.pixelColor(10, 10);
    const int got[3] = {c.red(), c.green(), c.blue()};
    const int channels[3] = {128, 64, 32};
    for (int i = 0; i < 3; ++i) {
        const int expected = int(std::lround(toSrgb(2 * toLinear(channels[i] / 255.0)) * 255));
        QVERIFY2(std::abs(got[i] - expected) <= 1, qPrintable(QStringLiteral("%1 → %2, not %3").arg(channels[i]).arg(got[i]).arg(expected)));
    }
    QCOMPARE(c.alpha(), 200);
}

// A two-pass effect with a texture of its own between its passes.
void TestGpu::chainTwoPasses()
{
    if (!m_available)
        QSKIP("no Vulkan device");
    QImage picture(32, 32, QImage::Format_RGBA8888);
    picture.fill(Qt::black);
    picture.setPixelColor(10, 10, Qt::white);
    Chain chain;
    QVERIFY(chain.setSteps({{QStringLiteral("box"), {{QStringLiteral("radius"), 1}}}}));
    const QImage out = chain.render(picture);
    QVERIFY2(!out.isNull(), qPrintable(chain.error()));
    const int ninth = int(std::lround(toSrgb(1.0 / 9) * 255));
    for (int y = 9; y <= 11; ++y) {
        for (int x = 9; x <= 11; ++x)
            QVERIFY(std::abs(out.pixelColor(x, y).red() - ninth) <= 1);
    }
    QVERIFY(out.pixelColor(12, 10).red() <= 1);
    QVERIFY(out.pixelColor(10, 8).red() <= 1);
}

// A slider moved: the same picture is not sent again, and the result follows the new value.
void TestGpu::chainKeepsThePicture()
{
    if (!m_available)
        QSKIP("no Vulkan device");
    QImage picture(1200, 800, QImage::Format_RGBA8888);
    picture.fill(QColor(100, 100, 100));
    Chain chain;
    QVERIFY(chain.setSteps({{QStringLiteral("gain"), {{QStringLiteral("gain"), 1.5}}}}));
    const QImage first = chain.render(picture);
    QVERIFY(!chain.reusedPicture());
    QVERIFY(chain.setSteps({{QStringLiteral("gain"), {{QStringLiteral("gain"), 0.5}}}}));
    const QImage second = chain.render(picture);
    QVERIFY(chain.reusedPicture());
    QVERIFY(second.pixelColor(5, 5).red() < first.pixelColor(5, 5).red());
    QVERIFY(std::abs(second.pixelColor(5, 5).red() - int(std::lround(toSrgb(0.5 * toLinear(100 / 255.0)) * 255))) <= 1);

    // Another picture, or a change to this one, is sent again.
    QImage other = picture.copy();
    other.setPixelColor(0, 0, Qt::red);
    QVERIFY(!chain.render(other).isNull());
    QVERIFY(!chain.reusedPicture());
    const QImage smaller = picture.scaled(600, 400);
    QCOMPARE(chain.render(smaller).size(), QSize(600, 400));
    QVERIFY(!chain.reusedPicture());
}

// A preview now, the full size on a worker thread, each chain with its own textures.
void TestGpu::chainLater()
{
    if (!m_available)
        QSKIP("no Vulkan device");
    QImage full(4000, 3000, QImage::Format_RGBA8888);
    full.fill(QColor(90, 140, 190));
    const QList<EffectStep> steps{{QStringLiteral("gain"), {{QStringLiteral("gain"), 1.25}}},
                                  {QStringLiteral("box"), {{QStringLiteral("radius"), 2}}}};
    Chain preview;
    Chain finished;
    QVERIFY(preview.setSteps(steps));
    QVERIFY(finished.setSteps(steps));
    QFuture<QImage> later = finished.renderLater(full);
    const QImage small = preview.render(full.scaled(800, 600));
    QVERIFY2(!small.isNull(), qPrintable(preview.error()));
    QVERIFY(QTest::qWaitFor([&] { return later.isFinished(); }, 30000));
    const QImage big = later.result();
    QVERIFY2(!big.isNull(), qPrintable(finished.error()));
    QCOMPARE(big.size(), full.size());
    QVERIFY(std::abs(big.pixelColor(2000, 1500).blue() - small.pixelColor(400, 300).blue()) <= 1);
    qInfo() << "4000 x 3000 with two effects:" << finished.gpuMilliseconds() << "ms on the GPU; 800 x 600:" << preview.gpuMilliseconds()
            << "ms";
}

// The Develop panel's effect: all at 0 leaves the picture, each slider moves what it says.
void TestGpu::developEffect()
{
    if (!m_available)
        QSKIP("no Vulkan device");
    QString error;
    const Effect develop = Effect::load(QStringLiteral("develop"), Context::instance(), &error);
    QVERIFY2(!develop.isNull(), qPrintable(error));
    QCOMPARE(develop.title(), QStringLiteral("Develop"));
    QCOMPARE(develop.group(), QStringLiteral("Basic"));
    QCOMPARE(develop.defaults().size(), 10);
    QString sections;
    for (const EffectParam &p : develop.params()) {
        if (p.name == QLatin1String("temperature") || p.name == QLatin1String("exposure") || p.name == QLatin1String("saturation"))
            sections += p.section + u';';
    }
    QCOMPARE(sections, QStringLiteral("White balance;Tone;Presence;"));

    // Grey 128, a warm colour, dark grey, light greys, near black.
    const QList<QColor> colors{QColor(128, 128, 128), QColor(200, 120, 80), QColor(30, 30, 30), QColor(200, 200, 200),
                               QColor(230, 230, 230), QColor(10, 10, 10)};
    QImage picture(int(colors.size()), 1, QImage::Format_RGBA8888);
    for (int i = 0; i < colors.size(); ++i)
        picture.setPixelColor(i, 0, colors[i]);
    auto run = [&](const QVariantMap &values) {
        Chain chain;
        if (!chain.setSteps({{QStringLiteral("develop"), values}}))
            return QImage();
        return chain.render(picture);
    };
    auto at = [](const QImage &img, int x) { return img.pixelColor(x, 0); };

    const QImage neutral = run({});
    for (int i = 0; i < colors.size(); ++i) {
        const QColor c = at(neutral, i);
        QVERIFY2(std::abs(c.red() - colors[i].red()) <= 1 && std::abs(c.green() - colors[i].green()) <= 1
                     && std::abs(c.blue() - colors[i].blue()) <= 1,
                 qPrintable(QStringLiteral("%1 became %2").arg(colors[i].name(), c.name())));
    }

    // One stop more is twice the light.
    const QImage brighter = run({{QStringLiteral("exposure"), 1.0}});
    QVERIFY(std::abs(at(brighter, 0).red() - int(std::lround(toSrgb(2 * toLinear(128 / 255.0)) * 255))) <= 1);

    const QImage grey = run({{QStringLiteral("saturation"), -100.0}});
    const QColor g = at(grey, 1);
    QVERIFY2(std::abs(g.red() - g.blue()) <= 1 && std::abs(g.red() - g.green()) <= 1, qPrintable(g.name()));

    const QColor warm = at(run({{QStringLiteral("temperature"), 100.0}}), 0);
    QVERIFY2(warm.red() > warm.blue() + 10, qPrintable(warm.name()));
    const QColor cool = at(run({{QStringLiteral("temperature"), -100.0}}), 0);
    QVERIFY2(cool.blue() > cool.red() + 10, qPrintable(cool.name()));

    // Contrast: the middle stays, darks darker, lights lighter.
    const QImage contrast = run({{QStringLiteral("contrast"), 100.0}});
    QVERIFY(std::abs(at(contrast, 0).red() - 128) <= 3);
    QVERIFY(at(contrast, 2).red() < 30);
    QVERIFY(at(contrast, 3).red() > 200);
    const QImage flat = run({{QStringLiteral("contrast"), -100.0}});
    QVERIFY(at(flat, 2).red() > 30);
    QVERIFY(at(flat, 3).red() < 200);

    // Each tone slider moves its own part.
    QVERIFY(at(run({{QStringLiteral("highlights"), -100.0}}), 4).red() < 225);
    QVERIFY(std::abs(at(run({{QStringLiteral("highlights"), -100.0}}), 5).red() - 10) <= 1);
    QVERIFY(at(run({{QStringLiteral("shadows"), 100.0}}), 2).red() > 35);
    QVERIFY(std::abs(at(run({{QStringLiteral("shadows"), 100.0}}), 4).red() - 230) <= 1);
    QVERIFY(at(run({{QStringLiteral("blacks"), 100.0}}), 5).red() > 15);
    QVERIFY(at(run({{QStringLiteral("whites"), -100.0}}), 4).red() < 225);
    // Vibrance lifts the weak colour more than saturation's share.
    const QColor vivid = at(run({{QStringLiteral("vibrance"), 100.0}}), 1);
    QVERIFY(vivid.red() - vivid.blue() > 200 - 80);
}

// A picture of a few colours in a row.
static QImage swatches(const QList<QColor> &colors)
{
    QImage picture(int(colors.size()), 1, QImage::Format_RGBA8888);
    for (int i = 0; i < colors.size(); ++i)
        picture.setPixelColor(i, 0, colors[i]);
    return picture;
}

static bool near(const QColor &a, const QColor &b, int by = 1)
{
    return std::abs(a.red() - b.red()) <= by && std::abs(a.green() - b.green()) <= by && std::abs(a.blue() - b.blue()) <= by;
}

// The curves input: 256 x 1 half floats, from a function per channel (red, green, blue, all).
template<typename F>
static QImage curvePicture(F f)
{
    QImage image(256, 1, QImage::Format_RGBA16FPx4);
    auto *px = reinterpret_cast<qfloat16 *>(image.scanLine(0));
    for (int i = 0; i < 256; ++i) {
        for (int c = 0; c < 4; ++c)
            px[i * 4 + c] = qfloat16(f(c, i / 255.0f));
    }
    return image;
}

// A 3D LUT as the shader takes it (N² × N), from a function of the colour.
template<typename F>
static QImage lutPicture(int n, F f)
{
    QImage image(n * n, n, QImage::Format_RGBA16FPx4);
    for (int g = 0; g < n; ++g) {
        auto *px = reinterpret_cast<qfloat16 *>(image.scanLine(g));
        for (int b = 0; b < n; ++b) {
            for (int r = 0; r < n; ++r) {
                const QVector3D out = f(QVector3D(r, g, b) / float(n - 1));
                qfloat16 *at = px + (b * n + r) * 4;
                at[0] = qfloat16(out.x());
                at[1] = qfloat16(out.y());
                at[2] = qfloat16(out.z());
                at[3] = qfloat16(1.0f);
            }
        }
    }
    return image;
}

// The tone curve: straight leaves the picture, the curve for all turns it over, a channel's own
// curve moves only that channel; without its curves the step says what it needs.
void TestGpu::curveEffect()
{
    if (!m_available)
        QSKIP("no Vulkan device");
    QString error;
    const Effect curve = Effect::load(QStringLiteral("curve"), Context::instance(), &error);
    QVERIFY2(!curve.isNull(), qPrintable(error));
    QCOMPARE(curve.title(), QStringLiteral("Tone curve"));
    QCOMPARE(curve.inputs(), (QHash<QString, QString>{{QStringLiteral("curve"), QStringLiteral("curve")}}));

    const QImage picture = swatches({QColor(100, 100, 100), QColor(200, 120, 80), QColor(10, 240, 30)});
    Chain chain;
    QVERIFY(!chain.setSteps({{QStringLiteral("curve"), {}}}, &error));
    QVERIFY2(error.contains(QLatin1String("curve")), qPrintable(error));

    auto run = [&](const QImage &curves) {
        EffectStep step{QStringLiteral("curve"), {}};
        step.inputs.insert(QStringLiteral("curve"), curves);
        if (!chain.setSteps({step}, &error))
            return QImage();
        return chain.render(picture);
    };
    const QImage straight = run(curvePicture([](int, float x) { return x; }));
    QVERIFY2(!straight.isNull(), qPrintable(error + chain.error()));
    for (int i = 0; i < picture.width(); ++i)
        QVERIFY2(near(straight.pixelColor(i, 0), picture.pixelColor(i, 0)), qPrintable(straight.pixelColor(i, 0).name()));

    const QImage over = run(curvePicture([](int c, float x) { return c == 3 ? 1.0f - x : x; }));
    QVERIFY2(near(over.pixelColor(0, 0), QColor(155, 155, 155)), qPrintable(over.pixelColor(0, 0).name()));
    QVERIFY2(near(over.pixelColor(1, 0), QColor(55, 135, 175)), qPrintable(over.pixelColor(1, 0).name()));

    // Red off: red goes, green and blue stay.
    const QImage noRed = run(curvePicture([](int c, float x) { return c == 0 ? 0.0f : x; }));
    QVERIFY2(near(noRed.pixelColor(1, 0), QColor(0, 120, 80)), qPrintable(noRed.pixelColor(1, 0).name()));
    // Other curves on the same chain: the new ones are used.
    const QImage fullBlue = run(curvePicture([](int c, float x) { return c == 2 ? 1.0f : x; }));
    QVERIFY2(near(fullBlue.pixelColor(1, 0), QColor(200, 120, 255)), qPrintable(fullBlue.pixelColor(1, 0).name()));
}

// A LUT: one that changes nothing leaves the picture; one that swaps red and blue swaps them; Amount
// mixes it in.
void TestGpu::lutEffect()
{
    if (!m_available)
        QSKIP("no Vulkan device");
    QString error;
    const Effect lut = Effect::load(QStringLiteral("lut"), Context::instance(), &error);
    QVERIFY2(!lut.isNull(), qPrintable(error));
    QCOMPARE(lut.inputs().value(QStringLiteral("lut")), QStringLiteral("lut"));
    QCOMPARE(lut.defaults().value(QStringLiteral("amount")).toDouble(), 100.0);
    const QImage picture = swatches({QColor(100, 100, 100), QColor(200, 120, 80), QColor(10, 240, 30), QColor(255, 255, 255)});
    Chain chain;
    auto run = [&](const QImage &table, double amount) {
        EffectStep step{QStringLiteral("lut"), {{QStringLiteral("amount"), amount}}};
        step.inputs.insert(QStringLiteral("lut"), table);
        if (!chain.setSteps({step}, &error))
            return QImage();
        return chain.render(picture);
    };
    for (int n : {2, 17, 33}) {
        const QImage same = run(lutPicture(n, [](const QVector3D &c) { return c; }), 100);
        QVERIFY2(!same.isNull(), qPrintable(error + chain.error()));
        for (int i = 0; i < picture.width(); ++i)
            QVERIFY2(near(same.pixelColor(i, 0), picture.pixelColor(i, 0)),
                     qPrintable(QStringLiteral("%1: %2").arg(n).arg(same.pixelColor(i, 0).name())));
    }
    const auto swap = [](const QVector3D &c) { return QVector3D(c.z(), c.y(), c.x()); };
    const QImage swapped = run(lutPicture(17, swap), 100);
    QVERIFY2(near(swapped.pixelColor(1, 0), QColor(80, 120, 200)), qPrintable(swapped.pixelColor(1, 0).name()));
    QVERIFY2(near(swapped.pixelColor(2, 0), QColor(30, 240, 10)), qPrintable(swapped.pixelColor(2, 0).name()));
    const QImage none = run(lutPicture(17, swap), 0);
    QVERIFY(near(none.pixelColor(1, 0), QColor(200, 120, 80)));
}

// The colour mixer: at 0 nothing changes; each colour's sliders move that colour, not the others,
// and greys stay grey.
void TestGpu::mixerEffect()
{
    if (!m_available)
        QSKIP("no Vulkan device");
    QString error;
    const Effect mixer = Effect::load(QStringLiteral("mixer"), Context::instance(), &error);
    QVERIFY2(!mixer.isNull(), qPrintable(error));
    QCOMPARE(mixer.defaults().size(), 24);
    // Red, blue, grey, green.
    const QImage picture = swatches({QColor(200, 40, 40), QColor(40, 60, 200), QColor(128, 128, 128), QColor(40, 180, 60)});
    auto run = [&](const QVariantMap &values) {
        Chain chain;
        if (!chain.setSteps({{QStringLiteral("mixer"), values}}, &error))
            return QImage();
        return chain.render(picture);
    };
    const QImage neutral = run({});
    QVERIFY2(!neutral.isNull(), qPrintable(error));
    for (int i = 0; i < picture.width(); ++i)
        QVERIFY2(near(neutral.pixelColor(i, 0), picture.pixelColor(i, 0)), qPrintable(neutral.pixelColor(i, 0).name()));

    const QImage paleRed = run({{QStringLiteral("satRed"), -100.0}});
    const QColor r = paleRed.pixelColor(0, 0);
    QVERIFY2(r.red() - r.blue() < 30, qPrintable(r.name()));
    QVERIFY2(near(paleRed.pixelColor(1, 0), picture.pixelColor(1, 0), 2), qPrintable(paleRed.pixelColor(1, 0).name()));
    QVERIFY(near(paleRed.pixelColor(2, 0), picture.pixelColor(2, 0)));

    // Red's hue towards orange: more green in it.
    const QColor orange = run({{QStringLiteral("hueRed"), 100.0}}).pixelColor(0, 0);
    QVERIFY2(orange.green() > 60, qPrintable(orange.name()));

    const QImage darkBlue = run({{QStringLiteral("lumBlue"), -100.0}});
    QVERIFY2(darkBlue.pixelColor(1, 0).lightness() < picture.pixelColor(1, 0).lightness() - 10,
             qPrintable(darkBlue.pixelColor(1, 0).name()));
    QVERIFY2(near(darkBlue.pixelColor(3, 0), picture.pixelColor(3, 0), 2), qPrintable(darkBlue.pixelColor(3, 0).name()));
    QVERIFY(near(darkBlue.pixelColor(2, 0), picture.pixelColor(2, 0)));
}

// Colour grading: no tint leaves the picture; a tint in the shadows colours the darks more than the
// lights, one in the highlights the other way round; Lightness moves its range.
void TestGpu::gradingEffect()
{
    if (!m_available)
        QSKIP("no Vulkan device");
    QString error;
    const Effect grading = Effect::load(QStringLiteral("grading"), Context::instance(), &error);
    QVERIFY2(!grading.isNull(), qPrintable(error));
    int wheels = 0;
    for (const EffectParam &p : grading.params())
        wheels += p.control == QLatin1String("wheel") && p.components == 2;
    QCOMPARE(wheels, 3);
    // A wheel's hue goes round, its saturation stops at 100.
    const QVariantMap resolved = grading.resolve({{QStringLiteral("shadows"), QVariantList{370.0, 150.0}}}, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QVariantList shadows = resolved.value(QStringLiteral("shadows")).toList();
    QCOMPARE(shadows.value(0).toDouble(), 10.0);
    QCOMPARE(shadows.value(1).toDouble(), 100.0);

    const QImage picture = swatches({QColor(30, 30, 30), QColor(128, 128, 128), QColor(225, 225, 225), QColor(200, 120, 80)});
    auto run = [&](const QVariantMap &values) {
        Chain chain;
        if (!chain.setSteps({{QStringLiteral("grading"), values}}, &error))
            return QImage();
        return chain.render(picture);
    };
    const QImage neutral = run({});
    QVERIFY2(!neutral.isNull(), qPrintable(error));
    for (int i = 0; i < picture.width(); ++i)
        QVERIFY2(near(neutral.pixelColor(i, 0), picture.pixelColor(i, 0)), qPrintable(neutral.pixelColor(i, 0).name()));

    auto redness = [](const QColor &c) { return c.red() - c.blue(); };
    auto blueness = [](const QColor &c) { return c.blue() - c.red(); };
    const QImage redShadows = run({{QStringLiteral("shadows"), QVariantList{0.0, 100.0}}});
    QVERIFY2(redness(redShadows.pixelColor(0, 0)) > 5, qPrintable(redShadows.pixelColor(0, 0).name()));
    QVERIFY(redness(redShadows.pixelColor(0, 0)) > redness(redShadows.pixelColor(2, 0)));
    const QImage blueHighlights = run({{QStringLiteral("highlights"), QVariantList{240.0, 100.0}}});
    QVERIFY2(blueness(blueHighlights.pixelColor(2, 0)) > 5, qPrintable(blueHighlights.pixelColor(2, 0).name()));
    QVERIFY(blueness(blueHighlights.pixelColor(2, 0)) > blueness(blueHighlights.pixelColor(0, 0)));
    // Green midtones: the middle grey turns green.
    const QColor middle = run({{QStringLiteral("midtones"), QVariantList{120.0, 100.0}}}).pixelColor(1, 0);
    QVERIFY2(middle.green() > middle.red() + 5 && middle.green() > middle.blue() + 5, qPrintable(middle.name()));

    const QImage lighterShadows = run({{QStringLiteral("shadowLightness"), 100.0}});
    QVERIFY2(lighterShadows.pixelColor(0, 0).red() > 35, qPrintable(lighterShadows.pixelColor(0, 0).name()));
    QVERIFY2(std::abs(lighterShadows.pixelColor(2, 0).red() - 225) <= 3, qPrintable(lighterShadows.pixelColor(2, 0).name()));
    const QImage darkerHighlights = run({{QStringLiteral("highlightLightness"), -100.0}});
    QVERIFY2(darkerHighlights.pixelColor(2, 0).red() < 220, qPrintable(darkerHighlights.pixelColor(2, 0).name()));
}

// A grey picture of `size` with a value per pixel from a function (0..255), alpha full.
template<typename F>
static QImage greyPicture(const QSize &size, F f)
{
    QImage image(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        auto *line = image.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            const int v = std::clamp(int(std::lround(f(x, y))), 0, 255);
            line[x * 4] = line[x * 4 + 1] = line[x * 4 + 2] = uchar(v);
            line[x * 4 + 3] = 255;
        }
    }
    return image;
}

// Mean and spread of a channel over a rectangle.
static QPair<double, double> spread(const QImage &image, const QRect &r, int channel = 0)
{
    double sum = 0, squares = 0;
    int n = 0;
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    for (int y = r.top(); y <= r.bottom(); ++y) {
        const uchar *line = rgba.constScanLine(y);
        for (int x = r.left(); x <= r.right(); ++x) {
            const double v = line[x * 4 + channel];
            sum += v;
            squares += v * v;
            ++n;
        }
    }
    const double mean = sum / n;
    return {mean, std::sqrt(std::max(0.0, squares / n - mean * mean))};
}

static QImage runOne(const QString &effect, const QVariantMap &values, const QImage &picture, QString *error = nullptr)
{
    Chain chain;
    if (!chain.setSteps({{effect, values}}, error))
        return {};
    const QImage out = chain.render(picture);
    if (out.isNull() && error)
        *error = chain.error();
    return out;
}

// Textures smaller than the picture: n times, a long side, a fixed size; passes over them.
void TestGpu::scaledTextures()
{
    if (!m_available)
        QSKIP("no Vulkan device");
    QString error;
    const Effect local = Effect::load(QStringLiteral("local"), Context::instance(), &error);
    QVERIFY2(!local.isNull(), qPrintable(error));
    QCOMPARE(local.textureSize(QStringLiteral("fine"), QSize(6000, 4000)), QSize(1024, 683));
    QCOMPARE(local.textureSize(QStringLiteral("coarse"), QSize(6000, 4000)), QSize(256, 171));
    QCOMPARE(local.textureSize(QStringLiteral("coarse"), QSize(3000, 6000)), QSize(128, 256));
    // Never larger than the picture.
    QCOMPARE(local.textureSize(QStringLiteral("fine"), QSize(800, 600)), QSize(800, 600));
    QCOMPARE(local.textureSize(QStringLiteral("air"), QSize(800, 600)), QSize(1, 1));
    QCOMPARE(local.passSize(0, QSize(6000, 4000)), QSize(1024, 683));
    QCOMPARE(local.passSize(local.passes().size() - 1, QSize(6000, 4000)), QSize(6000, 4000));
    const Effect detail = Effect::load(QStringLiteral("detail"), Context::instance(), &error);
    QVERIFY2(!detail.isNull(), qPrintable(error));
    QCOMPARE(detail.textureSize(QStringLiteral("colour"), QSize(801, 600)), QSize(201, 150));
    QCOMPARE(detail.textureSize(QStringLiteral("blurred"), QSize(801, 600)), QSize(801, 600));
    // A picture of one pixel still runs.
    const QImage dot = runOne(QStringLiteral("local"), {{QStringLiteral("clarity"), 50.0}, {QStringLiteral("dehaze"), 50.0}},
                              swatches({QColor(100, 120, 140)}), &error);
    QVERIFY2(!dot.isNull(), qPrintable(error));
}

// Texture, Clarity and Dehaze: at 0 nothing changes and a flat picture stays flat whatever they say;
// more raises the picture's local contrast, less lowers it; Clarity on the preview's size agrees
// with the full size made small.
void TestGpu::localEffect()
{
    if (!m_available)
        QSKIP("no Vulkan device");
    QString error;
    const QSize size(256, 192);
    const QRect inside(16, 16, 224, 160);
    const QImage stripes = greyPicture(size, [](int x, int y) { return 128 + 40 * std::sin(x * 2 * M_PI / 12) * std::cos(y * 2 * M_PI / 16); });
    const QImage neutral = runOne(QStringLiteral("local"), {}, stripes, &error);
    QVERIFY2(!neutral.isNull(), qPrintable(error));
    QVERIFY(std::abs(spread(neutral, inside).second - spread(stripes, inside).second) < 0.5);
    QVERIFY(std::abs(neutral.pixelColor(40, 40).red() - stripes.pixelColor(40, 40).red()) <= 1);

    const QImage flat = greyPicture(size, [](int, int) { return 128; });
    const QImage flatOut = runOne(QStringLiteral("local"),
                                  {{QStringLiteral("texture"), 100.0}, {QStringLiteral("clarity"), 100.0}, {QStringLiteral("dehaze"), 100.0}}, flat);
    QVERIFY2(std::abs(spread(flatOut, inside).first - 128) < 1.5 && spread(flatOut, inside).second < 1.0,
             qPrintable(QStringLiteral("%1 ± %2").arg(spread(flatOut, inside).first).arg(spread(flatOut, inside).second)));

    const double base = spread(stripes, inside).second;
    for (const QString &name : {QStringLiteral("texture"), QStringLiteral("clarity")}) {
        const double more = spread(runOne(QStringLiteral("local"), {{name, 100.0}}, stripes), inside).second;
        const double less = spread(runOne(QStringLiteral("local"), {{name, -100.0}}, stripes), inside).second;
        QVERIFY2(more > base * 1.1 && less < base * 0.95, qPrintable(QStringLiteral("%1: %2 / %3 / %4").arg(name).arg(less).arg(base).arg(more)));
    }

    // A hazy picture: dehazing spreads it and takes the haze's grey out of the darks.
    const QImage hazy = greyPicture(size, [](int x, int y) {
        const double scene = (x / 32 + y / 32) % 2 ? 200.0 : 40.0;
        return 0.45 * scene + 0.55 * 220.0;
    });
    const auto hazed = spread(hazy, inside);
    const QImage clear = runOne(QStringLiteral("local"), {{QStringLiteral("dehaze"), 100.0}}, hazy);
    const auto cleared = spread(clear, inside);
    QVERIFY2(cleared.second > hazed.second * 1.3, qPrintable(QStringLiteral("%1 → %2").arg(hazed.second).arg(cleared.second)));
    // A dark square of the scene.
    QVERIFY2(clear.pixelColor(16, 16).red() < hazy.pixelColor(16, 16).red() - 15,
             qPrintable(QStringLiteral("%1 → %2").arg(hazy.pixelColor(16, 16).red()).arg(clear.pixelColor(16, 16).red())));
    const QImage hazier = runOne(QStringLiteral("local"), {{QStringLiteral("dehaze"), -100.0}}, hazy);
    QVERIFY(spread(hazier, inside).second < hazed.second * 0.8);

    // The same picture at 1024 and at 256 wide: Clarity's change agrees once the large one is small.
    auto blobs = [](double w, double h) {
        return [w, h](int x, int y) {
            const double u = x / w, v = y / h;
            return 120 + 70 * std::exp(-(std::pow(u - 0.3, 2) + std::pow(v - 0.4, 2)) / 0.0012)
                   - 60 * std::exp(-(std::pow(u - 0.65, 2) + std::pow(v - 0.6, 2)) / 0.0018);
        };
    };
    const QImage big = greyPicture(QSize(1024, 768), blobs(1024, 768));
    const QImage small = greyPicture(QSize(256, 192), blobs(256, 192));
    const QVariantMap clarity{{QStringLiteral("clarity"), 100.0}};
    const QImage bigOut = runOne(QStringLiteral("local"), clarity, big).scaled(256, 192, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const QImage smallOut = runOne(QStringLiteral("local"), clarity, small);
    const QImage bigIn = big.scaled(256, 192, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    // At the light blob's middle and the dark one's: Clarity moved each as much on both sizes.
    for (const QPoint at : {QPoint(77, 77), QPoint(166, 115)}) {
        const int large = bigOut.pixelColor(at).red() - bigIn.pixelColor(at).red();
        const int preview = smallOut.pixelColor(at).red() - small.pixelColor(at).red();
        QVERIFY2(std::abs(large) >= 6 && std::abs(preview - large) <= std::max(2, std::abs(large) / 4),
                 qPrintable(QStringLiteral("full size %1, preview %2").arg(large).arg(preview)));
    }
}

// Sharpening raises an edge's contrast, Masking keeps it off a nearly flat part; noise reduction
// calms the noise and keeps the edge; colour noise reduction calms the colour.
void TestGpu::detailEffect()
{
    if (!m_available)
        QSKIP("no Vulkan device");
    QString error;
    const QSize size(128, 64);
    const QImage edge = greyPicture(size, [](int x, int) { return x < 64 ? 60.0 : 180.0; });
    const QImage neutral = runOne(QStringLiteral("detail"), {}, edge, &error);
    QVERIFY2(!neutral.isNull(), qPrintable(error));
    for (int x : {10, 62, 63, 64, 65, 100})
        QVERIFY(std::abs(neutral.pixelColor(x, 30).red() - edge.pixelColor(x, 30).red()) <= 1);
    const QImage sharp = runOne(QStringLiteral("detail"), {{QStringLiteral("sharpen"), 100.0}}, edge);
    QVERIFY2(sharp.pixelColor(64, 30).red() > 185 && sharp.pixelColor(63, 30).red() < 55,
             qPrintable(QStringLiteral("%1 %2").arg(sharp.pixelColor(63, 30).red()).arg(sharp.pixelColor(64, 30).red())));
    QVERIFY(std::abs(sharp.pixelColor(10, 30).red() - 60) <= 1);

    // Faint noise: sharpening lifts it, unless masked.
    QRandomGenerator random(7);
    QList<int> noise;
    for (int i = 0; i < size.width() * size.height(); ++i)
        noise << random.bounded(-12, 13);
    const QImage noisy = greyPicture(size, [&](int x, int y) { return 128 + noise[y * size.width() + x]; });
    const QRect all(4, 4, 120, 56);
    const double before = spread(noisy, all).second;
    const double sharpened = spread(runOne(QStringLiteral("detail"), {{QStringLiteral("sharpen"), 150.0}}, noisy), all).second;
    const double masked = spread(runOne(QStringLiteral("detail"), {{QStringLiteral("sharpen"), 150.0}, {QStringLiteral("masking"), 100.0}}, noisy), all).second;
    QVERIFY2(sharpened > before * 1.2 && masked < sharpened * 0.8, qPrintable(QStringLiteral("%1 %2 %3").arg(before).arg(sharpened).arg(masked)));

    const double calmed = spread(runOne(QStringLiteral("detail"), {{QStringLiteral("luminanceNoise"), 100.0}}, noisy), all).second;
    QVERIFY2(calmed < before * 0.6, qPrintable(QStringLiteral("%1 → %2").arg(before).arg(calmed)));
    const QImage edgeCalm = runOne(QStringLiteral("detail"), {{QStringLiteral("luminanceNoise"), 100.0}}, edge);
    QVERIFY(std::abs(edgeCalm.pixelColor(63, 30).red() - 60) <= 3 && std::abs(edgeCalm.pixelColor(64, 30).red() - 180) <= 3);

    // Colour blotches on grey: red and blue apart by noise, brought together.
    QImage blotchy(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            const int n = noise[y * size.width() + x];
            blotchy.setPixelColor(x, y, QColor(128 + n, 128, 128 - n));
        }
    }
    auto colourSpread = [&](const QImage &image) {
        double sum = 0;
        for (int y = 4; y < 60; ++y) {
            for (int x = 4; x < 124; ++x) {
                const QColor c = image.pixelColor(x, y);
                sum += std::abs(c.red() - c.blue());
            }
        }
        return sum;
    };
    const QImage calmColour = runOne(QStringLiteral("detail"), {{QStringLiteral("colourNoise"), 100.0}}, blotchy);
    QVERIFY2(colourSpread(calmColour) < colourSpread(blotchy) * 0.5,
             qPrintable(QStringLiteral("%1 → %2").arg(colourSpread(blotchy)).arg(colourSpread(calmColour))));
}

// The vignette darkens or lightens the corners, not the middle; grain is noise around the same
// brightness, the same each time.
void TestGpu::finishEffect()
{
    if (!m_available)
        QSKIP("no Vulkan device");
    QString error;
    const QImage grey = greyPicture(QSize(300, 200), [](int, int) { return 128; });
    const QImage neutral = runOne(QStringLiteral("finish"), {}, grey, &error);
    QVERIFY2(!neutral.isNull(), qPrintable(error));
    QVERIFY(std::abs(neutral.pixelColor(2, 2).red() - 128) <= 1);
    const QImage dark = runOne(QStringLiteral("finish"), {{QStringLiteral("vignette"), -100.0}}, grey);
    QVERIFY2(dark.pixelColor(2, 2).red() < 70, qPrintable(dark.pixelColor(2, 2).name()));
    QVERIFY(std::abs(dark.pixelColor(150, 100).red() - 128) <= 1);
    QVERIFY(dark.pixelColor(2, 100).red() > dark.pixelColor(2, 2).red());
    const QImage light = runOne(QStringLiteral("finish"), {{QStringLiteral("vignette"), 100.0}}, grey);
    QVERIFY(light.pixelColor(2, 2).red() > 180);
    // Feather 0 is a sharp border, a later midpoint keeps more of the middle.
    const QImage hard = runOne(QStringLiteral("finish"), {{QStringLiteral("vignette"), -100.0}, {QStringLiteral("feather"), 0.0}}, grey);
    QVERIFY(hard.pixelColor(150, 60).red() >= 127);
    const QImage wide = runOne(QStringLiteral("finish"), {{QStringLiteral("vignette"), -100.0}, {QStringLiteral("midpoint"), 100.0}}, grey);
    QVERIFY(wide.pixelColor(30, 30).red() > dark.pixelColor(30, 30).red());

    const QImage big = greyPicture(QSize(1200, 800), [](int, int) { return 128; });
    const QVariantMap grain{{QStringLiteral("grain"), 100.0}};
    const QImage grainy = runOne(QStringLiteral("finish"), grain, big);
    const auto g = spread(grainy, QRect(100, 100, 1000, 600));
    QVERIFY2(g.second > 3 && std::abs(g.first - 128) < 2, qPrintable(QStringLiteral("%1 ± %2").arg(g.first).arg(g.second)));
    QCOMPARE(runOne(QStringLiteral("finish"), grain, big), grainy);
    const QImage smooth = runOne(QStringLiteral("finish"), {{QStringLiteral("grain"), 100.0}, {QStringLiteral("roughness"), 0.0}}, big);
    QVERIFY(smooth != grainy);
    QVERIFY(spread(smooth, QRect(100, 100, 1000, 600)).second > 3);
}

// The whole Develop tab at once on a 24-megapixel picture, for the time it takes.
void TestGpu::developAtFullSize()
{
    if (!m_available)
        QSKIP("no Vulkan device");
    QImage full(6000, 4000, QImage::Format_RGBA8888);
    full.fill(QColor(120, 110, 100));
    const QList<EffectStep> steps{{QStringLiteral("develop"), {{QStringLiteral("exposure"), 0.3}, {QStringLiteral("contrast"), 20.0}}},
                                  {QStringLiteral("detail"), {{QStringLiteral("sharpen"), 60.0}, {QStringLiteral("luminanceNoise"), 30.0}, {QStringLiteral("colourNoise"), 30.0}}},
                                  {QStringLiteral("local"), {{QStringLiteral("texture"), 20.0}, {QStringLiteral("clarity"), 30.0}, {QStringLiteral("dehaze"), 20.0}}},
                                  {QStringLiteral("mixer"), {{QStringLiteral("satRed"), 20.0}}},
                                  {QStringLiteral("grading"), {{QStringLiteral("shadows"), QVariantList{200.0, 30.0}}}},
                                  {QStringLiteral("finish"), {{QStringLiteral("vignette"), -30.0}, {QStringLiteral("grain"), 20.0}}}};
    Chain chain;
    QString error;
    QVERIFY2(chain.setSteps(steps, &error), qPrintable(error));
    QElapsedTimer timer;
    timer.start();
    const QImage out = chain.render(full);
    QVERIFY2(!out.isNull(), qPrintable(chain.error()));
    const qint64 first = timer.restart();
    QVERIFY(!chain.render(full).isNull());
    qInfo() << "6000 x 4000 through the Develop tab:" << first << "ms the first time," << timer.elapsed() << "ms again;"
            << chain.gpuMilliseconds() << "ms on the GPU";
    QVERIFY(timer.elapsed() < 3000);
}

// A scene for looking at: a hazy sky, hills, a textured field, a little noise.
static QImage scene(const QSize &size)
{
    QImage image(size, QImage::Format_RGBA8888);
    QRandomGenerator random(3);
    const double w = size.width(), h = size.height();
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            const double u = x / w, v = y / h;
            const double hill = 0.45 + 0.08 * std::sin(u * 7.0) + 0.05 * std::sin(u * 17.0 + 1.0);
            const double field = 0.7 + 0.04 * std::sin(u * 11.0);
            double r, g, b;
            if (v < hill) {
                r = 150 + 60 * v;
                g = 180 + 40 * v;
                b = 225 - 10 * v;
            } else if (v < field) {
                const double far = (field - v) / (field - hill);
                r = 70 + 90 * far;
                g = 100 + 70 * far;
                b = 80 + 110 * far;
            } else {
                const double grass = 0.5 + 0.5 * std::sin(x * 0.9 + 3 * std::sin(y * 0.31)) * std::sin(y * 0.7);
                r = 90 + 50 * grass;
                g = 120 + 60 * grass;
                b = 40 + 20 * grass;
            }
            const int n = random.bounded(-6, 7);
            image.setPixelColor(x, y, QColor(std::clamp(int(r) + n, 0, 255), std::clamp(int(g) + n, 0, 255), std::clamp(int(b) + n, 0, 255)));
        }
    }
    // A sun.
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            const double d = std::hypot(x - w * 0.75, y - h * 0.18) / (w * 0.05);
            if (d < 1.0)
                image.setPixelColor(x, y, QColor(255, 250, 230));
        }
    }
    return image;
}

// With LUMEN_SCREENSHOT_DIR: the scene through each step 5 effect, three by three in the order
// below (no captions: this test has no fonts), to look at.
void TestGpu::lookSheet()
{
    const QString dir = qEnvironmentVariable("LUMEN_SCREENSHOT_DIR");
    if (!m_available || dir.isEmpty())
        QSKIP("for looking at: needs a Vulkan device and LUMEN_SCREENSHOT_DIR");
    const QImage picture = scene(QSize(600, 400));
    const QList<QPair<QString, QPair<QString, QVariantMap>>> looks{
        {QStringLiteral("As it is"), {QStringLiteral("develop"), {}}},
        {QStringLiteral("Clarity +80"), {QStringLiteral("local"), {{QStringLiteral("clarity"), 80.0}}}},
        {QStringLiteral("Texture +100"), {QStringLiteral("local"), {{QStringLiteral("texture"), 100.0}}}},
        {QStringLiteral("Dehaze +70"), {QStringLiteral("local"), {{QStringLiteral("dehaze"), 70.0}}}},
        {QStringLiteral("Dehaze -50"), {QStringLiteral("local"), {{QStringLiteral("dehaze"), -50.0}}}},
        {QStringLiteral("Sharpen 120"), {QStringLiteral("detail"), {{QStringLiteral("sharpen"), 120.0}}}},
        {QStringLiteral("Noise 80"), {QStringLiteral("detail"), {{QStringLiteral("luminanceNoise"), 80.0}, {QStringLiteral("colourNoise"), 80.0}}}},
        {QStringLiteral("Vignette -60"), {QStringLiteral("finish"), {{QStringLiteral("vignette"), -60.0}}}},
        {QStringLiteral("Grain 60"), {QStringLiteral("finish"), {{QStringLiteral("grain"), 60.0}}}},
    };
    QImage sheet(picture.width() * 3, (picture.height() + 24) * 3, QImage::Format_RGB32);
    sheet.fill(Qt::black);
    QPainter painter(&sheet);
    for (int i = 0; i < looks.size(); ++i) {
        QString error;
        const QImage out = runOne(looks[i].second.first, looks[i].second.second, picture, &error);
        QVERIFY2(!out.isNull(), qPrintable(error));
        const QPoint at((i % 3) * picture.width(), (i / 3) * (picture.height() + 24));
        painter.drawImage(at + QPoint(0, 24), out);
    }
    painter.end();
    QVERIFY(sheet.save(QDir(dir).filePath(QStringLiteral("develop-step5.png"))));
}

// ---- Your own effects (plan M20 step 7) -------------------------------------------------------------

// An effect of your own: Lumen's modules imported, a slider, two passes with a small texture between.
static const char kOwnEffect[] = R"(import effect;
import colorspace;

struct Params
{
    [Label("Strength")] [Range(0, 100)] [Default(100)] float strength;
    uint2 size;
};
[[vk::push_constant]] ConstantBuffer<Params> p;

[[vk::binding(0)]] [format("rgba16f")] RWTexture2D<float4> source;
[[vk::binding(1)]] [format("rgba16f")] RWTexture2D<float4> result;
[Size(1, 1)] [[vk::binding(2)]] [format("rgba16f")] RWTexture2D<float4> one;

[Effect("Turn over", "Mine")]
[Over("one")]
[shader("compute")]
[numthreads(1, 1, 1)]
void prepare(uint3 id: SV_DispatchThreadID)
{
    one[uint2(0, 0)] = float4(1.0, 1.0, 1.0, 1.0);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= p.size))
        return;
    const float4 c = source[id.xy];
    const float3 over = one[uint2(0, 0)].rgb - toSrgb(c.rgb);
    result[id.xy] = float4(lerp(c.rgb, toLinear(over), p.strength / 100.0), c.a);
}
)";

void TestGpu::compilerMessages()
{
    const QString output = QStringLiteral(
        "E30015\terror\tC:/work/mine.slang\t6\t142\t6\t156\tundefined identifier\r\n"
        "E30015\tspan\tC:/work/mine.slang\t6\t142\t6\t156\tundefined identifier 'undefinedThing'.\r\n"
        "W41012\twarning\tC:/lumen/modules/effect.slang\t3\t1\t3\t4\tsomething in a module\n"
        "not a message\n");
    const QList<CompileMessage> messages = Compiler::parseMessages(output, QStringLiteral("C:\\work\\mine.slang"));
    QCOMPARE(messages.size(), 2);
    QCOMPARE(messages[0].severity, QStringLiteral("error"));
    QCOMPARE(messages[0].code, QStringLiteral("E30015"));
    QCOMPARE(messages[0].text, QStringLiteral("undefined identifier 'undefinedThing'."));
    QCOMPARE(messages[0].line, 6);
    QCOMPARE(messages[0].column, 142);
    QCOMPARE(messages[0].endColumn, 156);
    QVERIFY(messages[0].inSource);
    QCOMPARE(messages[1].severity, QStringLiteral("warning"));
    QVERIFY(!messages[1].inSource);
}

// Compiled, added and run like a built-in effect; compiled again, the new one runs.
void TestGpu::compileOwnEffect()
{
    if (QStringLiteral(LUMEN_TEST_SLANGC).isEmpty())
        QSKIP("no slangc");
    Compiler::setProgram(QStringLiteral(LUMEN_TEST_SLANGC));
    QVERIFY(Compiler::isAvailable());
    const Compiled compiled = Compiler::compile(QString::fromUtf8(kOwnEffect), QStringLiteral("Turn over"));
    QVERIFY2(compiled.ok(), qPrintable(compiled.error));
    QString error;
    const Effect effect = Effect::addCompiled(QStringLiteral("user:turn-over"), compiled.spirv, compiled.reflection, &error);
    QVERIFY2(!effect.isNull(), qPrintable(error));
    QCOMPARE(effect.title(), QStringLiteral("Turn over"));
    QCOMPARE(effect.defaults().value(QStringLiteral("strength")).toDouble(), 100.0);
    QCOMPARE(effect.textureSize(QStringLiteral("one"), QSize(640, 480)), QSize(1, 1));
    QVERIFY(Effect::compiled().contains(QStringLiteral("user:turn-over")));
    QCOMPARE(Effect::describe(QStringLiteral("user:turn-over")).title(), QStringLiteral("Turn over"));

    if (!m_available)
        QSKIP("no Vulkan device");
    const QImage picture = swatches({QColor(100, 100, 100), QColor(200, 120, 80)});
    QImage out = runOne(QStringLiteral("user:turn-over"), {}, picture, &error);
    QVERIFY2(!out.isNull(), qPrintable(error));
    QVERIFY2(near(out.pixelColor(1, 0), QColor(55, 135, 175)), qPrintable(out.pixelColor(1, 0).name()));
    QVERIFY(near(runOne(QStringLiteral("user:turn-over"), {{QStringLiteral("strength"), 0.0}}, picture).pixelColor(1, 0), QColor(200, 120, 80)));

    // Changed and added again under the same id: the new one runs.
    QString changed = QString::fromUtf8(kOwnEffect);
    changed.replace(QStringLiteral("float4(1.0, 1.0, 1.0, 1.0)"), QStringLiteral("float4(0.5, 0.5, 0.5, 1.0)"));
    const Compiled again = Compiler::compile(changed, QStringLiteral("Turn over"));
    QVERIFY2(again.ok(), qPrintable(again.error));
    QVERIFY(!Effect::addCompiled(QStringLiteral("user:turn-over"), again.spirv, again.reflection, &error).isNull());
    out = runOne(QStringLiteral("user:turn-over"), {}, picture, &error);
    QVERIFY2(out.pixelColor(0, 0).red() <= 30, qPrintable(out.pixelColor(0, 0).name()));
    Effect::removeCompiled(QStringLiteral("user:turn-over"));
    QVERIFY(Effect::describe(QStringLiteral("user:turn-over")).isNull());
}

// What goes wrong is said where it is: the compiler's errors by line, and an effect Lumen cannot run.
void TestGpu::compileMistakes()
{
    if (QStringLiteral(LUMEN_TEST_SLANGC).isEmpty())
        QSKIP("no slangc");
    Compiler::setProgram(QStringLiteral(LUMEN_TEST_SLANGC));
    QString broken = QString::fromUtf8(kOwnEffect);
    broken.replace(QStringLiteral("p.strength / 100.0"), QStringLiteral("p.strenght / 100.0"));
    const Compiled bad = Compiler::compile(broken);
    QVERIFY(!bad.ok());
    const int line = int(broken.left(broken.indexOf(QStringLiteral("p.strenght"))).count(u'\n')) + 1;
    bool found = false;
    for (const CompileMessage &m : bad.messages)
        found = found || (m.severity == QLatin1String("error") && m.inSource && m.line == line && m.column > 1);
    QVERIFY2(found, qPrintable(bad.error));
    QVERIFY2(bad.error.startsWith(QStringLiteral("Line %1:").arg(line)), qPrintable(bad.error));

    // Compiles, but writes no "result".
    QString nothing = QString::fromUtf8(kOwnEffect);
    nothing.replace(QStringLiteral("RWTexture2D<float4> result;"), QStringLiteral("RWTexture2D<float4> output;"));
    nothing.replace(QStringLiteral("result[id.xy]"), QStringLiteral("output[id.xy]"));
    const Compiled quiet = Compiler::compile(nothing);
    QVERIFY2(quiet.ok(), qPrintable(quiet.error));
    QString error;
    QVERIFY(Effect::addCompiled(QStringLiteral("user:quiet"), quiet.spirv, quiet.reflection, &error).isNull());
    QVERIFY2(error.contains(QLatin1String("result")), qPrintable(error));
    QVERIFY(!Effect::compiled().contains(QStringLiteral("user:quiet")));

    // No compiler: it says so.
    Compiler::setProgram(QStringLiteral("C:/nowhere/slangc.exe"));
    if (qEnvironmentVariableIsEmpty("LUMEN_SLANGC")) {
        QVERIFY(!Compiler::isAvailable());
        QVERIFY(!Compiler::compile(QString::fromUtf8(kOwnEffect)).ok());
    }
    Compiler::setProgram(QStringLiteral(LUMEN_TEST_SLANGC));
}

// Highlights and Shadows are local (plan M20 step 9): a textured area moves as a whole, so its
// texture keeps its contrast (a global curve lifts its lighter stripes more); next to an edge the
// other side does not glow; the preview at screen size agrees with the full size made small.
void TestGpu::localShadowsAndHighlights()
{
    if (!m_available)
        QSKIP("no Vulkan device");
    // Left: dark stripes (25 and 45); right: light stripes (200 and 225); the edge at 128.
    const QSize size(256, 192);
    const QImage picture = greyPicture(size, [](int x, int) {
        const bool other = (x / 2) % 2;
        return x < 128 ? (other ? 45.0 : 25.0) : (other ? 225.0 : 200.0);
    });
    auto at = [](const QImage &image, int x) { return image.pixelColor(x, 96).red(); };

    const QImage lifted = runOne(QStringLiteral("develop"), {{QStringLiteral("shadows"), 100.0}}, picture);
    QVERIFY(!lifted.isNull());
    // Inside the dark area: both stripes lifted by about as much.
    const int darkLift = at(lifted, 40) - at(picture, 40);
    const int otherLift = at(lifted, 42) - at(picture, 42);
    QVERIFY2(darkLift > 8 && otherLift > 8, qPrintable(QStringLiteral("%1 %2").arg(darkLift).arg(otherLift)));
    QVERIFY2(std::abs(darkLift - otherLift) <= 5, qPrintable(QStringLiteral("lifts %1 and %2").arg(darkLift).arg(otherLift)));
    // The light side next to the edge does not glow.
    for (int x = 128; x < 140; ++x)
        QVERIFY2(std::abs(at(lifted, x) - at(picture, x)) <= 2, qPrintable(QStringLiteral("%1: %2").arg(x).arg(at(lifted, x))));

    const QImage lowered = runOne(QStringLiteral("develop"), {{QStringLiteral("highlights"), -100.0}}, picture);
    const int lightDrop = at(picture, 200) - at(lowered, 200);
    const int otherDrop = at(picture, 202) - at(lowered, 202);
    QVERIFY2(lightDrop > 8 && otherDrop > 8, qPrintable(QStringLiteral("%1 %2").arg(lightDrop).arg(otherDrop)));
    QVERIFY2(std::abs(lightDrop - otherDrop) <= 5, qPrintable(QStringLiteral("drops %1 and %2").arg(lightDrop).arg(otherDrop)));
    for (int x = 116; x < 128; ++x)
        QVERIFY2(std::abs(at(lowered, x) - at(picture, x)) <= 2, qPrintable(QStringLiteral("%1: %2").arg(x).arg(at(lowered, x))));

    // A large picture and its preview at a quarter agree once the large one is made small.
    const QImage big = greyPicture(QSize(1024, 768), [](int x, int y) {
        return 60 + 40 * std::sin(x / 90.0) * std::cos(y / 70.0) + ((x / 3 + y / 3) % 2 ? 8 : 0);
    });
    const QVariantMap both{{QStringLiteral("shadows"), 80.0}, {QStringLiteral("highlights"), -80.0}};
    const QImage small = big.scaled(256, 192, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const QImage bigOut = runOne(QStringLiteral("develop"), both, big).scaled(256, 192, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const QImage smallOut = runOne(QStringLiteral("develop"), both, small);
    double apart = 0;
    for (int y = 0; y < 192; ++y) {
        for (int x = 0; x < 256; ++x)
            apart += std::abs(bigOut.pixelColor(x, y).red() - smallOut.pixelColor(x, y).red());
    }
    apart /= 256.0 * 192.0;
    QVERIFY2(apart < 2.0, qPrintable(QStringLiteral("%1 levels apart").arg(apart)));
}

QTEST_GUILESS_MAIN(TestGpu)
#include "tst_gpu.moc"
