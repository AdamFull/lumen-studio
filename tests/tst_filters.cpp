#include "core/Async.h"
#include "core/Error.h"
#include "core/ImageOps.h"
#include "filters/Evaluator.h"
#include "filters/Graph.h"
#include "filters/Library.h"
#include "filters/Ops.h"
#include "filters/Registry.h"

#include <QFile>
#include <QJsonDocument>
#include <QPromise>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <atomic>
#include <cmath>
#include <functional>

using namespace lumen;
using namespace lumen::filters;

namespace {

// ---- Synthetic images -------------------------------------------------------------------------

QImage make(int w, int h, const std::function<QRgb(int, int)> &pixel)
{
    QImage image(w, h, QImage::Format_ARGB32);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            image.setPixel(x, y, pixel(x, y));
    return img::normalized(image);
}

QImage solid(int w, int h, QRgb color)
{
    return make(w, h, [color](int, int) { return color; });
}

// Every pixel different: red from x, green from y.
QImage distinct(int w, int h)
{
    return make(w, h, [](int x, int y) { return qRgb(10 + x * 20, 10 + y * 20, 100); });
}

QImage gradient(int w, int h)
{
    return make(w, h, [w, h](int x, int y) {
        return qRgb(x * 255 / std::max(1, w - 1), y * 255 / std::max(1, h - 1), 255 - (x + y) * 255 / std::max(1, w + h - 2));
    });
}

QImage maskOf(int w, int h, const std::function<int(int, int)> &value)
{
    QImage m = img::createMask({w, h}, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            m.scanLine(y)[x] = static_cast<uchar>(value(x, y));
    return m;
}

int maskAt(const QImage &m, int x, int y)
{
    return m.constScanLine(y)[x];
}

double mean(const QImage &image, int (*channel)(QRgb))
{
    double sum = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            sum += channel(image.pixel(x, y));
    return sum / (double(image.width()) * image.height());
}

// Largest difference of any channel (unpremultiplied) between two images of the same size.
int maxDifference(const QImage &a, const QImage &b)
{
    if (a.size() != b.size())
        return 256;
    int worst = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
            worst = std::max({worst, std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q)),
                              std::abs(qBlue(p) - qBlue(q)), std::abs(qAlpha(p) - qAlpha(q))});
        }
    }
    return worst;
}

// ---- Running nodes ----------------------------------------------------------------------------

NodeTypePtr type(const QString &id)
{
    NodeTypePtr t = NodeRegistry::global().find(id);
    if (!t)
        qFatal("no node type %s", qPrintable(id));
    return t;
}

QImage apply(const QString &id, const QImage &image, const QVariantMap &params = {})
{
    return evaluateNode(*type(id), {{QStringLiteral("image"), image}}, params).value(QStringLiteral("image")).value<QImage>();
}

QImage applyToMask(const QString &id, const QImage &mask, const QVariantMap &params = {})
{
    return evaluateNode(*type(id), {{QStringLiteral("mask"), mask}}, params).value(QStringLiteral("mask")).value<QImage>();
}

struct Outcome
{
    EvalResult result;
    QString error;
    bool cancelled = false;
};

Outcome finish(QFuture<EvalResult> future)
{
    Outcome o;
    try {
        future.waitForFinished();
        if (future.isCanceled() && future.resultCount() == 0)
            o.cancelled = true;
        else
            o.result = future.result();
    } catch (const std::exception &e) {
        o.error = describeException(e);
    }
    return o;
}

Outcome run(Evaluator &evaluator, const Graph &graph, const EvalInputs &inputs, const EvalOptions &options = {})
{
    return finish(evaluator.evaluate(graph, inputs, options));
}

// input -> nodes... -> output, each node's "image" into the next one's "image".
Graph chain(const QStringList &types, const NodeRegistry &registry = NodeRegistry::global())
{
    Graph g;
    QString previous = g.addNode(QStringLiteral("input"), {}, registry);
    for (const QString &t : types) {
        const QString id = g.addNode(t, {}, registry);
        if (id.isEmpty())
            qFatal("unknown node type %s", qPrintable(t));
        g.connect(previous, QStringLiteral("image"), id, QStringLiteral("image"));
        previous = id;
    }
    const QString out = g.addNode(QStringLiteral("output"), {}, registry);
    g.connect(previous, QStringLiteral("image"), out, QStringLiteral("image"));
    return g;
}

// ---- Test node types --------------------------------------------------------------------------

struct Counter
{
    QMutex mutex;
    QHash<QString, int> counts;

    void add(const QString &node)
    {
        QMutexLocker lock(&mutex);
        ++counts[node];
    }
    int value(const QString &node)
    {
        QMutexLocker lock(&mutex);
        return counts.value(node);
    }
    void clear()
    {
        QMutexLocker lock(&mutex);
        counts.clear();
    }
};

Counter &counter()
{
    static Counter c;
    return c;
}

NodeTypeInfo passThroughInfo(const QString &id)
{
    NodeTypeInfo info;
    info.id = id;
    info.name = id;
    info.category = QStringLiteral("test");
    info.inputs = {{QStringLiteral("image"), QStringLiteral("Image"), PortType::Image, false}};
    info.outputs = {{QStringLiteral("image"), QStringLiteral("Image"), PortType::Image, false}};
    return info;
}

// Passes its image through and counts its evaluations per node.
NodeTypePtr countType()
{
    return makeNodeType(passThroughInfo(QStringLiteral("test-count")), [](const NodeArgs &a, EvalContext &ctx) -> NodeResult {
        counter().add(ctx.nodeId());
        return Values{{QStringLiteral("image"), a.image(QStringLiteral("image"))}};
    });
}

// A colour parameter, a text parameter and a number input that can drive the strength.
NodeTypePtr tintType()
{
    NodeTypeInfo info = passThroughInfo(QStringLiteral("test-tint"));
    info.inputs.append({QStringLiteral("strength"), QStringLiteral("Strength"), PortType::Number, true});
    ParamSpec color;
    color.id = QStringLiteral("color");
    color.label = QStringLiteral("Colour");
    color.type = PortType::Color;
    color.defaultValue = QColor(Qt::red);
    ParamSpec strength;
    strength.id = QStringLiteral("strength");
    strength.label = QStringLiteral("Strength");
    strength.defaultValue = 0.5;
    strength.decimals = 2;
    ParamSpec note;
    note.id = QStringLiteral("note");
    note.label = QStringLiteral("Note");
    note.type = PortType::Text;
    note.defaultValue = QString();
    info.params = {color, strength, note};
    return makeNodeType(info, [](const NodeArgs &a, EvalContext &) -> NodeResult {
        QImage out = a.image(QStringLiteral("image"));
        QPainter p(&out);
        p.setOpacity(a.number(QStringLiteral("strength")));
        p.fillRect(out.rect(), a.color(QStringLiteral("color")));
        p.end();
        return Values{{QStringLiteral("image"), out}};
    });
}

struct Slow
{
    QSemaphore started;
    std::atomic_bool sawCancel = false;
};

Slow &slow()
{
    static Slow s;
    return s;
}

// Runs until the evaluation is cancelled (at most 10 s).
NodeTypePtr slowType()
{
    return makeNodeType(passThroughInfo(QStringLiteral("test-slow")), [](const NodeArgs &a, EvalContext &ctx) -> NodeResult {
        slow().started.release();
        QElapsedTimer timer;
        timer.start();
        while (!ctx.isCancelled() && timer.elapsed() < 10000)
            QThread::msleep(1);
        slow().sawCancel = ctx.isCancelled();
        return Values{{QStringLiteral("image"), a.image(QStringLiteral("image"))}};
    });
}

struct Later
{
    QMutex mutex;
    std::shared_ptr<QPromise<Values>> promise;
    QImage input;
    QSemaphore started;
    std::atomic_bool cancelHandlerRan = false;

    std::shared_ptr<QPromise<Values>> take()
    {
        QMutexLocker lock(&mutex);
        return std::exchange(promise, nullptr);
    }
};

Later &later()
{
    static Later l;
    return l;
}

// Returns a future that the test resolves later, as an AI node returns its job's future.
NodeTypePtr laterType()
{
    return makeNodeType(passThroughInfo(QStringLiteral("test-later")), [](const NodeArgs &a, EvalContext &ctx) -> NodeResult {
        auto promise = std::make_shared<QPromise<Values>>();
        promise->start();
        {
            QMutexLocker lock(&later().mutex);
            later().promise = promise;
            later().input = a.image(QStringLiteral("image"));
        }
        ctx.onCancel([] { later().cancelHandlerRan = true; });
        later().started.release();
        return promise->future();
    });
}

NodeTypePtr expensiveType()
{
    NodeTypeInfo info = passThroughInfo(QStringLiteral("test-expensive"));
    info.expensive = true;
    return makeNodeType(info, [](const NodeArgs &a, EvalContext &ctx) -> NodeResult {
        counter().add(ctx.nodeId());
        return Values{{QStringLiteral("image"), ops::invert(a.image(QStringLiteral("image")))}};
    });
}

NodeRegistry testRegistry()
{
    NodeRegistry r = NodeRegistry::global();
    r.add(countType());
    r.add(tintType());
    r.add(slowType());
    r.add(laterType());
    r.add(expensiveType());
    return r;
}

QString dataPath(const QString &name)
{
    return QStringLiteral(LUMEN_SOURCE_DIR "/tests/data/filters/") + name;
}

// The source of the golden-image test: a colour gradient with a white square and a black dot.
QImage goldenSource()
{
    return make(48, 32, [](int x, int y) {
        if (x >= 30 && x < 40 && y >= 4 && y < 12)
            return qRgb(255, 255, 255);
        if ((x - 10) * (x - 10) + (y - 22) * (y - 22) <= 9)
            return qRgb(0, 0, 0);
        return qRgb(x * 255 / 47, y * 255 / 31, 255 - (x + y) * 255 / 78);
    });
}

QImage goldenSelection()
{
    return maskOf(48, 32, [](int x, int y) {
        const double dx = (x + 0.5 - 24) / 16.0, dy = (y + 0.5 - 16) / 10.0;
        return dx * dx + dy * dy <= 1.0 ? 255 : 0;
    });
}

} // namespace

class TestFilters : public QObject
{
    Q_OBJECT
private slots:
    void registryDescribesEveryNode();
    void invertNode();
    void desaturateNode();
    void thresholdNode();
    void posterizeNode();
    void levelsNode();
    void curvesNode();
    void brightnessContrastNode();
    void hueSaturationNode();
    void colorBalanceNode();
    void blurNode();
    void sharpenNodes();
    void noiseNode();
    void medianNode();
    void blendNode();
    void mixNode();
    void maskNodes();
    void resizeNode();
    void cropNode();
    void flipNode();
    void rotateNode();
    void inputAndOutput();
    void validation();
    void connectRefusesBadLinks();
    void jsonRoundTrip();
    void fileErrors();
    void library();
    void cacheRunsOnlyWhatChanged();
    void previewScale();
    void cancellation();
    void asynchronousNode();
    void expensiveNodeWaitsForRun();
    void goldenImage();
};

void TestFilters::registryDescribesEveryNode()
{
    const QStringList classic{QStringLiteral("input"), QStringLiteral("output"), QStringLiteral("levels"),
                              QStringLiteral("curves"), QStringLiteral("brightness-contrast"), QStringLiteral("hue-saturation"),
                              QStringLiteral("color-balance"), QStringLiteral("invert"), QStringLiteral("desaturate"),
                              QStringLiteral("threshold"), QStringLiteral("posterize"), QStringLiteral("gaussian-blur"),
                              QStringLiteral("sharpen"), QStringLiteral("unsharp-mask"), QStringLiteral("noise"),
                              QStringLiteral("median"), QStringLiteral("blend"), QStringLiteral("mix"),
                              QStringLiteral("mask-from-image"), QStringLiteral("grow-shrink-feather"),
                              QStringLiteral("invert-mask"), QStringLiteral("resize"), QStringLiteral("crop"),
                              QStringLiteral("flip"), QStringLiteral("rotate")};
    const QList<NodeTypePtr> types = NodeRegistry::global().types();
    QCOMPARE(types.size(), classic.size());
    for (const QString &id : classic) {
        const NodeTypePtr t = NodeRegistry::global().find(id);
        QVERIFY2(t, qPrintable(id));
        const NodeTypeInfo &info = t->info();
        QVERIFY(!info.name.isEmpty());
        QVERIFY(!info.description.isEmpty());
        QVERIFY2(categoryIds().contains(info.category), qPrintable(id));
        auto unique = [](QStringList ids) {
            const qsizetype n = ids.size();
            ids.removeDuplicates();
            return ids.size() == n;
        };
        QStringList inputs, outputs, params;
        for (const PortSpec &p : info.inputs)
            inputs << p.id;
        for (const PortSpec &p : info.outputs)
            outputs << p.id;
        for (const ParamSpec &p : info.params)
            params << p.id;
        QVERIFY2(unique(inputs) && unique(outputs) && unique(params), qPrintable(id));
        for (const ParamSpec &p : info.params) {
            QVERIFY(!p.label.isEmpty());
            // Every default is a value the parameter can have.
            QCOMPARE(coerceParam(p, QVariant()), coerceParam(p, p.defaultValue));
            if (p.type == PortType::Number) {
                QVERIFY2(p.minimum <= p.defaultValue.toDouble() && p.defaultValue.toDouble() <= p.maximum,
                         qPrintable(id + QLatin1Char('.') + p.id));
            } else if (p.type == PortType::Choice) {
                QVERIFY(!p.choices.isEmpty());
                QCOMPARE(coerceParam(p, p.defaultValue).toString(), p.defaultValue.toString());
            }
        }
    }
    QCOMPARE(categoryName(categories::Color), QStringLiteral("Colour"));
    QCOMPARE(NodeRegistry::global().typesInCategory(categories::InputOutput).size(), 2);
    // Values are coerced to their spec: clamped, rounded, an unknown choice replaced.
    const ParamSpec *level = type(QStringLiteral("threshold"))->info().param(QStringLiteral("level"));
    QCOMPARE(coerceParam(*level, 300).toDouble(), 255.0);
    QCOMPARE(coerceParam(*level, 12.6).toDouble(), 13.0);
    const ParamSpec *mode = type(QStringLiteral("blend"))->info().param(QStringLiteral("mode"));
    QCOMPARE(coerceParam(*mode, QStringLiteral("nonsense")).toString(), QStringLiteral("normal"));
}

void TestFilters::invertNode()
{
    QImage src(3, 1, QImage::Format_ARGB32);
    src.setPixel(0, 0, qRgb(10, 20, 30));
    src.setPixel(1, 0, qRgb(255, 0, 128));
    src.setPixel(2, 0, qRgba(255, 255, 255, 0));
    const QImage out = apply(QStringLiteral("invert"), src);
    QCOMPARE(out.format(), QImage::Format_ARGB32_Premultiplied);
    QCOMPARE(out.pixel(0, 0), qRgb(245, 235, 225));
    QCOMPARE(out.pixel(1, 0), qRgb(0, 255, 127));
    QCOMPARE(qAlpha(out.pixel(2, 0)), 0); // transparent stays transparent
    QCOMPARE(maxDifference(apply(QStringLiteral("invert"), apply(QStringLiteral("invert"), gradient(16, 8))), gradient(16, 8)), 0);
}

void TestFilters::desaturateNode()
{
    const QImage red = solid(2, 2, qRgb(255, 0, 0));
    auto grey = [&](const QString &method) {
        return apply(QStringLiteral("desaturate"), red, {{QStringLiteral("method"), method}}).pixel(0, 0);
    };
    QCOMPARE(grey(QStringLiteral("luminosity")), qRgb(76, 76, 76)); // 0.299 * 255
    QCOMPARE(grey(QStringLiteral("average")), qRgb(85, 85, 85));
    QCOMPARE(grey(QStringLiteral("lightness")), qRgb(128, 128, 128));
    const QImage out = apply(QStringLiteral("desaturate"), gradient(16, 8));
    for (int y = 0; y < out.height(); ++y) {
        for (int x = 0; x < out.width(); ++x) {
            const QRgb p = out.pixel(x, y);
            QVERIFY(qRed(p) == qGreen(p) && qGreen(p) == qBlue(p));
        }
    }
}

void TestFilters::thresholdNode()
{
    QImage src(4, 1, QImage::Format_RGB32);
    src.setPixel(0, 0, qRgb(127, 127, 127));
    src.setPixel(1, 0, qRgb(128, 128, 128));
    src.setPixel(2, 0, qRgb(255, 0, 0)); // luminosity 76
    src.setPixel(3, 0, qRgb(0, 255, 0)); // luminosity 150
    const QImage out = apply(QStringLiteral("threshold"), src, {{QStringLiteral("level"), 128}});
    QCOMPARE(out.pixel(0, 0), qRgb(0, 0, 0));
    QCOMPARE(out.pixel(1, 0), qRgb(255, 255, 255));
    QCOMPARE(out.pixel(2, 0), qRgb(0, 0, 0));
    QCOMPARE(out.pixel(3, 0), qRgb(255, 255, 255));
    QCOMPARE(apply(QStringLiteral("threshold"), src, {{QStringLiteral("level"), 0}}).pixel(2, 0), qRgb(255, 255, 255));
}

void TestFilters::posterizeNode()
{
    const QList<int> values{0, 42, 43, 127, 128, 200, 255};
    QImage src(values.size(), 1, QImage::Format_RGB32);
    for (int i = 0; i < values.size(); ++i)
        src.setPixel(i, 0, qRgb(values[i], values[i], values[i]));
    const QImage four = apply(QStringLiteral("posterize"), src, {{QStringLiteral("levels"), 4}});
    const QList<int> expected4{0, 0, 85, 85, 170, 170, 255};
    for (int i = 0; i < values.size(); ++i)
        QCOMPARE(qRed(four.pixel(i, 0)), expected4[i]);
    const QImage two = apply(QStringLiteral("posterize"), src, {{QStringLiteral("levels"), 2}});
    const QList<int> expected2{0, 0, 0, 0, 255, 255, 255};
    for (int i = 0; i < values.size(); ++i)
        QCOMPARE(qBlue(two.pixel(i, 0)), expected2[i]);
}

void TestFilters::levelsNode()
{
    const QList<int> values{0, 20, 50, 51, 64, 125, 200, 230, 255};
    QImage src(values.size(), 1, QImage::Format_RGB32);
    for (int i = 0; i < values.size(); ++i)
        src.setPixel(i, 0, qRgb(values[i], values[i], values[i]));
    auto levels = [&](const QVariantMap &params, int index) { return qGreen(apply(QStringLiteral("levels"), src, params).pixel(index, 0)); };

    const QVariantMap stretch{{QStringLiteral("inBlack"), 50}, {QStringLiteral("inWhite"), 200}};
    QCOMPARE(levels(stretch, 1), 0);   // 20
    QCOMPARE(levels(stretch, 2), 0);   // 50
    QCOMPARE(levels(stretch, 5), 128); // 125: half way, 127.5 rounds up
    QCOMPARE(levels(stretch, 6), 255); // 200
    QCOMPARE(levels(stretch, 7), 255); // 230
    const QVariantMap gamma{{QStringLiteral("gamma"), 2.0}};
    QCOMPARE(levels(gamma, 4), 128); // sqrt(64 / 255) * 255 = 127.75
    QCOMPARE(levels(gamma, 0), 0);
    QCOMPARE(levels(gamma, 8), 255);
    const QVariantMap output{{QStringLiteral("outBlack"), 100}, {QStringLiteral("outWhite"), 200}};
    QCOMPARE(levels(output, 0), 100);
    QCOMPARE(levels(output, 3), 120); // 51 / 255 = 0.2
    QCOMPARE(levels(output, 8), 200);
    // One channel only.
    const QImage red = apply(QStringLiteral("levels"), src, {{QStringLiteral("inWhite"), 100}, {QStringLiteral("channel"), QStringLiteral("red")}});
    QCOMPARE(red.pixel(2, 0), qRgb(128, 50, 50)); // 50 / 100 * 255 = 127.5
    QCOMPARE(apply(QStringLiteral("levels"), src), img::normalized(src));
}

void TestFilters::curvesNode()
{
    const QImage src = gradient(32, 8);
    QCOMPARE(maxDifference(apply(QStringLiteral("curves"), src), src), 0);
    QCOMPARE(maxDifference(apply(QStringLiteral("curves"), src, {{QStringLiteral("points"), QStringLiteral("0 255, 255 0")}}),
                           apply(QStringLiteral("invert"), src)),
             0);
    QCOMPARE(maxDifference(apply(QStringLiteral("curves"), src, {{QStringLiteral("points"), QStringLiteral("0 0, 128 128, 255 255")}}), src), 0);

    const QList<QPointF> lifted = ops::parseCurve(QStringLiteral("255 255, 0 0, 128 200")); // any order
    QCOMPARE(lifted.size(), 3);
    const QList<int> table = ops::curveTable(lifted);
    QCOMPARE(table[0], 0);
    QCOMPARE(table[128], 200);
    QCOMPARE(table[255], 255);
    for (int i = 1; i < 256; ++i)
        QVERIFY2(table[i] >= table[i - 1], "a rising curve stays rising (no overshoot)");
    // A peak does not overshoot either.
    const QList<int> peak = ops::curveTable(ops::parseCurve(QStringLiteral("0 0, 128 255, 255 0")));
    QCOMPARE(*std::max_element(peak.begin(), peak.end()), 255);
    QCOMPARE(peak[128], 255);
    // Flat outside the points; bad text gives the identity line.
    const QList<int> inner = ops::curveTable(ops::parseCurve(QStringLiteral("64 32, 192 224")));
    QCOMPARE(inner[0], 32);
    QCOMPARE(inner[255], 224);
    QCOMPARE(ops::parseCurve(QStringLiteral("nonsense")), (QList<QPointF>{{0, 0}, {255, 255}}));
    QCOMPARE(ops::curveText(lifted), QStringLiteral("0 0, 128 200, 255 255"));
    // One channel only.
    const QImage green = apply(QStringLiteral("curves"), solid(1, 1, qRgb(100, 100, 100)),
                               {{QStringLiteral("points"), QStringLiteral("0 255, 255 0")}, {QStringLiteral("channel"), QStringLiteral("green")}});
    QCOMPARE(green.pixel(0, 0), qRgb(100, 155, 100));
}

void TestFilters::brightnessContrastNode()
{
    const QImage src = gradient(32, 8);
    QCOMPARE(maxDifference(apply(QStringLiteral("brightness-contrast"), src), src), 0);
    QCOMPARE(apply(QStringLiteral("brightness-contrast"), src, {{QStringLiteral("brightness"), 100}}).pixel(3, 3), qRgb(255, 255, 255));
    QCOMPARE(apply(QStringLiteral("brightness-contrast"), src, {{QStringLiteral("brightness"), -100}}).pixel(3, 3), qRgb(0, 0, 0));
    QCOMPARE(apply(QStringLiteral("brightness-contrast"), src, {{QStringLiteral("contrast"), -100}}).pixel(7, 2), qRgb(128, 128, 128));
    QImage greys(2, 1, QImage::Format_RGB32);
    greys.setPixel(0, 0, qRgb(64, 64, 64));
    greys.setPixel(1, 0, qRgb(192, 192, 192));
    const QImage stronger = apply(QStringLiteral("brightness-contrast"), greys, {{QStringLiteral("contrast"), 50}});
    QVERIFY(qRed(stronger.pixel(0, 0)) < 64);
    QVERIFY(qRed(stronger.pixel(1, 0)) > 192);
    const QImage brighter = apply(QStringLiteral("brightness-contrast"), greys, {{QStringLiteral("brightness"), 50}});
    QCOMPARE(qRed(brighter.pixel(0, 0)), 160); // half way to white: 64 + 191 / 2 = 159.5
}

void TestFilters::hueSaturationNode()
{
    QImage src(3, 1, QImage::Format_RGB32);
    src.setPixel(0, 0, qRgb(255, 0, 0));
    src.setPixel(1, 0, qRgb(0, 255, 0));
    src.setPixel(2, 0, qRgb(0, 0, 255));
    const QImage turned = apply(QStringLiteral("hue-saturation"), src, {{QStringLiteral("hue"), 180}});
    QCOMPARE(turned.pixel(0, 0), qRgb(0, 255, 255)); // red -> cyan
    QCOMPARE(turned.pixel(1, 0), qRgb(255, 0, 255)); // green -> magenta
    QCOMPARE(turned.pixel(2, 0), qRgb(255, 255, 0)); // blue -> yellow
    QCOMPARE(apply(QStringLiteral("hue-saturation"), src, {{QStringLiteral("hue"), 120}}).pixel(0, 0), qRgb(0, 255, 0));
    const QImage grey = apply(QStringLiteral("hue-saturation"), gradient(16, 8), {{QStringLiteral("saturation"), -100}});
    for (int x = 0; x < grey.width(); ++x) {
        const QRgb p = grey.pixel(x, 4);
        QVERIFY(qRed(p) == qGreen(p) && qGreen(p) == qBlue(p));
    }
    QCOMPARE(apply(QStringLiteral("hue-saturation"), src, {{QStringLiteral("lightness"), 100}}).pixel(1, 0), qRgb(255, 255, 255));
    QCOMPARE(apply(QStringLiteral("hue-saturation"), src, {{QStringLiteral("lightness"), -100}}).pixel(1, 0), qRgb(0, 0, 0));
    QCOMPARE(maxDifference(apply(QStringLiteral("hue-saturation"), gradient(16, 8)), gradient(16, 8)), 0);
}

void TestFilters::colorBalanceNode()
{
    const QImage grey = solid(2, 2, qRgb(128, 128, 128));
    const QImage warm = apply(QStringLiteral("color-balance"), grey,
                              {{QStringLiteral("cyanRed"), 100}, {QStringLiteral("keepBrightness"), QStringLiteral("no")}});
    QVERIFY(qRed(warm.pixel(0, 0)) > 180);
    QCOMPARE(qGreen(warm.pixel(0, 0)), 128);
    QCOMPARE(qBlue(warm.pixel(0, 0)), 128);
    const QRgb kept = apply(QStringLiteral("color-balance"), grey, {{QStringLiteral("cyanRed"), 100}}).pixel(0, 0);
    QVERIFY(qRed(kept) > qGreen(kept));
    QCOMPARE(qGreen(kept), qBlue(kept));
    QVERIFY(std::abs((std::max({qRed(kept), qGreen(kept), qBlue(kept)}) + std::min({qRed(kept), qGreen(kept), qBlue(kept)})) / 2.0 - 128) <= 1.0);
    // The other way: cyan, on the shadows; white has no shadows and black no highlights.
    const QRgb cool = apply(QStringLiteral("color-balance"), solid(1, 1, qRgb(40, 40, 40)),
                            {{QStringLiteral("tones"), QStringLiteral("shadows")}, {QStringLiteral("cyanRed"), -100},
                             {QStringLiteral("keepBrightness"), QStringLiteral("no")}})
                          .pixel(0, 0);
    QVERIFY(qRed(cool) < 40);
    QCOMPARE(apply(QStringLiteral("color-balance"), solid(1, 1, qRgb(255, 255, 255)),
                   {{QStringLiteral("tones"), QStringLiteral("shadows")}, {QStringLiteral("yellowBlue"), 100}})
                 .pixel(0, 0),
             qRgb(255, 255, 255));
    QCOMPARE(apply(QStringLiteral("color-balance"), solid(1, 1, qRgb(0, 0, 0)),
                   {{QStringLiteral("tones"), QStringLiteral("highlights")}, {QStringLiteral("magentaGreen"), 100}})
                 .pixel(0, 0),
             qRgb(0, 0, 0));
}

void TestFilters::blurNode()
{
    // A bright square in the middle of a flat background, far enough from the edges that the
    // extended edge adds nothing: the blur keeps the mean and the symmetry.
    const QImage src = make(41, 41, [](int x, int y) {
        return std::abs(x - 20) <= 2 && std::abs(y - 20) <= 2 ? qRgb(200, 200, 200) : qRgb(100, 100, 100);
    });
    for (double radius : {1.0, 2.5, 6.0}) { // 6 uses the box passes
        const QImage out = apply(QStringLiteral("gaussian-blur"), src, {{QStringLiteral("radius"), radius}});
        QVERIFY2(std::abs(mean(out, qRed) - mean(src, qRed)) < 0.5, qPrintable(QString::number(radius)));
        QVERIFY(qRed(out.pixel(20, 20)) < 200);
        QVERIFY(qRed(out.pixel(20, 23)) > 100);
        QCOMPARE(maxDifference(out, out.flipped(Qt::Horizontal)), 0);
        QCOMPARE(maxDifference(out, out.flipped(Qt::Vertical)), 0);
        QCOMPARE(maxDifference(out, out.transformed(QTransform().rotate(90))), 0);
    }
    const QImage flat = solid(9, 9, qRgb(30, 60, 90));
    QCOMPARE(maxDifference(apply(QStringLiteral("gaussian-blur"), flat, {{QStringLiteral("radius"), 3.0}}), flat), 0);
    QCOMPARE(apply(QStringLiteral("gaussian-blur"), src, {{QStringLiteral("radius"), 0.0}}), src);
    // Blurred premultiplied: a transparent neighbour does not darken the colour. (QImage::pixel()
    // gives premultiplied values for a premultiplied image.)
    const QImage half = make(8, 1, [](int x, int) { return x < 4 ? qRgba(255, 0, 0, 255) : qRgba(0, 0, 0, 0); });
    const QRgb edge = apply(QStringLiteral("gaussian-blur"), half, {{QStringLiteral("radius"), 1.0}}).pixel(4, 0);
    QVERIFY(qAlpha(edge) > 0 && qAlpha(edge) < 255);
    QCOMPARE(qUnpremultiply(edge), qRgba(255, 0, 0, qAlpha(edge)));
}

void TestFilters::sharpenNodes()
{
    const QImage flat = solid(8, 8, qRgb(120, 120, 120));
    QCOMPARE(apply(QStringLiteral("sharpen"), flat), flat);
    QCOMPARE(apply(QStringLiteral("unsharp-mask"), flat), flat);
    const QImage step = make(12, 4, [](int x, int) { return x < 6 ? qRgb(100, 100, 100) : qRgb(150, 150, 150); });
    for (const QString &id : {QStringLiteral("sharpen"), QStringLiteral("unsharp-mask")}) {
        const QImage out = apply(id, step);
        QVERIFY2(qRed(out.pixel(5, 1)) < 100, qPrintable(id)); // darker before the edge
        QVERIFY2(qRed(out.pixel(6, 1)) > 150, qPrintable(id)); // brighter after it
        QCOMPARE(out.pixel(0, 1), qRgb(100, 100, 100)); // far from the edge nothing changes
        QCOMPARE(out.pixel(11, 1), qRgb(150, 150, 150));
    }
    QCOMPARE(apply(QStringLiteral("unsharp-mask"), step, {{QStringLiteral("threshold"), 255}}), step);
    QCOMPARE(apply(QStringLiteral("sharpen"), step, {{QStringLiteral("amount"), 0}}), step);
}

void TestFilters::noiseNode()
{
    const QImage grey = solid(32, 32, qRgb(128, 128, 128));
    const QImage a = apply(QStringLiteral("noise"), grey);
    QCOMPARE(apply(QStringLiteral("noise"), grey), a); // the same seed gives the same grain
    QVERIFY(apply(QStringLiteral("noise"), grey, {{QStringLiteral("seed"), 2}}) != a);
    QVERIFY(a != grey);
    QCOMPARE(apply(QStringLiteral("noise"), grey, {{QStringLiteral("amount"), 0}}), grey);
    for (int (*channel)(QRgb) : std::initializer_list<int (*)(QRgb)>{qRed, qGreen, qBlue})
        QVERIFY(std::abs(mean(a, channel) - 128.0) < 1.5);
    bool coloured = false;
    for (int x = 0; x < 32 && !coloured; ++x)
        coloured = qRed(a.pixel(x, 5)) != qGreen(a.pixel(x, 5));
    QVERIFY(coloured);
    const QImage mono = apply(QStringLiteral("noise"), grey, {{QStringLiteral("type"), QStringLiteral("mono")},
                                                              {QStringLiteral("distribution"), QStringLiteral("uniform")},
                                                              {QStringLiteral("amount"), 20}});
    int spread = 0;
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const QRgb p = mono.pixel(x, y);
            QVERIFY(qRed(p) == qGreen(p) && qGreen(p) == qBlue(p));
            QVERIFY(std::abs(qRed(p) - 128) <= 51); // uniform: at most the amount (20 % of 255)
            spread = std::max(spread, std::abs(qRed(p) - 128));
        }
    }
    QVERIFY(spread > 40);
}

void TestFilters::medianNode()
{
    // Salt and pepper on grey: a 3 x 3 median removes every speck.
    QImage specks = solid(16, 16, qRgb(100, 100, 100)).convertToFormat(QImage::Format_ARGB32);
    for (const QPoint &p : {QPoint(3, 3), QPoint(8, 5), QPoint(12, 12), QPoint(0, 0)})
        specks.setPixel(p, qRgb(255, 255, 255));
    specks.setPixel(5, 10, qRgb(0, 0, 0));
    QCOMPARE(apply(QStringLiteral("median"), specks), solid(16, 16, qRgb(100, 100, 100)));
    // A straight edge stays where it is.
    const QImage step = make(16, 8, [](int x, int) { return x < 8 ? qRgb(50, 60, 70) : qRgb(200, 210, 220); });
    QCOMPARE(apply(QStringLiteral("median"), step, {{QStringLiteral("radius"), 2}}), step);
    // Thin lines narrower than the window go.
    const QImage line = make(16, 16, [](int, int y) { return y == 8 ? qRgb(0, 0, 0) : qRgb(180, 180, 180); });
    QCOMPARE(apply(QStringLiteral("median"), line), solid(16, 16, qRgb(180, 180, 180)));
}

void TestFilters::blendNode()
{
    const QImage base = solid(4, 4, qRgb(100, 150, 200));
    auto blend = [&](const QImage &top, const QVariantMap &params, const QImage &mask = QImage()) {
        Values in{{QStringLiteral("base"), base}, {QStringLiteral("top"), top}};
        if (!mask.isNull())
            in.insert(QStringLiteral("mask"), mask);
        return evaluateNode(*type(QStringLiteral("blend")), in, params).value(QStringLiteral("image")).value<QImage>();
    };
    const QImage white = solid(4, 4, qRgb(255, 255, 255));
    const QImage black = solid(4, 4, qRgb(0, 0, 0));
    QCOMPARE(blend(white, {{QStringLiteral("mode"), QStringLiteral("multiply")}}), base);
    QCOMPARE(blend(black, {{QStringLiteral("mode"), QStringLiteral("screen")}}), base);
    QCOMPARE(blend(black, {{QStringLiteral("mode"), QStringLiteral("add")}}), base);
    QCOMPARE(blend(white, {}), white);
    QCOMPARE(blend(base, {{QStringLiteral("mode"), QStringLiteral("difference")}}), black);
    QCOMPARE(blend(white, {{QStringLiteral("mode"), QStringLiteral("darken")}}), base);
    QCOMPARE(blend(white, {{QStringLiteral("mode"), QStringLiteral("lighten")}}), white);
    // Overlay multiplies dark backgrounds and screens bright ones: black and white stay.
    const QImage red = solid(4, 4, qRgb(255, 0, 0));
    for (const QImage &extreme : {black, white}) {
        const Values in{{QStringLiteral("base"), extreme}, {QStringLiteral("top"), red}};
        QCOMPARE(evaluateNode(*type(QStringLiteral("blend")), in, {{QStringLiteral("mode"), QStringLiteral("overlay")}})
                     .value(QStringLiteral("image"))
                     .value<QImage>(),
                 extreme);
    }
    const QRgb halfway = blend(white, {{QStringLiteral("opacity"), 50}}).pixel(0, 0);
    QVERIFY(std::abs(qRed(halfway) - (100 + 155 / 2)) <= 1);
    // The mask limits where the foreground goes; a smaller foreground is stretched.
    const QImage left = maskOf(4, 4, [](int x, int) { return x < 2 ? 255 : 0; });
    const QImage masked = blend(solid(2, 2, qRgb(255, 255, 255)), {}, left);
    QCOMPARE(masked.pixel(0, 0), qRgb(255, 255, 255));
    QCOMPARE(masked.pixel(3, 3), qRgb(100, 150, 200));
    QCOMPARE(ops::blendModeIds().size(), 13); // one per doc::BlendMode
}

void TestFilters::mixNode()
{
    const QImage red = solid(5, 2, qRgb(255, 0, 0));
    const QImage blue = solid(5, 2, qRgb(0, 0, 255));
    const QImage mask = maskOf(5, 2, [](int x, int) { return x < 2 ? 0 : x == 2 ? 128 : 255; });
    const QImage out = evaluateNode(*type(QStringLiteral("mix")),
                                    {{QStringLiteral("outside"), red}, {QStringLiteral("inside"), blue}, {QStringLiteral("mask"), mask}})
                           .value(QStringLiteral("image"))
                           .value<QImage>();
    QCOMPARE(out.pixel(0, 0), qRgb(255, 0, 0));
    QCOMPARE(out.pixel(1, 1), qRgb(255, 0, 0));
    QCOMPARE(out.pixel(4, 0), qRgb(0, 0, 255));
    QVERIFY(std::abs(qRed(out.pixel(2, 0)) - 127) <= 1);
    QVERIFY(std::abs(qBlue(out.pixel(2, 0)) - 128) <= 1);
    // A missing mask is an error in plain words.
    try {
        evaluateNode(*type(QStringLiteral("mix")), {{QStringLiteral("outside"), red}, {QStringLiteral("inside"), blue}});
        QFAIL("a mix without a mask ran");
    } catch (const Error &e) {
        QVERIFY2(e.message().contains(QStringLiteral("Mask")), qPrintable(e.message()));
    }
}

void TestFilters::maskNodes()
{
    QImage src(3, 1, QImage::Format_ARGB32);
    src.setPixel(0, 0, qRgba(0, 0, 0, 255));
    src.setPixel(1, 0, qRgba(128, 128, 128, 100));
    src.setPixel(2, 0, qRgba(255, 255, 255, 255));
    const Values fromBrightness = evaluateNode(*type(QStringLiteral("mask-from-image")), {{QStringLiteral("image"), src}});
    const QImage luminance = fromBrightness.value(QStringLiteral("mask")).value<QImage>();
    QCOMPARE(luminance.format(), QImage::Format_Grayscale8);
    QCOMPARE(maskAt(luminance, 0, 0), 0);
    QCOMPARE(maskAt(luminance, 2, 0), 255);
    QVERIFY(std::abs(maskAt(luminance, 1, 0) - 128 * 100 / 255) <= 1); // transparent counts as unselected
    const QImage alpha = evaluateNode(*type(QStringLiteral("mask-from-image")), {{QStringLiteral("image"), src}},
                                      {{QStringLiteral("source"), QStringLiteral("alpha")}})
                             .value(QStringLiteral("mask"))
                             .value<QImage>();
    QCOMPARE(maskAt(alpha, 0, 0), 255);
    QCOMPARE(maskAt(alpha, 1, 0), 100);

    const QImage dot = maskOf(9, 9, [](int x, int y) { return x == 4 && y == 4 ? 255 : 0; });
    const QImage grown = applyToMask(QStringLiteral("grow-shrink-feather"), dot, {{QStringLiteral("grow"), 2}});
    QCOMPARE(img::maskBounds(grown), QRect(2, 2, 5, 5));
    QCOMPARE(img::maskCoverage(grown) * 81, 25.0);
    const QImage shrunk = applyToMask(QStringLiteral("grow-shrink-feather"), grown, {{QStringLiteral("grow"), -2}});
    QCOMPARE(shrunk, dot);
    const QImage square = maskOf(40, 40, [](int x, int y) { return x >= 10 && x < 30 && y >= 10 && y < 30 ? 255 : 0; });
    const QImage soft = applyToMask(QStringLiteral("grow-shrink-feather"), square, {{QStringLiteral("feather"), 4}});
    QCOMPARE(maskAt(soft, 20, 20), 255);
    QVERIFY(maskAt(soft, 10, 20) > 0 && maskAt(soft, 10, 20) < 255);
    QVERIFY(maskAt(soft, 8, 20) > 0); // the edge spreads out
    QVERIFY(std::abs(img::maskCoverage(soft) - img::maskCoverage(square)) < 0.01);

    const QImage values = maskOf(2, 1, [](int x, int) { return x == 0 ? 0 : 200; });
    const QImage inverted = applyToMask(QStringLiteral("invert-mask"), values);
    QCOMPARE(maskAt(inverted, 0, 0), 255);
    QCOMPARE(maskAt(inverted, 1, 0), 55);
}

void TestFilters::resizeNode()
{
    const QImage src = distinct(4, 2);
    const QImage twice = apply(QStringLiteral("resize"), src, {{QStringLiteral("percent"), 200}, {QStringLiteral("quality"), QStringLiteral("nearest")}});
    QCOMPARE(twice.size(), QSize(8, 4));
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 8; ++x)
            QCOMPARE(twice.pixel(x, y), src.pixel(x / 2, y / 2));
    const QImage half = apply(QStringLiteral("resize"), twice, {{QStringLiteral("percent"), 50}, {QStringLiteral("quality"), QStringLiteral("nearest")}});
    QCOMPARE(half, src);
    const QImage wide = apply(QStringLiteral("resize"), src, {{QStringLiteral("by"), QStringLiteral("size")}, {QStringLiteral("width"), 10}});
    QCOMPARE(wide.size(), QSize(10, 5)); // the height follows the proportions
    const QImage exact = apply(QStringLiteral("resize"), src,
                               {{QStringLiteral("by"), QStringLiteral("size")}, {QStringLiteral("width"), 3}, {QStringLiteral("height"), 7}});
    QCOMPARE(exact.size(), QSize(3, 7));
    const QImage smooth = apply(QStringLiteral("resize"), solid(8, 8, qRgb(10, 200, 30)), {{QStringLiteral("percent"), 25}});
    QCOMPARE(smooth, solid(2, 2, qRgb(10, 200, 30)));
}

void TestFilters::cropNode()
{
    const QImage src = distinct(5, 4);
    const QImage out = apply(QStringLiteral("crop"), src,
                             {{QStringLiteral("x"), 1}, {QStringLiteral("y"), 2}, {QStringLiteral("width"), 3}, {QStringLiteral("height"), 2}});
    QCOMPARE(out.size(), QSize(3, 2));
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 3; ++x)
            QCOMPARE(out.pixel(x, y), src.pixel(x + 1, y + 2));
    // Clipped to the image.
    QCOMPARE(apply(QStringLiteral("crop"), src, {{QStringLiteral("x"), 3}, {QStringLiteral("width"), 10}, {QStringLiteral("height"), 10}}).size(),
             QSize(2, 4));
    try {
        apply(QStringLiteral("crop"), src, {{QStringLiteral("x"), 50}});
        QFAIL("a crop outside the image ran");
    } catch (const Error &e) {
        QVERIFY2(e.message().contains(QStringLiteral("outside the image")), qPrintable(e.message()));
    }
}

void TestFilters::flipNode()
{
    const QImage src = distinct(3, 2);
    const QImage h = apply(QStringLiteral("flip"), src);
    const QImage v = apply(QStringLiteral("flip"), src, {{QStringLiteral("direction"), QStringLiteral("vertical")}});
    const QImage both = apply(QStringLiteral("flip"), src, {{QStringLiteral("direction"), QStringLiteral("both")}});
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 3; ++x) {
            QCOMPARE(h.pixel(x, y), src.pixel(2 - x, y));
            QCOMPARE(v.pixel(x, y), src.pixel(x, 1 - y));
            QCOMPARE(both.pixel(x, y), src.pixel(2 - x, 1 - y));
        }
    }
}

void TestFilters::rotateNode()
{
    const QImage src = distinct(3, 2); // W = 3, H = 2
    const QImage cw = apply(QStringLiteral("rotate"), src);
    QCOMPARE(cw.size(), QSize(2, 3));
    const QImage half = apply(QStringLiteral("rotate"), src, {{QStringLiteral("angle"), 180}});
    const QImage ccw = apply(QStringLiteral("rotate"), src, {{QStringLiteral("angle"), -90}});
    QCOMPARE(ccw, apply(QStringLiteral("rotate"), src, {{QStringLiteral("angle"), 270}}));
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 3; ++x) {
            QCOMPARE(cw.pixel(1 - y, x), src.pixel(x, y));        // clockwise: (x, y) -> (H - 1 - y, x)
            QCOMPARE(half.pixel(2 - x, 1 - y), src.pixel(x, y));
            QCOMPARE(ccw.pixel(y, 2 - x), src.pixel(x, y));       // (x, y) -> (y, W - 1 - x)
        }
    }
    QCOMPARE(apply(QStringLiteral("rotate"), src, {{QStringLiteral("angle"), 360}}), src);
    // Other angles grow the canvas; the corners are transparent.
    const QImage tilted = apply(QStringLiteral("rotate"), solid(20, 20, qRgb(255, 0, 0)), {{QStringLiteral("angle"), 45}});
    QVERIFY(tilted.width() >= 28 && tilted.width() <= 30);
    QCOMPARE(qAlpha(tilted.pixel(0, 0)), 0);
    QCOMPARE(tilted.pixel(tilted.width() / 2, tilted.height() / 2), qRgb(255, 0, 0));
}

void TestFilters::inputAndOutput()
{
    Evaluator evaluator;
    const QImage src = gradient(12, 8);
    Graph g = Graph::createDefault();
    QVERIFY(g.isValid());
    Outcome o = run(evaluator, g, {src.convertToFormat(QImage::Format_RGB32), {}});
    QVERIFY2(o.error.isEmpty(), qPrintable(o.error));
    QCOMPARE(o.result.image, src);
    QVERIFY(o.result.mask.isNull());
    QCOMPARE(o.result.evaluated, (QStringList{QStringLiteral("n1"), QStringLiteral("n2")}));

    // The selection reaches the output's mask; without a selection everything is selected.
    QVERIFY(g.connect(QStringLiteral("n1"), QStringLiteral("selection"), QStringLiteral("n2"), QStringLiteral("mask")));
    o = run(evaluator, g, {src, {}});
    QCOMPARE(o.result.mask, img::createMask(src.size(), 255));
    const QImage selection = maskOf(12, 8, [](int x, int) { return x < 6 ? 255 : 0; });
    o = run(evaluator, g, {src, selection});
    QCOMPARE(o.result.mask, selection);
    // A selection of another size is stretched to the image.
    o = run(evaluator, g, {src, maskOf(6, 4, [](int x, int) { return x < 3 ? 255 : 0; })});
    QCOMPARE(o.result.mask.size(), src.size());
    QCOMPARE(maskAt(o.result.mask, 0, 0), 255);
    QCOMPARE(maskAt(o.result.mask, 11, 7), 0);
    // A mask may feed an image input: it arrives as a grey image.
    QVERIFY(g.connect(QStringLiteral("n1"), QStringLiteral("selection"), QStringLiteral("n2"), QStringLiteral("image")));
    o = run(evaluator, g, {src, selection});
    QCOMPARE(o.result.image.pixel(0, 0), qRgb(255, 255, 255));
    QCOMPARE(o.result.image.pixel(11, 0), qRgb(0, 0, 0));
    // No image: an error in plain words.
    o = run(evaluator, g, {});
    QVERIFY(o.error.contains(QStringLiteral("no image")));
}

void TestFilters::validation()
{
    auto errors = [](const Graph &g) {
        QStringList out;
        for (const Issue &i : g.validate()) {
            if (i.severity == Issue::Severity::Error)
                out << i.message;
        }
        return out;
    };
    auto warnings = [](const Graph &g) {
        QStringList out;
        for (const Issue &i : g.validate()) {
            if (i.severity == Issue::Severity::Warning)
                out << i.message;
        }
        return out;
    };

    Graph empty;
    QCOMPARE(errors(empty).size(), 1);
    QVERIFY(errors(empty).first().contains(QStringLiteral("no output")));

    Graph two = Graph::createDefault();
    two.addNode(QStringLiteral("output"));
    QVERIFY(errors(two).join(QLatin1Char(' ')).contains(QStringLiteral("more than one output")));

    // A node that feeds the output needs its required inputs.
    Graph open;
    const QString levels = open.addNode(QStringLiteral("levels"));
    const QString out = open.addNode(QStringLiteral("output"));
    QVERIFY(open.connect(levels, QStringLiteral("image"), out, QStringLiteral("image")));
    QCOMPARE(errors(open), QStringList{QStringLiteral("“Levels” has nothing connected to its “Image” input.")});
    Evaluator evaluator;
    QCOMPARE(run(evaluator, open, {solid(2, 2, qRgb(1, 2, 3)), {}}).error, errors(open).first());

    // A node that does not feed the output only warns.
    Graph loose = Graph::createDefault();
    loose.addNode(QStringLiteral("gaussian-blur"));
    QVERIFY(loose.isValid());
    QCOMPARE(warnings(loose), QStringList{QStringLiteral("“Blur” is not connected to the output, so it has no effect.")});
    QVERIFY(run(evaluator, loose, {solid(2, 2, qRgb(1, 2, 3)), {}}).error.isEmpty());

    // Types, loops and published parameters, from a file (connect() refuses them).
    const QByteArray json = R"({
        "format": "lumen-filter", "version": 1,
        "nodes": [
            {"id": "in", "type": "input"}, {"id": "a", "type": "levels"}, {"id": "b", "type": "invert"},
            {"id": "m", "type": "invert-mask"}, {"id": "out", "type": "output"}
        ],
        "links": [
            {"from": "in", "output": "image", "to": "m", "input": "mask"},
            {"from": "b", "output": "image", "to": "a", "input": "image"},
            {"from": "a", "output": "image", "to": "b", "input": "image"},
            {"from": "b", "output": "image", "to": "out", "input": "image"},
            {"from": "m", "output": "mask", "to": "out", "input": "mask"}
        ],
        "published": [{"node": "a", "param": "nope", "label": "Strength"}]
    })";
    QString error;
    const std::optional<Graph> bad = Graph::fromJson(QJsonDocument::fromJson(json).object(), NodeRegistry::global(), &error);
    QVERIFY2(bad, qPrintable(error));
    const QString all = errors(*bad).join(QLatin1Char('\n'));
    QVERIFY2(all.contains(QStringLiteral("“Input” gives an image, but the “Mask” input of “Invert mask” takes a mask. "
                                         "Put a “Mask from image” node between them.")),
             qPrintable(all));
    QVERIFY2(all.contains(QStringLiteral("“Levels” is part of a loop")), qPrintable(all));
    QVERIFY2(all.contains(QStringLiteral("“Invert” is part of a loop")), qPrintable(all));
    QVERIFY2(all.contains(QStringLiteral("The slider “Strength” belongs to a setting that is not in the filter.")), qPrintable(all));
    QVERIFY(bad->evaluationOrder(QStringLiteral("out")).isEmpty());
    QVERIFY(!run(evaluator, *bad, {solid(2, 2, qRgb(1, 2, 3)), {}}).error.isEmpty());

    // A published slider needs a range that is not empty.
    Graph published = Graph::createDefault();
    const QString blur = published.addNode(QStringLiteral("gaussian-blur"));
    published.publish({blur, QStringLiteral("radius"), QStringLiteral("Softness"), 5, 2});
    QVERIFY(errors(published).join(QLatin1Char(' ')).contains(QStringLiteral("no range")));
    published.publish({blur, QStringLiteral("radius"), QStringLiteral("Softness"), 0, 20});
    QVERIFY(published.isValid());
    QCOMPARE(published.published().size(), 1);
    published.removeNode(blur);
    QVERIFY(published.published().isEmpty());
}

void TestFilters::connectRefusesBadLinks()
{
    Graph g = Graph::createDefault();
    const QString invertMask = g.addNode(QStringLiteral("invert-mask"));
    const QString blur = g.addNode(QStringLiteral("gaussian-blur"));
    QString error;
    QVERIFY(!g.connect(QStringLiteral("n1"), QStringLiteral("image"), invertMask, QStringLiteral("mask"), &error));
    QVERIFY2(error.contains(QStringLiteral("Mask from image")), qPrintable(error));
    QVERIFY(g.connect(QStringLiteral("n1"), QStringLiteral("selection"), invertMask, QStringLiteral("mask")));
    QVERIFY(!g.connect(QStringLiteral("n1"), QStringLiteral("image"), blur, QStringLiteral("nope"), &error));
    // Linking the blur into the output and back into itself would loop.
    QVERIFY(g.connect(QStringLiteral("n1"), QStringLiteral("image"), blur, QStringLiteral("image")));
    QVERIFY(g.connect(blur, QStringLiteral("image"), QStringLiteral("n2"), QStringLiteral("image")));
    QCOMPARE(g.links().size(), 3); // the output's old link was replaced
    QCOMPARE(g.linkInto(QStringLiteral("n2"), QStringLiteral("image"))->fromNode, blur);
    const QString sharpen = g.addNode(QStringLiteral("sharpen"));
    QVERIFY(g.connect(blur, QStringLiteral("image"), sharpen, QStringLiteral("image")));
    QVERIFY(!g.connect(sharpen, QStringLiteral("image"), blur, QStringLiteral("image"), &error));
    QVERIFY2(error.contains(QStringLiteral("loop")), qPrintable(error));
    QVERIFY(!g.connect(blur, QStringLiteral("image"), blur, QStringLiteral("image"), &error));
    QVERIFY(g.disconnect(sharpen, QStringLiteral("image")));
    QVERIFY(!g.disconnect(sharpen, QStringLiteral("image")));
    // Parameters are stored coerced.
    QVERIFY(g.setParam(blur, QStringLiteral("radius"), 9000));
    QCOMPARE(g.param(blur, QStringLiteral("radius")).toDouble(), 250.0);
    QVERIFY(!g.setParam(blur, QStringLiteral("nope"), 1));
    QCOMPARE(g.param(sharpen, QStringLiteral("amount")).toDouble(), 100.0); // the default
}

void TestFilters::jsonRoundTrip()
{
    const NodeRegistry registry = testRegistry();
    Graph g = Graph::createDefault(registry);
    g.name = QStringLiteral("Warm glow");
    g.description = QStringLiteral("Brighter, warmer midtones.");
    const QString levels = g.addNode(QStringLiteral("levels"), QPointF(120, 40), registry);
    const QString curves = g.addNode(QStringLiteral("curves"), QPointF(240, -20.5), registry);
    const QString blend = g.addNode(QStringLiteral("blend"), QPointF(360, 10), registry);
    const QString tint = g.addNode(QStringLiteral("test-tint"), QPointF(480, 10), registry);
    QVERIFY(g.setParam(levels, QStringLiteral("gamma"), 1.25));
    QVERIFY(g.setParam(levels, QStringLiteral("channel"), QStringLiteral("red")));
    QVERIFY(g.setParam(curves, QStringLiteral("points"), QStringLiteral("0 0, 100 140, 255 255")));
    QVERIFY(g.setParam(blend, QStringLiteral("mode"), QStringLiteral("soft-light")));
    QVERIFY(g.setParam(tint, QStringLiteral("color"), QColor(10, 20, 30, 40)));
    QVERIFY(g.setParam(tint, QStringLiteral("note"), QStringLiteral("ünïcödé")));
    g.node(tint)->params.insert(QStringLiteral("fromNewerVersion"), 7); // kept, not used
    QVERIFY(g.connect(QStringLiteral("n1"), QStringLiteral("image"), levels, QStringLiteral("image")));
    QVERIFY(g.connect(levels, QStringLiteral("image"), curves, QStringLiteral("image")));
    QVERIFY(g.connect(curves, QStringLiteral("image"), blend, QStringLiteral("base")));
    QVERIFY(g.connect(QStringLiteral("n1"), QStringLiteral("image"), blend, QStringLiteral("top")));
    QVERIFY(g.connect(QStringLiteral("n1"), QStringLiteral("selection"), blend, QStringLiteral("mask")));
    QVERIFY(g.connect(blend, QStringLiteral("image"), tint, QStringLiteral("image")));
    QVERIFY(g.connect(tint, QStringLiteral("image"), QStringLiteral("n2"), QStringLiteral("image")));
    g.publish({levels, QStringLiteral("gamma"), QStringLiteral("Brightness"), 0.5, 2.0});
    g.publish({blend, QStringLiteral("opacity"), QStringLiteral("Strength"), 0, 0});
    QVERIFY(g.isValid());

    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("warm.lumenfilter"));
    QString error;
    QVERIFY2(g.save(path, &error), qPrintable(error));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject saved = QJsonDocument::fromJson(file.readAll()).object();
    QCOMPARE(saved.value(QStringLiteral("version")).toInt(), Graph::FormatVersion);
    QCOMPARE(saved.value(QStringLiteral("format")).toString(), QStringLiteral("lumen-filter"));

    const std::optional<Graph> loaded = Graph::load(path, registry, &error);
    QVERIFY2(loaded, qPrintable(error));
    QCOMPARE(loaded->toJson(), g.toJson());
    QCOMPARE(loaded->name, g.name);
    QCOMPARE(loaded->description, g.description);
    QCOMPARE(loaded->links(), g.links());
    QCOMPARE(loaded->published(), g.published());
    QCOMPARE(loaded->node(curves)->position, QPointF(240, -20.5));
    QCOMPARE(loaded->param(levels, QStringLiteral("gamma")).toDouble(), 1.25);
    QCOMPARE(loaded->param(levels, QStringLiteral("channel")).toString(), QStringLiteral("red"));
    QCOMPARE(loaded->param(tint, QStringLiteral("color")).value<QColor>(), QColor(10, 20, 30, 40));
    QCOMPARE(loaded->param(tint, QStringLiteral("note")).toString(), QStringLiteral("ünïcödé"));
    QCOMPARE(loaded->node(tint)->params.value(QStringLiteral("fromNewerVersion")).toInt(), 7);
    QCOMPARE(loaded->param(levels, QStringLiteral("inWhite")).toDouble(), 255.0); // unset: the default

    // Both evaluate to the same image.
    Evaluator evaluator;
    const EvalInputs in{gradient(16, 12), maskOf(16, 12, [](int x, int) { return x * 16; })};
    const Outcome a = run(evaluator, g, in);
    Evaluator other;
    const Outcome b = run(other, *loaded, in);
    QVERIFY2(a.error.isEmpty() && b.error.isEmpty(), qPrintable(a.error + b.error));
    QCOMPARE(a.result.image, b.result.image);

    // A number input drives the parameter of the same name.
    const Values strong = evaluateNode(*registry.find(QStringLiteral("test-tint")),
                                       {{QStringLiteral("image"), solid(1, 1, qRgb(0, 0, 255))}, {QStringLiteral("strength"), 1.0}});
    QCOMPARE(strong.value(QStringLiteral("image")).value<QImage>().pixel(0, 0), qRgb(255, 0, 0));
}

void TestFilters::fileErrors()
{
    auto load = [](const QByteArray &json) {
        QString error;
        const std::optional<Graph> g = Graph::fromJson(QJsonDocument::fromJson(json).object(), NodeRegistry::global(), &error);
        return g ? QString() : error;
    };
    QCOMPARE(load(R"({"format": "lumen-filter", "version": 2, "nodes": []})"),
             QStringLiteral("This filter was made with a newer version of Lumen Studio (file version 2; this version reads "
                            "up to 1). Update Lumen Studio to use it."));
    QCOMPARE(load(R"({"format": "lumen-filter", "version": 1, "nodes": [{"id": "a", "type": "ai-edit"}]})"),
             QStringLiteral("This filter uses a node that this version of Lumen Studio does not have (“ai-edit”). It may "
                            "come from a plugin that is turned off."));
    QCOMPARE(load(R"({"nodes": []})"), QStringLiteral("This is not a Lumen filter file."));
    QVERIFY(load(R"({"format": "lumen-filter", "version": 1, "nodes": [{"id": "a", "type": "input"}],
                    "links": [{"from": "a", "output": "image", "to": "x", "input": "image"}]})")
                .contains(QStringLiteral("damaged")));
    QVERIFY(load(R"({"format": "lumen-filter", "version": 1,
                    "nodes": [{"id": "a", "type": "input"}, {"id": "a", "type": "output"}]})")
                .contains(QStringLiteral("damaged")));
    QVERIFY(load(R"({"format": "lumen-filter", "version": 1,
                    "nodes": [{"id": "a", "type": "input"}, {"id": "b", "type": "output"}],
                    "links": [{"from": "a", "output": "colour", "to": "b", "input": "image"}]})")
                .contains(QStringLiteral("has no “colour” output")));
    QVERIFY(load(R"({"format": "lumen-filter", "version": 1,
                    "nodes": [{"id": "a", "type": "input"}, {"id": "b", "type": "output"}]})")
                .isEmpty());

    QTemporaryDir dir;
    QString error;
    QVERIFY(!Graph::load(dir.filePath(QStringLiteral("missing.lumenfilter")), NodeRegistry::global(), &error));
    QVERIFY(error.startsWith(QStringLiteral("Could not open the filter")));
    QFile junk(dir.filePath(QStringLiteral("junk.lumenfilter")));
    QVERIFY(junk.open(QIODevice::WriteOnly));
    junk.write("not json");
    junk.close();
    QVERIFY(!Graph::load(junk.fileName(), NodeRegistry::global(), &error));
    QCOMPARE(error, QStringLiteral("This is not a Lumen filter file."));
}

void TestFilters::library()
{
    QTemporaryDir dir;
    const Library lib(dir.filePath(QStringLiteral("filters"))); // created on the first save
    QVERIFY(lib.entries().isEmpty());
    Graph warm = Graph::createDefault();
    warm.name = QStringLiteral("Warm: glow?");
    warm.description = QStringLiteral("Warmer");
    QString error;
    const QString warmPath = lib.save(warm, {}, &error);
    QVERIFY2(!warmPath.isEmpty(), qPrintable(error));
    QCOMPARE(QFileInfo(warmPath).fileName(), QStringLiteral("Warm glow.lumenfilter"));
    QCOMPARE(QFileInfo(lib.save(warm)).fileName(), QStringLiteral("Warm glow 2.lumenfilter"));
    Graph cold = Graph::createDefault();
    cold.name = QStringLiteral("Cold");
    QVERIFY(!lib.save(cold).isEmpty());
    Graph unnamed = Graph::createDefault();
    QCOMPARE(QFileInfo(lib.save(unnamed)).fileName(), QStringLiteral("Filter.lumenfilter"));

    auto write = [&](const QString &name, const QByteArray &data) {
        QFile f(QDir(lib.folder()).filePath(name));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(data);
    };
    write(QStringLiteral("Newer.lumenfilter"), R"({"format": "lumen-filter", "version": 99, "name": "Zoom", "nodes": []})");
    write(QStringLiteral("notes.txt"), "not a filter");

    const QList<Library::Entry> entries = lib.entries();
    QStringList names;
    for (const Library::Entry &e : entries)
        names << e.name;
    QCOMPARE(names, (QStringList{QStringLiteral("Cold"), QStringLiteral("Filter"), QStringLiteral("Newer"),
                                 QStringLiteral("Warm: glow?"), QStringLiteral("Warm: glow?")}));
    QVERIFY(entries[0].error.isEmpty());
    QVERIFY(entries[2].error.contains(QStringLiteral("newer version")));
    QCOMPARE(entries[3].description, QStringLiteral("Warmer"));
    QVERIFY(entries[3].modified.isValid());
    const std::optional<Graph> back = lib.load(entries[0].path);
    QVERIFY(back);
    QCOMPARE(back->name, QStringLiteral("Cold"));
}

void TestFilters::cacheRunsOnlyWhatChanged()
{
    counter().clear();
    const NodeRegistry registry = testRegistry();
    // input -> count (n2) -> levels (n3) -> count (n4) -> output (n5)
    const Graph base = chain({QStringLiteral("test-count"), QStringLiteral("levels"), QStringLiteral("test-count")}, registry);
    Graph g = base;
    Evaluator evaluator;
    const QImage src = gradient(24, 16);
    const QStringList all{QStringLiteral("n1"), QStringLiteral("n2"), QStringLiteral("n3"), QStringLiteral("n4"), QStringLiteral("n5")};

    Outcome first = run(evaluator, g, {src, {}});
    QVERIFY2(first.error.isEmpty(), qPrintable(first.error));
    QCOMPARE(first.result.evaluated, all);
    QCOMPARE(counter().value(QStringLiteral("n2")), 1);
    QCOMPARE(counter().value(QStringLiteral("n4")), 1);
    QCOMPARE(evaluator.cachedNodes(), 5);

    // Nothing changed: everything comes from the cache.
    Outcome again = run(evaluator, g, {src, {}});
    QVERIFY(again.result.evaluated.isEmpty());
    QCOMPARE(again.result.image, first.result.image);
    QCOMPARE(counter().value(QStringLiteral("n2")), 1);

    // One parameter: that node and what follows it run again, nothing before it.
    QVERIFY(g.setParam(QStringLiteral("n3"), QStringLiteral("gamma"), 1.8));
    Outcome changed = run(evaluator, g, {src, {}});
    QCOMPARE(changed.result.evaluated, (QStringList{QStringLiteral("n3"), QStringLiteral("n4"), QStringLiteral("n5")}));
    QCOMPARE(counter().value(QStringLiteral("n2")), 1);
    QCOMPARE(counter().value(QStringLiteral("n4")), 2);
    QVERIFY(changed.result.image != first.result.image);
    // Moving a node in the editor changes nothing.
    g.setPosition(QStringLiteral("n3"), QPointF(500, 500));
    QVERIFY(run(evaluator, g, {src, {}}).result.evaluated.isEmpty());

    // A copy of the same image is the same source; another image runs everything.
    const QImage copy = src;
    QVERIFY(run(evaluator, g, {copy, {}}).result.evaluated.isEmpty());
    QCOMPARE(run(evaluator, g, {gradient(24, 16), {}}).result.evaluated, all);
    // So does another selection, through the input node.
    const QImage selection = img::createMask(src.size(), 128);
    QCOMPARE(run(evaluator, g, {src, selection}).result.evaluated, all);
    QVERIFY(run(evaluator, g, {src, selection}).result.evaluated.isEmpty());

    // A removed node leaves the cache at the next evaluation.
    QVERIFY(g.removeNode(QStringLiteral("n4")));
    QVERIFY(g.connect(QStringLiteral("n3"), QStringLiteral("image"), QStringLiteral("n5"), QStringLiteral("image")));
    QCOMPARE(run(evaluator, g, {src, selection}).result.evaluated, QStringList{QStringLiteral("n5")});
    QCOMPARE(evaluator.cachedNodes(), 4);
    evaluator.clearCache();
    QCOMPARE(evaluator.cachedNodes(), 0);

    // Previewing one node runs only what it depends on.
    Evaluator fresh;
    EvalOptions preview;
    preview.target = QStringLiteral("n2");
    const Outcome part = run(fresh, base, {src, {}}, preview);
    QCOMPARE(part.result.evaluated, (QStringList{QStringLiteral("n1"), QStringLiteral("n2")}));
    QCOMPARE(part.result.outputs.value(QStringLiteral("image")).value<QImage>(), src);
    QVERIFY(part.result.image.isNull());
}

void TestFilters::previewScale()
{
    const QImage src = gradient(64, 48);
    const QImage selection = maskOf(64, 48, [](int x, int) { return x < 32 ? 255 : 0; });
    Evaluator evaluator;

    Graph blur = chain({QStringLiteral("gaussian-blur")});
    blur.setParam(QStringLiteral("n2"), QStringLiteral("radius"), 4.0);
    QVERIFY(blur.connect(QStringLiteral("n1"), QStringLiteral("selection"), QStringLiteral("n3"), QStringLiteral("mask")));
    EvalOptions half;
    half.scale = 0.5;
    const Outcome preview = run(evaluator, blur, {src, selection}, half);
    QVERIFY2(preview.error.isEmpty(), qPrintable(preview.error));
    QCOMPARE(preview.result.scale, 0.5);
    QCOMPARE(preview.result.image.size(), QSize(32, 24));
    QCOMPARE(preview.result.mask.size(), QSize(32, 24));
    // The preview blurs the half-size image by half the radius, so it looks like the result.
    const QImage small = img::scaled(src, QSize(32, 24), true);
    QCOMPARE(preview.result.image, ops::gaussianBlur(small, 2.0));
    // Applying runs again at full resolution (the scale is part of every key).
    const Outcome full = run(evaluator, blur, {src, selection});
    QCOMPARE(full.result.evaluated.size(), 3);
    QCOMPARE(full.result.image, ops::gaussianBlur(src, 4.0));
    QCOMPARE(full.result.scale, 1.0);
    QCOMPARE(run(evaluator, blur, {src, selection}).result.evaluated, QStringList());

    // Pixel positions and sizes follow the scale; percentages and angles do not need to.
    Graph crop = chain({QStringLiteral("crop")});
    crop.setParam(QStringLiteral("n2"), QStringLiteral("x"), 8);
    crop.setParam(QStringLiteral("n2"), QStringLiteral("y"), 8);
    crop.setParam(QStringLiteral("n2"), QStringLiteral("width"), 32);
    crop.setParam(QStringLiteral("n2"), QStringLiteral("height"), 16);
    QCOMPARE(run(evaluator, crop, {src, {}}).result.image.size(), QSize(32, 16));
    EvalOptions quarter;
    quarter.scale = 0.25;
    const Outcome small4 = run(evaluator, crop, {src, {}}, quarter);
    QCOMPARE(small4.result.image.size(), QSize(8, 4));
    QCOMPARE(small4.result.image, img::scaled(src, QSize(16, 12), true).copy(2, 2, 8, 4));
    // Out-of-range scales mean full resolution.
    EvalOptions odd;
    odd.scale = 3.0;
    QCOMPARE(run(evaluator, crop, {src, {}}, odd).result.scale, 1.0);
}

void TestFilters::cancellation()
{
    const NodeRegistry registry = testRegistry();
    counter().clear();
    // input -> slow (n2) -> count (n3) -> output
    const Graph g = chain({QStringLiteral("test-slow"), QStringLiteral("test-count")}, registry);
    const QImage src = gradient(8, 8);

    {
        Evaluator evaluator;
        slow().sawCancel = false;
        QFuture<EvalResult> future = evaluator.evaluate(g, {src, {}});
        QVERIFY(slow().started.tryAcquire(1, 5000));
        evaluator.cancel();
        const Outcome o = finish(future);
        QVERIFY(o.cancelled);
        QVERIFY(slow().sawCancel);
        QCOMPARE(counter().value(QStringLiteral("n3")), 0); // nothing after the cancelled node ran
        QCOMPARE(evaluator.cachedNodes(), 1);               // the input; not the interrupted node
    }
    {
        // Cancelling the future works too.
        Evaluator evaluator;
        slow().sawCancel = false;
        QFuture<EvalResult> future = evaluator.evaluate(g, {src, {}});
        QVERIFY(slow().started.tryAcquire(1, 5000));
        future.cancel();
        QVERIFY(finish(future).cancelled);
        QVERIFY(slow().sawCancel);
        QCOMPARE(counter().value(QStringLiteral("n3")), 0);
    }
    {
        // Destroying the evaluator cancels what runs.
        slow().sawCancel = false;
        QFuture<EvalResult> future;
        {
            Evaluator evaluator;
            future = evaluator.evaluate(g, {src, {}});
            QVERIFY(slow().started.tryAcquire(1, 5000));
        }
        QVERIFY(finish(future).cancelled);
        QVERIFY(slow().sawCancel);
    }
}

void TestFilters::asynchronousNode()
{
    const NodeRegistry registry = testRegistry();
    const Graph g = chain({QStringLiteral("test-later"), QStringLiteral("invert")}, registry);
    const QImage src = gradient(10, 6);
    const int threads = async::pool()->maxThreadCount();
    async::setMaxThreads(1);
    auto restore = qScopeGuard([threads] { async::setMaxThreads(threads); });

    {
        Evaluator evaluator;
        QFuture<EvalResult> future = evaluator.evaluate(g, {src, {}});
        QVERIFY(later().started.tryAcquire(1, 5000));
        // While the node waits for its job, the only worker thread is free for other work.
        QSemaphore otherRan;
        async::run([&otherRan] { otherRan.release(); });
        QVERIFY(otherRan.tryAcquire(1, 5000));
        QVERIFY(!future.isFinished());
        // The "job" finishes on another thread; the evaluation continues on the pool.
        const std::shared_ptr<QPromise<Values>> promise = later().take();
        QCOMPARE(later().input, src);
        QThread *thread = QThread::create([promise] {
            promise->addResult(Values{{QStringLiteral("image"), solid(10, 6, qRgb(255, 0, 0))}});
            promise->finish();
        });
        thread->start();
        const Outcome o = finish(future);
        thread->wait();
        delete thread;
        QVERIFY2(o.error.isEmpty(), qPrintable(o.error));
        QCOMPARE(o.result.image, solid(10, 6, qRgb(0, 255, 255)));
        QCOMPARE(o.result.evaluated.size(), 4);
        // Its result is cached like any other.
        QVERIFY(run(evaluator, g, {src, {}}).result.evaluated.isEmpty());
    }
    {
        // A failed job fails the evaluation with the node's name.
        Evaluator evaluator;
        QFuture<EvalResult> future = evaluator.evaluate(g, {src, {}});
        QVERIFY(later().started.tryAcquire(1, 5000));
        const std::shared_ptr<QPromise<Values>> promise = later().take();
        promise->setException(Error("The server is not running."));
        promise->finish();
        QCOMPARE(finish(future).error, QStringLiteral("“test-later”: The server is not running."));
    }
    {
        // Cancelling while the node waits finishes at once and tells the node.
        Evaluator evaluator;
        later().cancelHandlerRan = false;
        QFuture<EvalResult> future = evaluator.evaluate(g, {src, {}});
        QVERIFY(later().started.tryAcquire(1, 5000));
        const std::shared_ptr<QPromise<Values>> promise = later().take();
        evaluator.cancel();
        QVERIFY(finish(future).cancelled);
        QVERIFY(later().cancelHandlerRan);
        QVERIFY(promise->isCanceled());
        promise->finish(); // a late finish is harmless
    }
    {
        // A job that is cancelled on its own side cancels the evaluation.
        Evaluator evaluator;
        QFuture<EvalResult> future = evaluator.evaluate(g, {src, {}});
        QVERIFY(later().started.tryAcquire(1, 5000));
        const std::shared_ptr<QPromise<Values>> promise = later().take();
        promise->future().cancel();
        promise->finish();
        QVERIFY(finish(future).cancelled);
    }
}

void TestFilters::expensiveNodeWaitsForRun()
{
    counter().clear();
    const NodeRegistry registry = testRegistry();
    // input -> expensive (n2) -> levels (n3) -> output
    Graph g = chain({QStringLiteral("test-expensive"), QStringLiteral("levels")}, registry);
    QVERIFY(g.needsExplicitRun());
    QVERIFY(!Graph::createDefault().needsExplicitRun());
    const QImage src = gradient(8, 8);
    Evaluator evaluator;
    EvalOptions preview;
    preview.runExpensive = false;

    const Outcome waiting = run(evaluator, g, {src, {}}, preview);
    QVERIFY(waiting.error.isEmpty());
    QVERIFY(waiting.result.needsRun);
    QVERIFY(waiting.result.image.isNull());
    QCOMPARE(counter().value(QStringLiteral("n2")), 0);

    const Outcome ran = run(evaluator, g, {src, {}}); // Run
    QVERIFY(!ran.result.needsRun);
    QCOMPARE(ran.result.image, ops::invert(src));
    QCOMPARE(counter().value(QStringLiteral("n2")), 1);

    // After the run, the step's result is reused and a slider after it previews live.
    g.setParam(QStringLiteral("n3"), QStringLiteral("gamma"), 2.0);
    const Outcome live = run(evaluator, g, {src, {}}, preview);
    QVERIFY(!live.result.needsRun);
    QCOMPARE(live.result.evaluated, (QStringList{QStringLiteral("n3"), QStringLiteral("n4")}));
    QCOMPARE(counter().value(QStringLiteral("n2")), 1);
}

void TestFilters::goldenImage()
{
    // tests/data/filters/golden.lumenfilter: levels, hue and saturation, blur, overlay with the
    // original, posterize, and the result mixed into the original through the feathered
    // selection. Every node of it is checked analytically above. To regenerate golden.png after
    // an intended change, run: LUMEN_UPDATE_GOLDEN=1 tst_filters goldenImage
    QString error;
    const std::optional<Graph> graph = Graph::load(dataPath(QStringLiteral("golden.lumenfilter")), NodeRegistry::global(), &error);
    QVERIFY2(graph, qPrintable(error));
    QVERIFY(graph->isValid());
    QCOMPARE(graph->published().size(), 1);
    Evaluator evaluator;
    const Outcome o = run(evaluator, *graph, {goldenSource(), goldenSelection()});
    QVERIFY2(o.error.isEmpty(), qPrintable(o.error));
    QCOMPARE(o.result.image.size(), QSize(48, 32));
    // Outside the feathered selection the original stays.
    QCOMPARE(o.result.image.pixel(0, 0), goldenSource().pixel(0, 0));
    QVERIFY(o.result.image.pixel(24, 16) != goldenSource().pixel(24, 16));

    const QString goldenPath = dataPath(QStringLiteral("golden.png"));
    if (qEnvironmentVariableIntValue("LUMEN_UPDATE_GOLDEN") == 1) {
        QVERIFY(o.result.image.convertToFormat(QImage::Format_ARGB32).save(goldenPath, "PNG"));
        QSKIP("golden.png written");
    }
    const QImage golden(goldenPath);
    QVERIFY2(!golden.isNull(), qPrintable(goldenPath));
    // One level of slack for another compiler's floating point (pow, exp in the nodes).
    const int difference = maxDifference(o.result.image, img::normalized(golden));
    QVERIFY2(difference <= 1, qPrintable(QStringLiteral("largest difference to golden.png: %1").arg(difference)));
}

QTEST_MAIN(TestFilters)
#include "tst_filters.moc"
