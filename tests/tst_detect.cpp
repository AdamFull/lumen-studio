// Finding objects with detection models through ONNX Runtime (plan M18 §18.1): the steps around
// the models without the runtime, and, when LUMEN_TEST_ORT (onnxruntime.dll) and the model folders
// are given, the real models on a drawn scene. Nothing is downloaded here.

#include "ai/Detector.h"
#include "ai/Onnx.h"
#include "ai/Segmenter.h"
#include "core/ImageOps.h"

#include <QElapsedTimer>
#include <QPainter>
#include <QtTest>

using namespace lumen;

namespace {

// A grey floor, a red ball and a blue box.
QImage scene()
{
    QImage im(640, 480, QImage::Format_ARGB32_Premultiplied);
    im.fill(QColor(150, 150, 145));
    QPainter p(&im);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(QRect(0, 0, 640, 200), QColor(200, 215, 230));
    p.setPen(Qt::NoPen);
    QRadialGradient ball(QPointF(190, 280), 90);
    ball.setColorAt(0, QColor(250, 80, 70));
    ball.setColorAt(1, QColor(160, 20, 20));
    p.setBrush(ball);
    p.drawEllipse(QPoint(200, 300), 80, 80);
    p.setBrush(QColor(40, 70, 190));
    p.drawRect(QRect(400, 220, 160, 160));
    return im;
}

double coverage(const QImage &mask, const QRect &r)
{
    int n = 0, in = 0;
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x, ++n)
            in += qGray(mask.pixel(x, y)) > 127;
    return n ? double(in) / n : 0;
}

} // namespace

class TestDetect : public QObject
{
    Q_OBJECT
private slots:
    void sam2Steps();
    void sam2Real();
    void wordPiece();
    void dinoSteps();
    void dinoReal();
};

// The picture in the encoder's form, points in its space, a mask back at the picture's size, the
// best of the candidates.
void TestDetect::sam2Steps()
{
    QImage red(200, 100, QImage::Format_ARGB32);
    red.fill(QColor(255, 0, 0));
    const std::vector<float> px = ai::sam2Pixels(red);
    QCOMPARE(px.size(), size_t(3 * 1024 * 1024));
    QVERIFY(std::abs(px[0] - (1.0f - 0.485f) / 0.229f) < 1e-4);              // R
    QVERIFY(std::abs(px[1024 * 1024] - (0.0f - 0.456f) / 0.224f) < 1e-4);    // G
    QCOMPARE(ai::sam2Point(QPointF(100, 50), QSize(200, 100)), QPointF(512, 512));
    QCOMPARE(ai::sam2Point(QPointF(0, 100), QSize(200, 100)), QPointF(0, 1024));
    // A logit square in the middle of 256 × 256 becomes the middle of the picture.
    std::vector<float> logits(256 * 256, -8.0f);
    for (int y = 64; y < 192; ++y)
        for (int x = 64; x < 192; ++x)
            logits[size_t(y) * 256 + x] = 8.0f;
    const QImage mask = ai::sam2Mask(logits.data(), 256, 256, QSize(400, 200));
    QCOMPARE(mask.size(), QSize(400, 200));
    QCOMPARE(mask.format(), QImage::Format_Grayscale8);
    QCOMPARE(qGray(mask.pixel(200, 100)), 255);
    QCOMPARE(qGray(mask.pixel(20, 20)), 0);
    QVERIFY(std::abs(img::maskCoverage(mask) - 0.25) < 0.02);
    QCOMPARE(ai::bestCandidate({0.2f, 0.9f, 0.5f}, 0, 3), 1);
    QCOMPARE(ai::bestCandidate({0.1f, 0.2f, 0.9f, 0.5f, 0.3f, 0.95f}, 3, 3), 2);
}

// SAM 2.1 tiny on the scene: a click on the ball selects the ball (not the box, not the floor), a
// box around the box selects it, and the second prompt reuses the encoding.
void TestDetect::sam2Real()
{
    const QString dll = qEnvironmentVariable("LUMEN_TEST_ORT");
    const QString dir = qEnvironmentVariable("LUMEN_TEST_SAM2");
    if (dll.isEmpty() || dir.isEmpty())
        QSKIP("LUMEN_TEST_ORT (onnxruntime.dll) and LUMEN_TEST_SAM2 (the SAM 2.1 folder) are needed");
    QString error;
    auto rt = ai::OnnxRuntime::load(dll, &error);
    QVERIFY2(rt, qPrintable(error));
    qInfo().noquote() << "ONNX Runtime" << rt->version();
    QElapsedTimer t;
    t.start();
    auto sam = ai::Sam2Segmenter::open(rt, dir + QStringLiteral("/onnx/vision_encoder_int8.onnx"),
                                       dir + QStringLiteral("/onnx/prompt_encoder_mask_decoder_int8.onnx"), &error);
    QVERIFY2(sam, qPrintable(error));
    qInfo() << "loaded in" << t.restart() << "ms";
    const QImage picture = scene();
    ai::SegmentPrompt click;
    click.points = {QPointF(200, 300)};
    click.positive = {true};
    const QImage ball = sam->segment(picture, click, &error);
    QVERIFY2(!ball.isNull(), qPrintable(error));
    qInfo() << "first prompt (with the encoding) in" << t.restart() << "ms";
    QCOMPARE(ball.size(), picture.size());
    QVERIFY2(coverage(ball, QRect(160, 260, 80, 80)) > 0.95, "the ball's middle");
    QVERIFY2(coverage(ball, QRect(420, 240, 120, 120)) < 0.02, "not the box");
    QVERIFY2(coverage(ball, QRect(10, 420, 100, 50)) < 0.02, "not the floor");
    ai::SegmentPrompt box;
    box.box = QRectF(390, 210, 180, 180);
    const QImage boxed = sam->segment(picture, box, &error);
    QVERIFY2(!boxed.isNull(), qPrintable(error));
    qInfo() << "second prompt in" << t.restart() << "ms";
    QVERIFY2(coverage(boxed, QRect(420, 240, 120, 120)) > 0.95, "the box");
    QVERIFY2(coverage(boxed, QRect(160, 260, 80, 80)) < 0.02, "not the ball");
    QCOMPARE(sam->encodings(), 1);
}

// BERT's uncased WordPiece, as Grounding DINO reads: lower case, accents off, punctuation apart, the
// longest pieces with "##" inside a word, [CLS] … [SEP]. With LUMEN_TEST_GDINO, against the real
// vocabulary's ids.
void TestDetect::wordPiece()
{
    ai::WordPiece w;
    w.setVocabulary({QStringLiteral("[PAD]"), QStringLiteral("[UNK]"), QStringLiteral("[CLS]"), QStringLiteral("[SEP]"), QStringLiteral("."),
                     QStringLiteral("the"), QStringLiteral("cup"), QStringLiteral("tea"), QStringLiteral("##cup"), QStringLiteral("cafe")});
    QCOMPARE(w.encode(QStringLiteral("The cup.")), (std::vector<int64_t>{2, 5, 6, 4, 3}));
    QCOMPARE(w.encode(QStringLiteral("teacup")), (std::vector<int64_t>{2, 7, 8, 3}));     // tea ##cup
    QCOMPARE(w.encode(QStringLiteral("Café")), (std::vector<int64_t>{2, 9, 3}));          // accents off
    QCOMPARE(w.encode(QStringLiteral("xyz")), (std::vector<int64_t>{2, 1, 3}));           // unknown
    QCOMPARE(ai::dinoText(QStringLiteral("  The Red  Cup. ")), QStringLiteral("the red cup ."));
    const QString dir = qEnvironmentVariable("LUMEN_TEST_GDINO");
    if (!dir.isEmpty()) {
        ai::WordPiece bert;
        QVERIFY(bert.load(dir + QStringLiteral("/vocab.txt")));
        // As the Hugging Face tokenizer gives them.
        QCOMPARE(bert.encode(QStringLiteral("a cup .")), (std::vector<int64_t>{101, 1037, 2452, 1012, 102}));
        QCOMPARE(bert.encode(QStringLiteral("the black lines .")), (std::vector<int64_t>{101, 1996, 2304, 3210, 1012, 102}));
    }
}

// The picture in the detector's form, and boxes back from its answer: a word's best score passes the
// threshold, the box is scaled to the picture, an overlapping weaker box is dropped.
void TestDetect::dinoSteps()
{
    QImage red(100, 50, QImage::Format_ARGB32);
    red.fill(QColor(255, 0, 0));
    const std::vector<float> px = ai::dinoPixels(red);
    QCOMPARE(px.size(), size_t(3 * 800 * 800));
    QVERIFY(std::abs(px[0] - (1.0f - 0.485f) / 0.229f) < 1e-4);
    // Three candidates over a text of 4 tokens ([CLS] cup . [SEP]); the word is at 1.
    const int text = 4;
    std::vector<float> logits(3 * text, -6.0f);
    logits[0 * text + 1] = 2.0f;  // sure: a box on the left
    logits[1 * text + 1] = 0.5f;  // fairly sure, but overlapping the first
    logits[2 * text + 2] = 5.0f;  // sure of the ".", not of the word: not kept
    const std::vector<float> boxes{0.25f, 0.5f, 0.4f, 0.8f, 0.27f, 0.5f, 0.4f, 0.8f, 0.75f, 0.5f, 0.2f, 0.2f};
    const QList<ai::Detection> found = ai::dinoDecode(logits, boxes, 3, text, {1}, QSize(200, 100), 0.3f);
    QCOMPARE(found.size(), 1);
    const QRectF box = found.first().box;
    QVERIFY(std::abs(box.x() - 10) < 1e-3 && std::abs(box.y() - 10) < 1e-3 && std::abs(box.width() - 80) < 1e-3 && std::abs(box.height() - 80) < 1e-3);
    QVERIFY(found.first().score > 0.85f);
}

// Grounding DINO tiny and SAM 2.1 tiny on the drawn scene: "ball" finds the ball and "box" the box;
// the objects' area is the ball's, not the box's.
void TestDetect::dinoReal()
{
    const QString dll = qEnvironmentVariable("LUMEN_TEST_ORT");
    const QString dino = qEnvironmentVariable("LUMEN_TEST_GDINO");
    const QString sam = qEnvironmentVariable("LUMEN_TEST_SAM2");
    if (dll.isEmpty() || dino.isEmpty() || sam.isEmpty())
        QSKIP("LUMEN_TEST_ORT, LUMEN_TEST_GDINO and LUMEN_TEST_SAM2 are needed");
    QString error;
    auto rt = ai::OnnxRuntime::load(dll, &error);
    QVERIFY2(rt, qPrintable(error));
    QElapsedTimer t;
    t.start();
    auto detector = ai::GroundingDino::open(rt, dino + QStringLiteral("/onnx/model_int8.onnx"), dino + QStringLiteral("/vocab.txt"), &error);
    QVERIFY2(detector, qPrintable(error));
    qInfo() << "detector loaded in" << t.restart() << "ms";
    const QImage picture = scene();
    const QList<ai::Detection> balls = detector->detect(picture, QStringLiteral("a red ball"), 0.3f, &error);
    qInfo() << "detected in" << t.restart() << "ms:" << balls.size();
    QVERIFY2(!balls.isEmpty(), qPrintable(error));
    for (const ai::Detection &d : balls)
        qInfo() << "  ball" << d.box << d.score;
    QVERIFY(balls.first().box.contains(QPointF(200, 300)));
    QVERIFY(!balls.first().box.contains(QPointF(480, 300)));
    const QList<ai::Detection> boxes = detector->detect(picture, QStringLiteral("blue box"), 0.3f, &error);
    QVERIFY2(!boxes.isEmpty(), qPrintable(error));
    QVERIFY(boxes.first().box.contains(QPointF(480, 300)));
    auto segmenter = ai::Sam2Segmenter::open(rt, sam + QStringLiteral("/onnx/vision_encoder_int8.onnx"),
                                             sam + QStringLiteral("/onnx/prompt_encoder_mask_decoder_int8.onnx"), &error);
    QVERIFY2(segmenter, qPrintable(error));
    int count = 0;
    t.restart();
    const QImage area = ai::findObjects(*detector, *segmenter, picture, QStringLiteral("the ball"), &count, &error);
    qInfo() << "found and cut out in" << t.elapsed() << "ms:" << count;
    QVERIFY2(!area.isNull(), qPrintable(error));
    QVERIFY(coverage(area, QRect(160, 260, 80, 80)) > 0.95);
    QVERIFY(coverage(area, QRect(420, 240, 120, 120)) < 0.02);
}

QTEST_MAIN(TestDetect)
#include "tst_detect.moc"
