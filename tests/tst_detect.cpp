// Finding objects with detection models through ONNX Runtime (plan M18 §18.1): the steps around
// the models without the runtime, and, when LUMEN_TEST_ORT (onnxruntime.dll) and the model folders
// are given, the real models on a drawn scene. Nothing is downloaded here.

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

QTEST_MAIN(TestDetect)
#include "tst_detect.moc"
