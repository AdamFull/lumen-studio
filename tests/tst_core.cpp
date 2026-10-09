#include "core/ImageOps.h"
#include "core/JobQueue.h"
#include "core/JsonSchema.h"
#include "core/JsonUtil.h"

#include <QElapsedTimer>
#include <QPainter>
#include <QSignalSpy>
#include <QtTest>

using namespace lumen;

class TestCore : public QObject
{
    Q_OBJECT
private slots:
    void maskBoundsAndEmpty();
    void dilateErode();
    void blurKeepsRange();
    void expandRegionAspect();
    void pixelBudget();
    void base64RoundTrip();
    void compositeMasked();
    void colorMatchCorrectsShift();
    void fillTransparentFromEdges();
    void dilatePerformance();
    void schemaValidation();
    void strictSchema();
    void extractJsonFromProse();
    void jobQueueConcurrency();
    void jobCancelQueued();
};

void TestCore::maskBoundsAndEmpty()
{
    QImage m = img::createMask(QSize(100, 80));
    QVERIFY(img::maskIsEmpty(m));
    QVERIFY(img::maskBounds(m).isNull());
    for (int y = 10; y <= 20; ++y)
        for (int x = 30; x <= 45; ++x)
            m.scanLine(y)[x] = 255;
    QVERIFY(!img::maskIsEmpty(m));
    QCOMPARE(img::maskBounds(m), QRect(QPoint(30, 10), QPoint(45, 20)));
}

void TestCore::dilateErode()
{
    QImage m = img::createMask(QSize(64, 64));
    m.scanLine(32)[32] = 255;
    const QImage d = img::dilateMask(m, 3);
    QCOMPARE(img::maskBounds(d), QRect(QPoint(29, 29), QPoint(35, 35)));
    const QImage e = img::erodeMask(d, 3);
    QCOMPARE(img::maskBounds(e), QRect(QPoint(32, 32), QPoint(32, 32)));
    // Erosion must not shrink a mask from the canvas edge.
    QImage full = img::createMask(QSize(16, 16), 255);
    QVERIFY(!img::maskIsEmpty(img::erodeMask(full, 4)));
    QCOMPARE(img::maskBounds(img::erodeMask(full, 4)), QRect(0, 0, 16, 16));
}

void TestCore::blurKeepsRange()
{
    QImage m = img::createMask(QSize(50, 50));
    for (int y = 13; y < 37; ++y)
        for (int x = 13; x < 37; ++x)
            m.scanLine(y)[x] = 255;
    const QImage b = img::blurMask(m, 8);
    QVERIFY(b.scanLine(25)[25] > 200);
    QVERIFY(b.scanLine(25)[10] > 0);
    QVERIFY(b.scanLine(25)[10] < 128);
    QCOMPARE(int(b.scanLine(0)[0]), 0);
}

void TestCore::expandRegionAspect()
{
    const QRect r = img::expandRegion(QRect(100, 100, 50, 20), 10, QSize(400, 300), 1.0);
    QCOMPARE(r.width(), r.height());
    QVERIFY(r.contains(QRect(100, 100, 50, 20)));
    // Near the border the region is shifted inside instead of clipped.
    const QRect edge = img::expandRegion(QRect(0, 0, 40, 40), 8, QSize(400, 300), 2.0);
    QCOMPARE(edge.topLeft(), QPoint(0, 0));
    QCOMPARE(edge.width(), 2 * edge.height());
}

void TestCore::pixelBudget()
{
    const QSize s = img::fitToPixelBudget(QSize(300, 200), 1024 * 1024, 64);
    QCOMPARE(s.width() % 64, 0);
    QCOMPARE(s.height() % 64, 0);
    QVERIFY(std::abs(double(s.width()) / s.height() - 1.5) < 0.1);
}

void TestCore::base64RoundTrip()
{
    QImage im(16, 8, QImage::Format_ARGB32);
    im.fill(QColor(10, 20, 30, 200));
    const QImage back = img::fromBase64(img::toDataUrl(im).toLatin1());
    QCOMPARE(back.size(), im.size());
    QCOMPARE(back.pixelColor(3, 3), QColor(10, 20, 30, 200));
}

void TestCore::compositeMasked()
{
    QImage base(10, 10, QImage::Format_ARGB32_Premultiplied);
    base.fill(Qt::black);
    QImage overlay(4, 4, QImage::Format_ARGB32_Premultiplied);
    overlay.fill(Qt::white);
    QImage mask = img::createMask(QSize(4, 4), 255);
    mask.scanLine(0)[0] = 0;
    const QImage out = img::compositeMasked(base, overlay, mask, QPoint(2, 2));
    QCOMPARE(out.pixelColor(3, 3), QColor(Qt::white));
    QCOMPARE(out.pixelColor(2, 2), QColor(Qt::black));
    QCOMPARE(out.pixelColor(8, 8), QColor(Qt::black));
}

void TestCore::colorMatchCorrectsShift()
{
    QImage ref(32, 32, QImage::Format_ARGB32);
    QImage gen(32, 32, QImage::Format_ARGB32);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            const int v = (x * 7 + y * 3) % 200;
            ref.setPixelColor(x, y, QColor(v, v / 2, 50));
            gen.setPixelColor(x, y, QColor(qMin(255, v + 30), v / 2 + 10, 70));
        }
    QImage mask = img::createMask(QSize(32, 32));
    const QImage fixed = img::colorMatch(gen, ref, mask);
    const QColor a = fixed.pixelColor(10, 10), b = ref.pixelColor(10, 10);
    QVERIFY(std::abs(a.red() - b.red()) <= 3);
    QVERIFY(std::abs(a.blue() - b.blue()) <= 3);
}

void TestCore::fillTransparentFromEdges()
{
    QImage im(64, 64, QImage::Format_ARGB32_Premultiplied);
    im.fill(Qt::transparent);
    QPainter p(&im);
    p.fillRect(0, 0, 32, 64, Qt::red);
    p.end();
    const QImage out = img::fillTransparentFromEdges(im);
    QCOMPARE(out.pixelColor(10, 10), QColor(Qt::red));
    const QColor filled = out.pixelColor(60, 30);
    QCOMPARE(filled.alpha(), 255);
    QVERIFY(filled.red() > 200);
}

void TestCore::dilatePerformance()
{
    QImage m = img::createMask(QSize(2048, 2048));
    for (int y = 900; y < 1100; ++y)
        for (int x = 900; x < 1100; ++x)
            m.scanLine(y)[x] = 255;
    QElapsedTimer t;
    t.start();
    const QImage d = img::dilateMask(m, 64);
    const QImage b = img::blurMask(d, 32);
    QVERIFY(!b.isNull());
    qInfo() << "dilate(64)+blur(32) on 2048^2 took" << t.elapsed() << "ms";
    QVERIFY(t.elapsed() < 4000);
}

void TestCore::schemaValidation()
{
    const QJsonObject schema = QJsonDocument::fromJson(R"({
        "type":"object",
        "properties":{
            "shots":{"type":"array","minItems":1,"items":{"$ref":"#/$defs/shot"}},
            "mode":{"type":"string","enum":["a","b"]}
        },
        "required":["shots"],
        "additionalProperties":false,
        "$defs":{"shot":{"type":"object","properties":{"n":{"type":"integer","minimum":1}},"required":["n"]}}
    })").object();
    json::SchemaValidator v(schema);
    QVERIFY(v.validate(QJsonDocument::fromJson(R"({"shots":[{"n":1},{"n":3}],"mode":"a"})").object()));
    QStringList errors;
    QVERIFY(!v.validate(QJsonDocument::fromJson(R"({"shots":[{"n":0},{}],"mode":"c","x":1})").object(), &errors));
    QCOMPARE(errors.size(), 4);
    QVERIFY(errors.join(u'|').contains(QStringLiteral("/shots/1: missing required property")));
}

void TestCore::strictSchema()
{
    const QJsonObject schema = QJsonDocument::fromJson(R"({"type":"object","properties":{"a":{"type":"object","properties":{"b":{"type":"number","minimum":0}}}}})").object();
    const QJsonObject strict = json::toStrictSchema(schema);
    QCOMPARE(strict.value(QStringLiteral("additionalProperties")).toBool(true), false);
    const QJsonObject a = strict.value(QStringLiteral("properties")).toObject().value(QStringLiteral("a")).toObject();
    QCOMPARE(a.value(QStringLiteral("required")).toArray().size(), 1);
    QVERIFY(!a.value(QStringLiteral("properties")).toObject().value(QStringLiteral("b")).toObject().contains(QStringLiteral("minimum")));
}

void TestCore::extractJsonFromProse()
{
    QCOMPARE(json::extractJson(QStringLiteral("Sure!\n```json\n{\"a\": 1}\n```\nDone")).toObject().value(QStringLiteral("a")).toInt(), 1);
    QCOMPARE(json::extractJson(QStringLiteral("<think>{bad</think> Here: {\"b\": [1, {\"c\": \"}\"}]} trailing")).toObject().value(QStringLiteral("b")).toArray().size(), 2);
    QVERIFY(json::extractJson(QStringLiteral("no json here")).isUndefined());
}

void TestCore::jobQueueConcurrency()
{
    JobQueue queue;
    queue.setConcurrency(QStringLiteral("g"), 1);
    int running = 0, maxRunning = 0, done = 0;
    QList<Job *> jobs;
    for (int i = 0; i < 3; ++i) {
        jobs << queue.submit(QStringLiteral("job %1").arg(i), QStringLiteral("g"), [&](Job *job) {
            ++running;
            maxRunning = qMax(maxRunning, running);
            QTimer::singleShot(10, job, [&, job] {
                --running;
                ++done;
                job->succeed(42);
            });
        });
    }
    QTRY_COMPARE(done, 3);
    QCOMPARE(maxRunning, 1);
    QCOMPARE(jobs[2]->result().toInt(), 42);
    QCOMPARE(queue.activeCount(), 0);
}

void TestCore::jobCancelQueued()
{
    JobQueue queue;
    queue.setConcurrency(QStringLiteral("g"), 1);
    bool cancelHandlerCalled = false;
    Job *a = queue.submit(QStringLiteral("a"), QStringLiteral("g"), [&](Job *job) {
        job->setCancelHandler([&, job] {
            cancelHandlerCalled = true;
            job->reportCancelled();
        });
    });
    Job *b = queue.submit(QStringLiteral("b"), QStringLiteral("g"), [](Job *) { QFAIL("must not start"); });
    QTRY_COMPARE(a->state(), Job::State::Running);
    b->cancel();
    QCOMPARE(b->state(), Job::State::Cancelled);
    a->cancel();
    QVERIFY(cancelHandlerCalled);
    QCOMPARE(a->state(), Job::State::Cancelled);
    queue.retry(b);
    QCOMPARE(b->state(), Job::State::Queued);
    queue.remove(b);
}

QTEST_MAIN(TestCore)
#include "tst_core.moc"
