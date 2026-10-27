#include "core/CrashGuard.h"
#include "core/Finders.h"
#include "core/ImageOps.h"
#include "core/JobQueue.h"
#include "core/JsonSchema.h"
#include "core/JsonUtil.h"

#include <QElapsedTimer>
#include <QPainter>
#include <QSignalSpy>
#include <QTemporaryDir>
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
    void pngIsCompressed();
    void compositeMasked();
    void colorMatchCorrectsShift();
    void fillTransparentFromEdges();
    void dilatePerformance();
    void schemaValidation();
    void strictSchema();
    void extractJsonFromProse();
    void jobQueueConcurrency();
    void jobCancelQueued();
    void sessionLockDetectsCrash();
    void finders();
    void finderWords();
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
    // Odd widths and pixels at the row ends (the scan skips zero runs in 8-byte words).
    for (int w : {1, 7, 9, 61}) {
        QImage edge = img::createMask(QSize(w, 5));
        edge.scanLine(4)[w - 1] = 3;
        edge.scanLine(1)[0] = 200;
        QCOMPARE(img::maskBounds(edge), QRect(QPoint(0, 1), QPoint(w - 1, 4)));
        QCOMPARE(img::maskBounds(edge, 10), QRect(0, 1, 1, 1)); // the faint pixel is below the threshold
        QVERIFY(!img::maskIsEmpty(edge, 200));
        QVERIFY(img::maskIsEmpty(edge, 201));
    }
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

void TestCore::pngIsCompressed()
{
    // A flat 1216 x 1664 image: stored without compression it is about 8 MB.
    QImage image(1216, 1664, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(128, 128, 128));
    for (int level : {-1, 1, 3, 6, 9}) {
        const QByteArray png = img::encode(image, "PNG", level);
        QVERIFY2(png.size() < 200 * 1024, qPrintable(QStringLiteral("level %1: %2 bytes").arg(level).arg(png.size())));
        QCOMPARE(img::decode(png).convertToFormat(QImage::Format_ARGB32), image.convertToFormat(QImage::Format_ARGB32));
    }
    QVERIFY(img::encode(image, "PNG", 0).size() > 4 * 1024 * 1024); // level 0 stores
    QVERIFY(img::encode(image, "PNG", 9).size() <= img::encode(image, "PNG", 1).size());
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

    // Work past its commit (a moved models folder): a cancel that came before is dropped, later
    // ones do nothing, and it still succeeds.
    Job *c = queue.submit(QStringLiteral("c"), QStringLiteral("h"), [](Job *job) { job->setCancelHandler([] {}); });
    QTRY_COMPARE(c->state(), Job::State::Running);
    c->cancel();
    QVERIFY(c->isCancelRequested());
    c->ignoreCancel();
    QVERIFY(!c->isCancelRequested());
    c->cancel();
    QVERIFY(!c->isCancelRequested());
    c->succeed(7);
    QCOMPARE(c->state(), Job::State::Succeeded);
    QCOMPARE(c->result().toInt(), 7);
    // A retry can be cancelled again.
    Job *d = queue.submit(QStringLiteral("d"), QStringLiteral("h"), [](Job *) {});
    QTRY_COMPARE(d->state(), Job::State::Running);
    d->ignoreCancel();
    d->fail(QStringLiteral("x"));
    queue.retry(d);
    QCOMPARE(d->state(), Job::State::Queued);
    d->cancel();
    QCOMPARE(d->state(), Job::State::Cancelled);
}

void TestCore::sessionLockDetectsCrash()
{
    QTemporaryDir dir;
    QVERIFY(crash::SessionLock::processAlive(QCoreApplication::applicationPid()));
    QVERIFY(!crash::SessionLock::processAlive(-1));
    {
        crash::SessionLock first(dir.path());
        QVERIFY(!first.previousSessionCrashed());
        QVERIFY(QFile::exists(first.lockFile()));
        first.release();
        QVERIFY(!QFile::exists(first.lockFile()));
    }
    // A lock left by a process that no longer exists = the previous session crashed.
    qint64 deadPid = 999990;
    while (crash::SessionLock::processAlive(deadPid))
        --deadPid;
    QFile stale(dir.filePath(QStringLiteral("session-%1.lock").arg(deadPid)));
    QVERIFY(stale.open(QIODevice::WriteOnly));
    stale.write("started=2026-01-01T00:00:00 pid=1 log=x\n");
    stale.close();
    crash::SessionLock second(dir.path());
    QVERIFY(second.previousSessionCrashed());
    QCOMPARE(second.staleSessions().size(), 1);
    QVERIFY(second.staleSessions().first().startsWith(QLatin1String("started=2026-01-01")));
    QVERIFY(!stale.exists());
    // Without a release (a crash) the next start sees this session's own lock as stale too.
    crash::SessionLock third(dir.path());
    QVERIFY(third.previousSessionCrashed());
    third.release();
    QVERIFY(crash::reports(dir.path()).isEmpty());
}

namespace {

// Paper (off-white with a gentle gradient) with thin black pen lines, a large dark coat, a few
// specks of dust and a red and a blue patch.
QImage scannedDrawing()
{
    QImage im(400, 300, QImage::Format_ARGB32);
    for (int y = 0; y < im.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(im.scanLine(y));
        for (int x = 0; x < im.width(); ++x) {
            const int v = 236 - x / 40; // a scan's uneven light
            line[x] = qRgb(v, v, v - 6);
        }
    }
    QPainter p(&im);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(25, 25, 25), 3));
    p.drawLine(20, 40, 380, 60);   // a long stroke
    p.drawLine(60, 280, 160, 120); // another
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(30, 30, 30));
    p.drawRect(240, 150, 120, 120); // a large dark shape: not a line
    p.setBrush(QColor(60, 60, 60));
    for (const QPoint &d : {QPoint(200, 100), QPoint(120, 200), QPoint(330, 110)})
        p.drawEllipse(d, 2, 2);     // dust
    p.setBrush(QColor(200, 30, 30));
    p.drawRect(20, 100, 40, 30);    // red
    p.setBrush(QColor(140, 20, 20));
    p.drawRect(20, 140, 40, 20);    // dark red
    p.setBrush(QColor(30, 60, 210));
    p.drawRect(80, 100, 40, 30);    // blue
    return im;
}

int on(const QImage &mask, int x, int y)
{
    return qGray(mask.pixel(x, y)) > 127;
}

double coverage(const QImage &mask, const QRect &r)
{
    int n = 0, in = 0;
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x, ++n)
            in += on(mask, x, y);
    return n ? double(in) / n : 0;
}

} // namespace

// The classic finders of plan M18 §18.1 on a scanned drawing: lines but not the large dark shape,
// dust but not the lines, colours by their hue (dark red is red), and a colour picked exactly.
void TestCore::finders()
{
    const QImage drawing = scannedDrawing();
    const QImage lines = img::findStrokes(drawing);
    QCOMPARE(lines.format(), QImage::Format_Grayscale8);
    QCOMPARE(lines.size(), drawing.size());
    QVERIFY(on(lines, 200, 50));            // on the first stroke
    QVERIFY(on(lines, 110, 200));           // on the second
    QVERIFY(coverage(lines, QRect(260, 170, 80, 80)) < 0.01); // inside the dark shape
    QVERIFY(coverage(lines, QRect(300, 10, 60, 20)) < 0.01);  // plain paper
    QVERIFY(!on(lines, 200, 100));          // a speck is too short for a line
    // Light strokes on dark ground are left alone when dark ones are asked for, and found otherwise.
    QImage negative = drawing;
    negative.invertPixels();
    QVERIFY(coverage(img::findStrokes(negative), QRect(150, 45, 100, 10)) < 0.05);
    img::StrokeOptions lightStrokes;
    lightStrokes.dark = false;
    QVERIFY(on(img::findStrokes(negative, lightStrokes), 200, 50));
    // A stroke wider than the widest asked for is not one: a 14 px band against a 3 px line.
    QImage bands(300, 200, QImage::Format_ARGB32);
    bands.fill(QColor(235, 235, 230));
    {
        QPainter p(&bands);
        p.fillRect(QRect(20, 40, 260, 14), QColor(20, 20, 20));
        p.fillRect(QRect(20, 140, 260, 3), QColor(20, 20, 20));
    }
    const QImage banded = img::findStrokes(bands);
    QVERIFY(!on(banded, 150, 47));
    QVERIFY(on(banded, 150, 141));
    img::StrokeOptions wide;
    wide.maxWidth = 16;
    QVERIFY(on(img::findStrokes(bands, wide), 150, 47));
    // A black line over saturated red (little darker in luminance) and through a dark room (darker by
    // less than the contrast, but a third as bright): both found, the red and the room's edges not.
    QImage scene(400, 200, QImage::Format_ARGB32);
    scene.fill(QColor(200, 200, 195));
    {
        QPainter p(&scene);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(130, 0, 5));
        p.drawEllipse(QPoint(120, 100), 60, 60);
        p.fillRect(QRect(260, 0, 140, 200), QColor(50, 44, 36));
        p.setPen(QPen(QColor(15, 15, 15), 3));
        p.drawLine(20, 100, 390, 100);
    }
    const QImage crossing = img::findStrokes(scene);
    QVERIFY(on(crossing, 120, 100)); // over the red
    QVERIFY(on(crossing, 330, 100)); // in the dark
    QVERIFY(on(crossing, 40, 100));
    QVERIFY(coverage(crossing, QRect(70, 140, 100, 30)) < 0.02);  // the red's lower edge
    QVERIFY(coverage(crossing, QRect(250, 130, 20, 60)) < 0.02);  // the room's edge

    const QImage dust = img::findSpecks(drawing);
    // (The one at 120, 200 is 5 px from a line: the paper between them is not a light speck.)
    QVERIFY(on(dust, 200, 100) && on(dust, 120, 200) && on(dust, 330, 110));
    QVERIFY(!on(dust, 200, 50));            // the lines are too long for dust
    QVERIFY(coverage(dust, QRect(260, 170, 80, 80)) < 0.01);

    const QImage red = img::findColorName(drawing, QStringLiteral("red"));
    QVERIFY(coverage(red, QRect(25, 105, 30, 20)) > 0.95);
    QVERIFY(coverage(red, QRect(25, 143, 30, 14)) > 0.95); // dark red is red
    QVERIFY(coverage(red, QRect(85, 105, 30, 20)) < 0.01); // blue is not
    QVERIFY(coverage(red, QRect(300, 10, 60, 20)) < 0.01); // nor the paper
    QVERIFY(coverage(img::findColorName(drawing, QStringLiteral("blau")), QRect(85, 105, 30, 20)) > 0.95);
    QVERIFY(coverage(img::findColorName(drawing, QStringLiteral("white")), QRect(300, 10, 60, 20)) > 0.95);
    QVERIFY(img::findColorName(drawing, QStringLiteral("teacup")).isNull());
    const QImage exact = img::findColor(drawing, QColor(200, 30, 30), 10);
    QVERIFY(coverage(exact, QRect(25, 105, 30, 20)) > 0.95);
    QVERIFY(coverage(exact, QRect(25, 143, 30, 14)) < 0.05); // far from it in Lab

    // A 12-megapixel scan stays quick (O(n) filters).
    QElapsedTimer t;
    t.start();
    img::findStrokes(drawing.scaled(4000, 3000));
    QVERIFY2(t.elapsed() < 8000, qPrintable(QString::number(t.elapsed())));
}

// The words of "Select → By description" read as a finder (English and some German), or as none.
void TestCore::finderWords()
{
    using K = img::FinderRequest::Kind;
    img::FinderRequest r = img::parseFinderRequest(QStringLiteral("the black lines"));
    QCOMPARE(r.kind, K::Strokes);
    QVERIFY(r.strokes.dark);
    QCOMPARE(r.strokes.maxWidth, 6);
    r = img::parseFinderRequest(QStringLiteral("thin white scratches"));
    QCOMPARE(r.kind, K::Strokes);
    QVERIFY(!r.strokes.dark);
    QCOMPARE(r.strokes.maxWidth, 3);
    r = img::parseFinderRequest(QStringLiteral("die dicken schwarzen Linien"));
    QCOMPARE(r.kind, K::Strokes);
    QVERIFY(r.strokes.dark);
    QCOMPARE(r.strokes.maxWidth, 12);
    r = img::parseFinderRequest(QStringLiteral("dust and specks"));
    QCOMPARE(r.kind, K::Specks);
    QCOMPARE(r.specks.tone, img::SpeckOptions::Tone::Both);
    QCOMPARE(img::parseFinderRequest(QStringLiteral("white dust")).specks.tone, img::SpeckOptions::Tone::Light);
    QCOMPARE(img::parseFinderRequest(QStringLiteral("Staub")).kind, K::Specks);
    r = img::parseFinderRequest(QStringLiteral("everything red"));
    QCOMPARE(r.kind, K::Color);
    QCOMPARE(r.color, QStringLiteral("red"));
    QCOMPARE(img::parseFinderRequest(QStringLiteral("die grüne Fläche")).color, QStringLiteral("green"));
    QCOMPARE(img::parseFinderRequest(QStringLiteral("the cup")).kind, K::None);
    QVERIFY(img::runFinder(scannedDrawing(), img::parseFinderRequest(QStringLiteral("the cup"))).isNull());
}

QTEST_MAIN(TestCore)
#include "tst_core.moc"
