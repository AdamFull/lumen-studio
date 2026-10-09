#include "MockHttpServer.h"

#include "ai/PartPipeline.h"
#include "ai/PartService.h"
#include "ai/ProviderCatalog.h"
#include "ai/providers/GeminiProvider.h"
#include "ai/providers/PartServerProvider.h"
#include "core/ImageOps.h"
#include "core/JobQueue.h"
#include "core/MaskOps.h"
#include "core/Psd.h"
#include "net/HttpClient.h"

#include <QJsonArray>
#include <QPainter>
#include <QTemporaryDir>
#include <QtTest>

using namespace lumen;
using namespace lumen::ai;
using doc::PartSet;

namespace {

QImage rectMask(const QSize &size, const QRect &r)
{
    QImage m = img::createMask(size, 0);
    for (int y = r.top(); y <= r.bottom(); ++y)
        std::fill(m.scanLine(y) + r.left(), m.scanLine(y) + r.right() + 1, uchar(255));
    return m;
}

QImage rgbaLayer(const QSize &size, const QRect &r, const QColor &c)
{
    QImage im(size, QImage::Format_ARGB32_Premultiplied);
    im.fill(Qt::transparent);
    QPainter p(&im);
    p.fillRect(r, c);
    p.end();
    return im;
}

// A simple front-facing figure on a transparent canvas.
QImage figure()
{
    QImage im(240, 400, QImage::Format_ARGB32_Premultiplied);
    im.fill(Qt::transparent);
    QPainter p(&im);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(250, 220, 200));
    p.drawEllipse(QRect(90, 20, 60, 70));            // head
    p.setBrush(QColor(60, 90, 170));
    p.drawRect(QRect(85, 90, 70, 130));              // torso
    p.drawRect(QRect(40, 95, 45, 18));               // arm R (image left)
    p.drawRect(QRect(155, 95, 45, 18));              // arm L
    p.setBrush(QColor(40, 40, 40));
    p.drawRect(QRect(92, 220, 22, 160));             // leg R
    p.drawRect(QRect(126, 220, 22, 160));            // leg L
    p.end();
    return im;
}

QString b64(const QImage &im)
{
    return QString::fromLatin1(img::toBase64(im));
}

int labelAt(const PartSet &set, int x, int y)
{
    return set.labels().constScanLine(y)[x];
}

} // namespace

class TestSlicing : public QObject
{
    Q_OBJECT
private slots:
    void init() { QStandardPaths::setTestModeEnabled(true); }
    void mergeStackedLayers();
    void mergeMasksUsesDrawOrder();
    void mergeScopes();
    void skeletonSplit();
    void keyColorAndExtraction();
    void partServerResponse();
    void geminiSegmentation();
    void endToEnd();

private:
    net::HttpClient m_http;
};

void TestSlicing::mergeStackedLayers()
{
    PartSet set;
    set.setSource(figure(), QStringLiteral("src"));
    const QSize size = set.size();
    QList<parts::SegmentLayer> layers;
    layers.append({QStringLiteral("topwear"), {}, QStringLiteral("layer"), rgbaLayer(size, QRect(40, 90, 160, 130), Qt::blue), 0, -1});
    layers.append({QStringLiteral("face"), {}, QStringLiteral("layer"), rgbaLayer(size, QRect(90, 20, 60, 80), Qt::red), 1, -1});
    layers.append({QStringLiteral("cape thing"), {}, QStringLiteral("layer"), rgbaLayer(size, QRect(0, 380, 240, 20), Qt::green), 2, -1});
    parts::SegmentOptions opt;
    opt.clipToSilhouette = false;
    const parts::SegmentOutcome out = parts::mergeSegmentation(set.snapshot(), QImage(), set.taxonomy(), layers, opt);
    QCOMPARE(out.parts.size(), 3);
    QCOMPARE(out.unmatched, QStringList{QStringLiteral("cape thing")});
    set.replaceParts(out.labels, out.parts, QStringLiteral("seg"));
    const int face = labelAt(set, 120, 95);
    QVERIFY(face != 0);
    QCOMPARE(set.part(face)->tag, QStringLiteral("face")); // the face layer is above the topwear
    QCOMPARE(set.part(labelAt(set, 50, 100))->tag, QStringLiteral("topwear"));
    QCOMPARE(set.part(labelAt(set, 10, 390))->tag, QStringLiteral("cape thing"));
    QCOMPARE(set.part(face)->state, doc::PartState::Masked);
}

void TestSlicing::mergeMasksUsesDrawOrder()
{
    PartSet set;
    set.setSource(figure(), QStringLiteral("src"));
    const QSize size = set.size();
    // Overlapping masks: front hair must win over the face (it is drawn in front).
    QList<parts::SegmentLayer> layers;
    layers.append({QStringLiteral("bangs"), {}, QStringLiteral("mask"), rectMask(size, QRect(90, 20, 60, 30)), -1, -1});
    layers.append({QStringLiteral("face"), {}, QStringLiteral("mask"), rectMask(size, QRect(90, 20, 60, 70)), -1, -1});
    parts::SegmentOptions opt;
    opt.clipToSilhouette = false;
    const parts::SegmentOutcome out = parts::mergeSegmentation(set.snapshot(), QImage(), set.taxonomy(), layers, opt);
    set.replaceParts(out.labels, out.parts, QStringLiteral("seg"));
    QCOMPARE(set.part(labelAt(set, 120, 30))->tag, QStringLiteral("front hair"));
    QCOMPARE(set.part(labelAt(set, 120, 70))->tag, QStringLiteral("face"));
}

void TestSlicing::mergeScopes()
{
    PartSet set;
    set.setSource(figure(), QStringLiteral("src"));
    const QSize size = set.size();
    const int face = set.addPart(QStringLiteral("face"), {}, {}, rectMask(size, QRect(90, 20, 60, 70)));
    const int body = set.addPart(QStringLiteral("topwear"), {}, {}, rectMask(size, QRect(85, 90, 70, 130)));
    doc::Part locked = *set.part(body);
    locked.locked = true;
    set.updatePart(locked);

    // Selected kinds: only the face is replaced; the locked body keeps its pixels.
    QList<parts::SegmentLayer> layers{{QStringLiteral("face"), {}, QStringLiteral("mask"), rectMask(size, QRect(80, 10, 80, 120)), -1, -1}};
    parts::SegmentOptions sel;
    sel.scope = parts::SegmentScope::SelectedKinds;
    sel.kinds = {QStringLiteral("face"), QStringLiteral("mouth")};
    sel.clipToSilhouette = false;
    parts::SegmentOutcome out = parts::mergeSegmentation(set.snapshot(), QImage(), set.taxonomy(), layers, sel);
    QCOMPARE(out.empty, QStringList{QStringLiteral("mouth")});
    set.replaceParts(out.labels, out.parts, QStringLiteral("seg"));
    QCOMPARE(labelAt(set, 85, 15), face);        // grown
    QCOMPARE(labelAt(set, 120, 100), body);      // locked pixels untouched (overlap 90..129)
    QCOMPARE(set.parts().size(), 2);

    // Fill unassigned: a new part only claims free pixels.
    QList<parts::SegmentLayer> legs{{QStringLiteral("legs"), {}, QStringLiteral("mask"), rectMask(size, QRect(80, 200, 80, 190)), -1, -1}};
    parts::SegmentOptions fill;
    fill.scope = parts::SegmentScope::FillUnassigned;
    fill.clipToSilhouette = false;
    out = parts::mergeSegmentation(set.snapshot(), QImage(), set.taxonomy(), legs, fill);
    set.replaceParts(out.labels, out.parts, QStringLiteral("seg"));
    QCOMPARE(labelAt(set, 120, 210), body);
    QCOMPARE(set.part(labelAt(set, 120, 300))->tag, QStringLiteral("legwear"));

    // Replace all keeps locked parts only.
    QList<parts::SegmentLayer> all{{QStringLiteral("face"), {}, QStringLiteral("mask"), rectMask(size, QRect(0, 0, 240, 400)), -1, -1}};
    parts::SegmentOptions replace;
    replace.clipToSilhouette = false;
    out = parts::mergeSegmentation(set.snapshot(), QImage(), set.taxonomy(), all, replace);
    set.replaceParts(out.labels, out.parts, QStringLiteral("seg"));
    QCOMPARE(set.parts().size(), 2);
    QCOMPARE(labelAt(set, 120, 100), body);
    QCOMPARE(set.part(labelAt(set, 5, 5))->tag, QStringLiteral("face"));
}

void TestSlicing::skeletonSplit()
{
    const QImage source = figure();
    const QImage sil = img::silhouetteMask(source);
    const pose::Person person = parts::fitSkeleton(sil);
    QVERIFY(person.body[pose::Neck].visible);
    const QRectF bounds = person.bounds();
    QVERIFY(QRectF(img::maskBounds(sil)).adjusted(-30, -30, 30, 30).contains(bounds));

    const QList<parts::BoneGroup> groups = parts::skeletonGroups(person, QStringLiteral("live2d"));
    QStringList names;
    for (const parts::BoneGroup &g : groups)
        names << g.tag + g.side;
    QVERIFY(names.contains(QStringLiteral("head")));
    QVERIFY(names.contains(QStringLiteral("upper armL")));
    QVERIFY(names.contains(QStringLiteral("upper armR")));
    QVERIFY(names.contains(QStringLiteral("shinL")));
    const QList<QImage> masks = parts::splitBySkeleton(sil, groups);
    double total = 0;
    for (const QImage &m : masks)
        total += img::maskCoverage(m);
    QVERIFY(std::abs(total - img::maskCoverage(sil)) < 1e-9);

    // Simple taxonomy: arms and legs per side.
    const QList<parts::BoneGroup> simple = parts::skeletonGroups(person, QStringLiteral("simple"));
    QCOMPARE(simple.size(), 6); // head, body, arms L/R, legs L/R
    const QList<parts::BoneGroup> limbs = parts::limbGroups(person, QStringLiteral("live2d"), true, false);
    QCOMPARE(limbs.size(), 6);  // upper arm, forearm, hand per side
}

void TestSlicing::keyColorAndExtraction()
{
    const QSize size(100, 100);
    QImage source(size, QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::white); // a white part: the key must not be white
    const QImage visible = rectMask(size, QRect(10, 10, 40, 80));
    const QImage extension = rectMask(size, QRect(10, 10, 80, 80));
    const parts::CompletionContext ctx = parts::buildCompletionContext(source, visible, extension);
    QVERIFY(ctx.key != QColor(Qt::white));
    QCOMPARE(ctx.image.pixelColor(70, 50), ctx.key);
    QCOMPARE(ctx.image.pixelColor(20, 50), QColor(Qt::white));
    QVERIFY(qGray(ctx.mask.pixel(70, 50)) == 255);
    QVERIFY(qGray(ctx.mask.pixel(20, 50)) == 0);

    // Generated: key everywhere, a red block continuing the part inside the extension.
    QImage generated = ctx.image;
    QPainter p(&generated);
    p.fillRect(QRect(50, 10, 20, 80), Qt::red);
    p.fillRect(QRect(95, 0, 5, 100), Qt::red); // outside the extension: ignored
    p.end();
    const QImage part = parts::extractCompletedPart(generated, source, visible, extension, ctx.key);
    QCOMPARE(part.pixelColor(20, 50), QColor(Qt::white));
    QCOMPARE(part.pixelColor(60, 50), QColor(Qt::red));
    QCOMPARE(part.pixelColor(80, 50).alpha(), 0); // key removed
    QCOMPARE(part.pixelColor(97, 50).alpha(), 0);

    const QString prompt = parts::completionPrompt(doc::Part{}, doc::partTaxonomy(QStringLiteral("live2d")).find(QStringLiteral("back hair")),
                                                   QStringLiteral("silver hair"), QColor(0, 255, 0));
    QVERIFY(prompt.contains(QStringLiteral("green background")));
    QVERIFY(prompt.contains(QStringLiteral("silver hair")));
}

void TestSlicing::partServerResponse()
{
    QImage crop(10, 5, QImage::Format_ARGB32);
    crop.fill(Qt::red);
    QImage mask(4, 4, QImage::Format_Grayscale8);
    mask.fill(255);
    const QJsonObject body{{QStringLiteral("width"), 50},
                           {QStringLiteral("height"), 40},
                           {QStringLiteral("layers"),
                            QJsonArray{QJsonObject{{QStringLiteral("tag"), QStringLiteral("face")}, {QStringLiteral("image"), b64(crop)},
                                                   {QStringLiteral("x"), 20}, {QStringLiteral("y"), 10}, {QStringLiteral("depth"), 0.6}},
                                       QJsonObject{{QStringLiteral("tag"), QStringLiteral("mouth")}, {QStringLiteral("kind"), QStringLiteral("mask")},
                                                   {QStringLiteral("image"), b64(mask)}, {QStringLiteral("x"), 1}, {QStringLiteral("y"), 2}}}}};
    const ImageResult r = PartServerProvider::parseSegmentResponse(QJsonDocument(body).toJson(), QSize(100, 80));
    QCOMPARE(r.images.size(), 2);
    QCOMPARE(r.images[0].image.size(), QSize(100, 80)); // scaled to the requested canvas
    QCOMPARE(r.images[0].image.pixelColor(50, 25), QColor(Qt::red));
    QCOMPARE(r.images[0].image.pixelColor(5, 5).alpha(), 0);
    QCOMPARE(r.images[0].info.value(QStringLiteral("kind")).toString(), QStringLiteral("layer"));
    QCOMPARE(r.images[0].info.value(QStringLiteral("depth")).toDouble(), 0.6);
    QCOMPARE(r.images[1].image.format(), QImage::Format_Grayscale8);
    QCOMPARE(qGray(r.images[1].image.pixel(5, 7)), 255);
    QCOMPARE(qGray(r.images[1].image.pixel(50, 50)), 0);
    QVERIFY_THROWS_EXCEPTION(ProviderError, PartServerProvider::parseSegmentResponse(R"({"error":"CUDA out of memory"})", QSize(10, 10)));
}

void TestSlicing::geminiSegmentation()
{
    QImage m(8, 8, QImage::Format_Grayscale8);
    m.fill(0);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 4; ++x)
            m.scanLine(y)[x] = 255;
    const QString list = QStringLiteral("```json\n[{\"box_2d\": [100, 200, 600, 800], \"mask\": \"data:image/png;base64,%1\", \"label\": \"face (face)\"}]\n```")
                             .arg(b64(m));
    const QJsonObject body{{QStringLiteral("candidates"),
                            QJsonArray{QJsonObject{{QStringLiteral("content"),
                                                    QJsonObject{{QStringLiteral("parts"), QJsonArray{QJsonObject{{QStringLiteral("text"), list}}}}}}}}}};
    const ImageResult r = GeminiImageProvider::parseSegmentation(QJsonDocument(body).toJson(), QSize(1000, 1000));
    QCOMPARE(r.images.size(), 1);
    QCOMPARE(r.images[0].info.value(QStringLiteral("label")).toString(), QStringLiteral("face"));
    const QImage mask = r.images[0].image;
    QCOMPARE(qGray(mask.pixel(250, 300)), 255); // left half of the box
    QCOMPARE(qGray(mask.pixel(700, 300)), 0);   // right half of the box
    QCOMPARE(qGray(mask.pixel(100, 50)), 0);    // outside the box
    const QJsonObject req = GeminiImageProvider::buildSegmentationBody(QImage(64, 64, QImage::Format_RGB32),
                                                                      QJsonArray{QJsonObject{{QStringLiteral("tag"), QStringLiteral("front hair")},
                                                                                             {QStringLiteral("prompt"), QStringLiteral("bangs")}}});
    QVERIFY(QJsonDocument(req).toJson().contains("front hair (bangs)"));
}

void TestSlicing::endToEnd()
{
    MockHttpServer server;
    QJsonObject segmentRequest;
    server.route("GET", QStringLiteral("/v1/info"), [](const MockRequest &) {
        return MockResponse::json({{QStringLiteral("name"), QStringLiteral("mock see-through")}, {QStringLiteral("tags"), QJsonArray{QStringLiteral("face")}}});
    });
    const QImage source = figure();
    server.route("POST", QStringLiteral("/v1/segment"), [&](const MockRequest &req) {
        segmentRequest = req.json();
        // Model layers: complete torso (also behind the arms) and head.
        QImage torso = rgbaLayer(source.size(), QRect(40, 90, 160, 135), QColor(60, 90, 170));
        QImage head = rgbaLayer(source.size(), QRect(88, 18, 64, 74), QColor(250, 220, 200));
        return MockResponse::json({{QStringLiteral("layers"),
                                    QJsonArray{QJsonObject{{QStringLiteral("tag"), QStringLiteral("topwear")}, {QStringLiteral("image"), b64(torso)}, {QStringLiteral("order"), 0}},
                                               QJsonObject{{QStringLiteral("tag"), QStringLiteral("face")}, {QStringLiteral("image"), b64(head)}, {QStringLiteral("order"), 1}}}}});
    });
    server.route("POST", QStringLiteral("/sdapi/v1/img2img"), [&](const MockRequest &req) {
        const QJsonObject payload = req.json();
        QImage out(payload.value(QStringLiteral("width")).toInt(), payload.value(QStringLiteral("height")).toInt(), QImage::Format_ARGB32);
        out.fill(QColor(60, 90, 170));
        return MockResponse::json({{QStringLiteral("images"), QJsonArray{b64(out)}}, {QStringLiteral("info"), QStringLiteral("{}")}});
    });
    server.route("GET", QStringLiteral("/sdapi/v1/progress"), [](const MockRequest &) { return MockResponse::json({}); });

    QTemporaryDir dir;
    doc::Project project;
    auto opened = project.open(dir.filePath(QStringLiteral("p.lumen")));
    QTRY_VERIFY(opened.isFinished());
    ProviderRegistry registry(&m_http);
    registerBuiltInProviders(registry);
    QFile::remove(registry.configPath());
    Provider *ps = registry.addProvider(QStringLiteral("partserver"), QStringLiteral("Parts"), {{QStringLiteral("url"), server.baseUrl().toString()}});
    Provider *sd = registry.addProvider(QStringLiteral("sdwebui"), QStringLiteral("SD"), {{QStringLiteral("url"), server.baseUrl().toString()}});
    QVERIFY(ps && sd);
    JobQueue queue;
    GenerationService generation(&registry, &queue);
    generation.setProject(&project);
    PartService service(&registry, &queue, &generation);
    QCOMPARE(service.segmenters().size(), 1);

    auto srcAsset = project.addAsset(source, doc::AssetKind::Reference, QStringLiteral("figure"));
    QTRY_VERIFY(srcAsset.isFinished());
    PartSet set;
    set.bind(&project);
    set.setSource(source, srcAsset.result().id, img::silhouetteMask(source));

    // 1. Segment.
    Job *seg = service.segment(&set, ps->instanceId(), parts::SegmentOptions{});
    QTRY_VERIFY_WITH_TIMEOUT(seg->isFinished(), 10000);
    QVERIFY2(seg->state() == Job::State::Succeeded, qPrintable(seg->errorString()));
    QVERIFY(segmentRequest.value(QStringLiteral("parts")).toArray().size() == 23);
    QCOMPARE(set.parts().size(), 2);
    const int face = labelAt(set, 120, 50);
    const int torso = labelAt(set, 120, 150);
    QCOMPARE(set.part(face)->tag, QStringLiteral("face"));
    QCOMPARE(set.part(torso)->tag, QStringLiteral("topwear"));
    QCOMPARE(labelAt(set, 20, 20), 0); // clipped to the silhouette
    QVERIFY(set.decomposition().contains(QStringLiteral("topwear")));
    QCOMPARE(set.undoStack()->count(), 1);

    // 2. Complete: decomposition for the torso, cut-only for the face.
    CompletionOptions decomp;
    decomp.strategy = CompletionStrategy::Decomposition;
    QList<Job *> jobs = service.complete(&set, {torso}, decomp);
    CompletionOptions cut;
    cut.strategy = CompletionStrategy::CutOnly;
    jobs += service.complete(&set, {face}, cut);
    QCOMPARE(jobs.size(), 2);
    for (Job *j : jobs) {
        QTRY_VERIFY_WITH_TIMEOUT(j->isFinished(), 10000);
        QVERIFY2(j->state() == Job::State::Succeeded, qPrintable(j->errorString()));
    }
    QCOMPARE(set.part(torso)->state, doc::PartState::Ready);
    // The decomposition layer continues behind the arms (pixels the face/arms cover are included).
    const QImage torsoResult = set.resultImage(torso);
    QVERIFY(!torsoResult.isNull());
    QCOMPARE(set.part(torso)->chosenRect().topLeft(), QPoint(40, 90));
    const QImage faceResult = set.resultImage(face);
    QVERIFY(!faceResult.isNull());

    // 3. Inpaint completion through SD WebUI.
    CompletionOptions inpaint;
    inpaint.providerId = sd->instanceId();
    inpaint.variants = 1;
    jobs = service.complete(&set, {torso}, inpaint);
    QTRY_VERIFY_WITH_TIMEOUT(jobs.first()->isFinished(), 10000);
    QVERIFY2(jobs.first()->state() == Job::State::Succeeded, qPrintable(jobs.first()->errorString()));
    QCOMPARE(set.part(torso)->variants.size(), 2);
    QCOMPARE(set.part(torso)->chosen, 1);

    // 4. Export PSD and PNGs.
    const QString psdPath = dir.filePath(QStringLiteral("parts.psd"));
    Job *exp = service.exportPsd(&set, psdPath, ExportOptions{});
    QTRY_VERIFY_WITH_TIMEOUT(exp->isFinished(), 10000);
    QVERIFY2(exp->state() == Job::State::Succeeded, qPrintable(exp->errorString()));
    const auto psdFile = psd::read(psdPath);
    QVERIFY(psdFile.has_value());
    QCOMPARE(psdFile->layers.size(), 3);
    QCOMPARE(psdFile->layers[0].name, QStringLiteral("Face"));             // front-most first
    QCOMPARE(psdFile->layers[1].name, QStringLiteral("Topwear / torso"));
    QCOMPARE(psdFile->layers[2].name, QStringLiteral("Original"));
    QVERIFY(!psdFile->layers[2].visible);

    const QString pngDir = dir.filePath(QStringLiteral("pngs"));
    exp = service.exportPngFolder(&set, pngDir, ExportOptions{});
    QTRY_VERIFY_WITH_TIMEOUT(exp->isFinished(), 10000);
    QVERIFY2(exp->state() == Job::State::Succeeded, qPrintable(exp->errorString()));
    QVERIFY(QFile::exists(pngDir + QStringLiteral("/parts.json")));
    QVERIFY(QFile::exists(pngDir + QStringLiteral("/00_Topwear _ torso.png")));
    QVERIFY(QFile::exists(pngDir + QStringLiteral("/preview.png")));

    // 5. Persisted record restores the session.
    auto saved = set.saveNow();
    QTRY_VERIFY_WITH_TIMEOUT(saved.isFinished(), 5000);
    auto records = project.records(QString::fromLatin1(PartSet::kRecordType));
    QTRY_VERIFY(records.isFinished());
    QCOMPARE(records.result().size(), 1);
    const PartSet::Snapshot snap = PartSet::decode(records.result().first().json);
    QCOMPARE(snap.parts.size(), 2);
    QCOMPARE(snap.labels, set.labels());
    QFile::remove(registry.configPath());
}

QTEST_MAIN(TestSlicing)
#include "tst_slicing.moc"
