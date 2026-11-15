// lumen_gpu (plan M20 step 1): the device, textures, kernels from Slang's reflection, uploads,
// dispatches and read-backs, with barriers from the textures' states. Runs under Vulkan's validation
// layer where the Vulkan SDK provides it, and checks it said nothing; skips without a usable device.
#include "gpu/Gpu.h"

#include <QElapsedTimer>
#include <QFloat16>
#include <QRandomGenerator>
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

QTEST_GUILESS_MAIN(TestGpu)
#include "tst_gpu.moc"
