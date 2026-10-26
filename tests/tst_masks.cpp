#include "core/ImageOps.h"
#include "core/MaskOps.h"
#include "core/Psd.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QPainter>
#include <QtTest>

using namespace lumen;

namespace {

QImage rectMask(const QSize &size, const QList<QRect> &rects)
{
    QImage m = img::createMask(size, 0);
    for (const QRect &r : rects) {
        for (int y = r.top(); y <= r.bottom(); ++y)
            std::fill(m.scanLine(y) + r.left(), m.scanLine(y) + r.right() + 1, uchar(255));
    }
    return m;
}

qint64 area(const QImage &m)
{
    qint64 n = 0;
    for (int y = 0; y < m.height(); ++y)
        for (int x = 0; x < m.width(); ++x)
            n += m.constScanLine(y)[x] >= 128;
    return n;
}

} // namespace

class TestMasks : public QObject
{
    Q_OBJECT
private slots:
    void components();
    void islandsAndHoles();
    void verticalSplit();
    void cutLineSplitsLimb();
    void cutLineThatMissesKeepsMask();
    void nearestSegment();
    void colorRegionContiguous();
    void labelMaps();
    void visibleMasksFromLayers();
    void extensionMask();
    void silhouetteFromBackground();
    void packBitsRoundTrip();
    void psdRoundTrip();
    void psdComponentsPerformance();
    void edgeColorsRemoveDrift();
    void restoreDetail();
};

void TestMasks::components()
{
    const QImage m = rectMask({100, 50}, {QRect(0, 0, 10, 10), QRect(50, 10, 30, 30), QRect(11, 0, 2, 2)});
    const img::Components c = img::connectedComponents(m);
    QCOMPARE(c.count, 3);
    const QList<QImage> pieces = img::splitComponents(m, 10);
    QCOMPARE(pieces.size(), 2);
    QCOMPARE(area(pieces[0]), 900);
    QCOMPARE(area(pieces[1]), 100);
    // Diagonal neighbours join only with 8-connectivity.
    QImage d = img::createMask({4, 4}, 0);
    d.scanLine(0)[0] = 255;
    d.scanLine(1)[1] = 255;
    QCOMPARE(img::connectedComponents(d, 128, true).count, 1);
    QCOMPARE(img::connectedComponents(d, 128, false).count, 2);
}

void TestMasks::islandsAndHoles()
{
    QImage m = rectMask({60, 60}, {QRect(10, 10, 30, 30), QRect(50, 50, 3, 3)});
    for (int y = 20; y < 25; ++y)
        std::fill(m.scanLine(y) + 20, m.scanLine(y) + 25, uchar(0)); // 25 px hole
    const QImage cleaned = img::removeSmallIslands(m, 20);
    QCOMPARE(area(cleaned), 900 - 25);
    QCOMPARE(area(img::fillHoles(cleaned, 10)), 900 - 25); // hole larger than the limit stays
    QCOMPARE(area(img::fillHoles(cleaned)), 900);
}

void TestMasks::verticalSplit()
{
    const QImage m = rectMask({40, 10}, {QRect(0, 0, 40, 10)});
    const auto [l, r] = img::splitByVerticalLine(m, 15);
    QCOMPARE(area(l), 150);
    QCOMPARE(area(r), 250);
}

void TestMasks::cutLineSplitsLimb()
{
    // A horizontal "arm" cut by a short vertical stroke that does not reach the edges.
    const QImage m = rectMask({200, 100}, {QRect(10, 40, 180, 20)});
    const QList<QImage> pieces = img::splitByCutLine(m, QPolygonF{QPointF(100, 45), QPointF(100, 55)});
    QCOMPARE(pieces.size(), 2);
    QCOMPARE(area(pieces[0]) + area(pieces[1]), 3600); // barrier pixels are reassigned
    QVERIFY(std::abs(area(pieces[0]) - area(pieces[1])) <= 60);
}

void TestMasks::cutLineThatMissesKeepsMask()
{
    const QImage m = rectMask({200, 100}, {QRect(10, 40, 50, 20)});
    const QList<QImage> pieces = img::splitByCutLine(m, QPolygonF{QPointF(150, 0), QPointF(150, 10)});
    QCOMPARE(pieces.size(), 1);
    QCOMPARE(area(pieces[0]), 1000);
}

void TestMasks::nearestSegment()
{
    const QImage m = rectMask({100, 100}, {QRect(0, 0, 100, 100)});
    const QList<QImage> parts = img::splitByNearestSegment(m, {QLineF(10, 10, 10, 90), QLineF(90, 10, 90, 90)});
    QCOMPARE(parts.size(), 2);
    QCOMPARE(area(parts[0]), 5000);
    QCOMPARE(area(parts[1]), 5000);
    const QList<QImage> grouped = img::splitByNearestSegmentGroups(m, {QLineF(0, 0, 0, 1), QLineF(99, 0, 99, 1), QLineF(50, 99, 51, 99)},
                                                                   {0, 0, 1}, 2);
    QCOMPARE(area(grouped[0]) + area(grouped[1]), 10000);
}

void TestMasks::colorRegionContiguous()
{
    QImage im(50, 20, QImage::Format_ARGB32);
    im.fill(Qt::white);
    QPainter p(&im);
    p.fillRect(QRect(0, 0, 10, 20), Qt::red);
    p.fillRect(QRect(40, 0, 10, 20), Qt::red);
    p.end();
    QCOMPARE(area(img::colorRegion(im, {2, 2}, 10, true)), 200);
    QCOMPARE(area(img::colorRegion(im, {2, 2}, 10, false)), 400);
    const QImage limit = rectMask(im.size(), {QRect(0, 0, 5, 20)});
    QCOMPARE(area(img::colorRegion(im, {2, 2}, 10, true, limit)), 100);
}

void TestMasks::labelMaps()
{
    const QSize size(30, 30);
    const QImage a = rectMask(size, {QRect(0, 0, 20, 20)});
    const QImage b = rectMask(size, {QRect(10, 10, 20, 20)});
    QImage labels = img::labelsFromMasks(size, {a, b}, {3, 7});
    const QVector<qint64> h = img::labelHistogram(labels);
    QCOMPARE(h[7], 400);
    QCOMPARE(h[3], 300); // b wins in the overlap
    QCOMPARE(img::labelBounds(labels, 3), QRect(0, 0, 20, 20));
    QCOMPARE(area(img::maskForLabel(labels, 7)), 400);
    QCOMPARE(img::maskForLabel(labels, 7, QRect(25, 25, 10, 10)).size(), QSize(5, 5));
    img::replaceLabel(labels, 3, 9);
    QCOMPARE(img::labelHistogram(labels)[9], 300);
    QVector<QRgb> palette(256, 0);
    palette[9] = qRgba(255, 0, 0, 255);
    const QImage col = img::colorizeLabels(labels, palette, QRect(0, 0, 5, 5));
    QCOMPARE(col.pixel(0, 0), qRgba(255, 0, 0, 255));
    const QImage edges = img::labelEdges(labels, labels.rect(), 0xffffffff);
    QVERIFY(qAlpha(edges.pixel(9, 15)) == 255 || qAlpha(edges.pixel(10, 15)) == 255);
    QCOMPARE(qAlpha(edges.pixel(2, 2)), 0);
}

void TestMasks::visibleMasksFromLayers()
{
    QImage bottom(20, 20, QImage::Format_ARGB32);
    bottom.fill(Qt::blue);
    QImage top(20, 20, QImage::Format_ARGB32);
    top.fill(Qt::transparent);
    QPainter p(&top);
    p.fillRect(QRect(0, 0, 10, 20), Qt::red);
    p.end();
    const QList<QImage> vis = img::visibleMasksFromLayers({bottom, top});
    QCOMPARE(area(vis[0]), 200);
    QCOMPARE(area(vis[1]), 200);
}

void TestMasks::extensionMask()
{
    const QImage own = rectMask({100, 100}, {QRect(0, 0, 40, 100)});
    const QImage occ = rectMask({100, 100}, {QRect(40, 0, 20, 50)});
    const QImage ext = img::extensionMask(own, occ, 10);
    QCOMPARE(area(ext), 4000 + 10 * 50);
    QVERIFY(img::adaptiveExtensionRadius(0) == 8);
    QVERIFY(img::adaptiveExtensionRadius(10'000'000) == 160);
}

void TestMasks::silhouetteFromBackground()
{
    QImage im(64, 64, QImage::Format_RGB32);
    im.fill(QColor(250, 250, 250));
    QPainter p(&im);
    p.fillRect(QRect(16, 8, 32, 48), QColor(40, 60, 90));
    p.end();
    QCOMPARE(area(img::silhouetteMask(im)), 32 * 48);
    QImage alpha(32, 32, QImage::Format_ARGB32);
    alpha.fill(Qt::transparent);
    QPainter q(&alpha);
    q.fillRect(QRect(0, 0, 8, 8), Qt::black);
    q.end();
    QCOMPARE(area(img::silhouetteMask(alpha)), 64);
}

void TestMasks::packBitsRoundTrip()
{
    QByteArray row;
    for (int i = 0; i < 300; ++i)
        row.append(char(i < 140 ? 7 : (i % 3 == 0 ? i : 1)));
    row.append(QByteArray(2, 'x'));
    const QByteArray packed = psd::packBits(reinterpret_cast<const uchar *>(row.constData()), int(row.size()));
    QVERIFY(packed.size() < row.size());
    QByteArray out(row.size(), 0);
    const uchar *p = reinterpret_cast<const uchar *>(packed.constData());
    QVERIFY(psd::unpackBits(p, p + packed.size(), reinterpret_cast<uchar *>(out.data()), int(out.size())));
    QCOMPARE(out, row);
}

void TestMasks::psdRoundTrip()
{
    psd::File f;
    f.size = QSize(64, 48);
    QImage body(64, 48, QImage::Format_ARGB32);
    body.fill(Qt::transparent);
    QPainter p(&body);
    p.fillRect(QRect(20, 10, 20, 30), QColor(10, 200, 30, 255));
    p.fillRect(QRect(20, 40, 20, 4), QColor(10, 200, 30, 128));
    p.end();
    QImage eye(6, 4, QImage::Format_ARGB32);
    eye.fill(QColor(0, 0, 255));
    f.layers = {psd::Layer::groupBegin(QStringLiteral("Head")),
                psd::Layer::pixels(QStringLiteral("目 irides"), eye, QPoint(30, 12)),
                psd::Layer::groupEnd(),
                psd::Layer::pixels(QStringLiteral("topwear"), body)};
    f.layers[3].opacity = 200;
    f.layers[1].visible = false;

    QBuffer buf;
    buf.open(QIODevice::ReadWrite);
    QVERIFY(psd::write(&buf, f));
    buf.seek(0);
    QString err;
    const auto back = psd::read(&buf, &err);
    QVERIFY2(back.has_value(), qPrintable(err));
    QCOMPARE(back->size, f.size);
    QCOMPARE(back->layers.size(), 4);
    QCOMPARE(back->layers[0].kind, psd::Layer::Kind::GroupBegin);
    QCOMPARE(back->layers[0].name, QStringLiteral("Head"));
    QCOMPARE(back->layers[1].name, QStringLiteral("目 irides"));
    QVERIFY(!back->layers[1].visible);
    QCOMPARE(back->layers[1].offset, QPoint(30, 12));
    QCOMPARE(back->layers[1].image.pixel(0, 0), qRgba(0, 0, 255, 255));
    QCOMPARE(back->layers[2].kind, psd::Layer::Kind::GroupEnd);
    QCOMPARE(back->layers[3].opacity, 200);
    QCOMPARE(back->layers[3].offset, QPoint(20, 10)); // cropped to opaque bounds
    QCOMPARE(back->layers[3].image.size(), QSize(20, 34));
    QCOMPARE(qAlpha(back->layers[3].image.pixel(5, 32)), 128);
    QVERIFY(std::abs(qGreen(back->layers[3].image.pixel(5, 32)) - 200) <= 1); // stored unpremultiplied
    QVERIFY(!back->composite.isNull());
    QCOMPARE(qAlpha(back->composite.pixel(0, 0)), 0);
    QVERIFY(qAlpha(back->composite.pixel(25, 20)) > 150);
}

void TestMasks::psdComponentsPerformance()
{
    // A 2048x2048 mask with many islands: components + split must stay interactive.
    QImage m = img::createMask({2048, 2048}, 0);
    for (int y = 0; y < 2048; y += 64)
        for (int x = 0; x < 2048; x += 64)
            for (int yy = y; yy < y + 40; ++yy)
                std::fill(m.scanLine(yy) + x, m.scanLine(yy) + x + 40, uchar(255));
    QElapsedTimer t;
    t.start();
    const img::Components c = img::connectedComponents(m);
    QCOMPARE(c.count, 32 * 32);
    const QList<QImage> cut = img::splitByCutLine(rectMask({2048, 2048}, {QRect(0, 900, 2048, 200)}),
                                                  QPolygonF{QPointF(1000, 950), QPointF(1010, 1050)});
    QCOMPARE(cut.size(), 2);
    qInfo() << "components + cut on 2048^2:" << t.elapsed() << "ms";
}

// A model redrew the context of an edit with a drift that changes across the image (brighter on
// the left, darker on the right) and put a new object in the selection: matchEdgeColors removes
// the drift at the selection's edge (no halo once blended) and leaves the object as it is.
void TestMasks::edgeColorsRemoveDrift()
{
    const QSize size(400, 400);
    QImage reference(size, QImage::Format_ARGB32);
    QImage generated(size, QImage::Format_ARGB32);
    QImage mask(size, QImage::Format_Grayscale8);
    mask.fill(0);
    const QPoint centre(200, 200);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            const QColor ref(60 + x / 4, 120, 200 - y / 4);
            reference.setPixelColor(x, y, ref);
            const int drift = 40 - x / 5; // +40 on the left to -40 on the right
            const double d = std::hypot(x - centre.x(), y - centre.y());
            QColor gen = d < 60 ? QColor(20, 40, 230) // the edited object
                                : QColor(std::clamp(ref.red() + drift, 0, 255), std::clamp(ref.green() + drift, 0, 255),
                                         std::clamp(ref.blue() + drift, 0, 255));
            generated.setPixelColor(x, y, gen);
            if (d < 110)
                mask.scanLine(y)[x] = 255;
        }
    }
    const QImage fixed = img::matchEdgeColors(generated, reference, mask, 24, 36);
    QCOMPARE(fixed.size(), size);
    // Just inside the selection's edge (where the feathered blend shows it), on both sides.
    auto error = [&](const QImage &im, int x, int y) {
        const QColor a = im.pixelColor(x, y), b = reference.pixelColor(x, y);
        return std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) + std::abs(a.blue() - b.blue());
    };
    // At least 70 % of the drift goes (context lies on one side only there, so not all of it).
    for (const QPoint p : {QPoint(95, 200), QPoint(305, 200), QPoint(130, 130)}) {
        const int before = error(generated, p.x(), p.y());
        const int after = error(fixed, p.x(), p.y());
        QVERIFY2(before >= 30, "the test drift is there");
        QVERIFY2(after * 10 <= before * 3, qPrintable(QStringLiteral("drift at %1,%2: %3 -> %4").arg(p.x()).arg(p.y()).arg(before).arg(after)));
    }
    // The object keeps the model's colour.
    QCOMPARE(fixed.pixelColor(200, 200), QColor(20, 40, 230));
    // Nothing to correct without context.
    QImage all(size, QImage::Format_Grayscale8);
    all.fill(255);
    QCOMPARE(img::matchEdgeColors(generated, reference, all, 24, 36), generated);
}

// An edit made at half the size gets the original's fine detail back where it kept the picture, and
// none where it changed what is there (no ghost of a removed object), also when the edit made the
// whole picture lighter.
void TestMasks::restoreDetail()
{
    const QSize full(400, 400);
    QImage original(full, QImage::Format_ARGB32);
    for (int y = 0; y < full.height(); ++y) {
        for (int x = 0; x < full.width(); ++x) {
            const int l = (x + y) % 2 ? 160 : 100; // a checker that halving the size turns flat
            original.setPixel(x, y, qRgb(l, l, l));
        }
    }
    QImage edited = original.scaled(200, 200, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    for (int y = 0; y < edited.height(); ++y) {
        for (int x = 0; x < edited.width(); ++x) {
            const int l = std::min(255, qGray(edited.pixel(x, y)) + 20); // lighter, and warm
            edited.setPixel(x, y, qRgb(std::min(255, l + 20), l, std::max(0, l - 20)));
        }
    }
    QPainter(&edited).fillRect(QRect(60, 60, 80, 80), QColor(20, 30, 200)); // something else in the middle
    const QImage restored = img::restoreDetail(edited, original);
    QCOMPARE(restored.size(), full);
    // How much neighbouring pixels differ: the checker's 60 levels, or nothing.
    auto texture = [&restored](const QRect &r) {
        double sum = 0;
        int n = 0;
        for (int y = r.top(); y <= r.bottom(); ++y) {
            for (int x = r.left(); x < r.right(); ++x, ++n)
                sum += std::abs(qGray(restored.pixel(x, y)) - qGray(restored.pixel(x + 1, y)));
        }
        return sum / n;
    };
    QVERIFY2(texture(QRect(20, 20, 60, 60)) > 45, qPrintable(QString::number(texture(QRect(20, 20, 60, 60)))));
    QVERIFY2(texture(QRect(150, 150, 100, 100)) < 3, qPrintable(QString::number(texture(QRect(150, 150, 100, 100)))));
    // The edit's colours stay: lighter and warm outside, blue in the middle.
    QVERIFY(qRed(restored.pixel(30, 31)) > qBlue(restored.pixel(30, 31)) + 20);
    QVERIFY(qBlue(restored.pixel(200, 200)) > 180);
    // Not made smaller: only scaled.
    QCOMPARE(img::restoreDetail(original, original).pixel(10, 11), original.pixel(10, 11));
}

QTEST_MAIN(TestMasks)
#include "tst_masks.moc"
