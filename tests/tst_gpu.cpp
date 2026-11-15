// lumen_gpu (plan M20 step 1): the device, textures, kernels from Slang's reflection, uploads,
// dispatches and read-backs, with barriers from the textures' states. Runs under Vulkan's validation
// layer where the Vulkan SDK provides it, and checks it said nothing; skips without a usable device.
#include "gpu/Effects.h"
#include "gpu/Gpu.h"

#include <QElapsedTimer>
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

QTEST_GUILESS_MAIN(TestGpu)
#include "tst_gpu.moc"
