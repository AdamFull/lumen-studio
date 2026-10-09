#include "core/ImageOps.h"
#include "core/MaskOps.h"
#include "doc/Parts.h"

#include <QPainter>
#include <QSignalSpy>
#include <QtTest>

using namespace lumen;
using doc::PartSet;
using doc::PartState;

namespace {

QImage rectMask(const QSize &size, const QRect &r)
{
    QImage m = img::createMask(size, 0);
    for (int y = r.top(); y <= r.bottom(); ++y)
        std::fill(m.scanLine(y) + r.left(), m.scanLine(y) + r.right() + 1, uchar(255));
    return m;
}

QImage character()
{
    QImage im(200, 300, QImage::Format_ARGB32_Premultiplied);
    im.fill(Qt::transparent);
    QPainter p(&im);
    p.fillRect(QRect(60, 20, 80, 80), QColor(255, 220, 200)); // head
    p.fillRect(QRect(50, 100, 100, 120), QColor(40, 90, 160)); // body
    p.fillRect(QRect(10, 110, 40, 20), QColor(255, 220, 200)); // arm R
    p.fillRect(QRect(150, 110, 40, 20), QColor(255, 220, 200)); // arm L
    p.end();
    return im;
}

} // namespace

class TestParts : public QObject
{
    Q_OBJECT
private slots:
    void taxonomyLookup();
    void addAndPaint();
    void splitAndUndo();
    void mergeAndRemove();
    void undoKeepsGenerationState();
    void extensionAreas();
    void unassigned();
    void encodeDecode();
};

void TestParts::taxonomyLookup()
{
    const doc::PartTaxonomy t = doc::partTaxonomy(QStringLiteral("live2d"));
    QVERIFY(t.find(QStringLiteral("front hair")));
    QCOMPARE(t.find(QStringLiteral("Front_Hair"))->tag, QStringLiteral("front hair"));
    QCOMPARE(t.find(QStringLiteral("bangs"))->tag, QStringLiteral("front hair"));
    QCOMPARE(t.find(QStringLiteral("skirt"))->tag, QStringLiteral("bottomwear"));
    QVERIFY(!t.find(QStringLiteral("spaceship")));
    int core = 0;
    for (const doc::PartKind &k : t.kinds)
        core += k.core;
    QCOMPARE(core, 23); // the see-through V3 tag set
    QCOMPARE(doc::partTaxonomy(QStringLiteral("nope")).id, QStringLiteral("live2d"));
    QVERIFY(t.find(QStringLiteral("back hair"))->order < t.find(QStringLiteral("face"))->order);
}

void TestParts::addAndPaint()
{
    PartSet set;
    set.setSource(character(), QStringLiteral("src"));
    QSignalSpy labels(&set, &PartSet::labelsChanged);
    const int body = set.addPart(QStringLiteral("topwear"), QString(), QString(), rectMask(set.size(), QRect(50, 100, 100, 120)));
    QVERIFY(body > 0);
    QCOMPARE(set.part(body)->pixels, 100 * 120);
    QCOMPARE(set.part(body)->state, PartState::Masked);
    QVERIFY(!labels.isEmpty());

    // Paint stroke: claim a block from the body for a new part.
    const int face = set.addPart(QStringLiteral("face"));
    QCOMPARE(set.part(face)->state, PartState::Empty);
    const QRect stroke(60, 90, 80, 20);
    const QImage before = set.labels().copy(stroke);
    QImage &lab = set.labelsForPainting();
    for (int y = stroke.top(); y <= stroke.bottom(); ++y)
        std::fill(lab.scanLine(y) + stroke.left(), lab.scanLine(y) + stroke.right() + 1, uchar(face));
    set.commitLabelPaint(stroke, before, QStringLiteral("Paint"));
    QCOMPARE(set.part(face)->pixels, 80 * 20);
    QCOMPARE(set.part(face)->state, PartState::Masked);
    QCOMPARE(set.part(body)->pixels, 100 * 120 - 80 * 10);
    set.undoStack()->undo();
    QCOMPARE(set.part(face)->pixels, 0);
    QCOMPARE(set.part(body)->pixels, 100 * 120);
    set.undoStack()->redo();
    QCOMPARE(set.part(face)->pixels, 80 * 20);
}

void TestParts::splitAndUndo()
{
    PartSet set;
    set.setSource(character(), QStringLiteral("src"));
    QImage arms = img::combineMasks(rectMask(set.size(), QRect(10, 110, 40, 20)), rectMask(set.size(), QRect(150, 110, 40, 20)),
                                    img::MaskOp::Union);
    const int handwear = set.addPart(QStringLiteral("handwear"), QString(), QString(), arms);
    const auto [left, right] = img::splitByVerticalLine(set.partMask(handwear), 100);
    // Image right = character's left.
    const QList<int> pieces = set.splitPart(handwear, {right, left}, {}, {QStringLiteral("L"), QStringLiteral("R")});
    QCOMPARE(pieces.size(), 2);
    QCOMPARE(set.parts().size(), 2);
    QCOMPARE(set.part(pieces[0])->side, QStringLiteral("L"));
    QCOMPARE(set.part(pieces[1])->side, QStringLiteral("R"));
    QCOMPARE(set.part(pieces[0])->pixels, 800);
    QCOMPARE(set.part(pieces[1])->pixels, 800);
    QCOMPARE(set.part(pieces[1])->tag, QStringLiteral("handwear"));
    QVERIFY(set.part(pieces[0])->color != set.part(pieces[1])->color);
    set.undoStack()->undo();
    QCOMPARE(set.parts().size(), 1);
    QCOMPARE(set.part(handwear)->pixels, 1600);
    QCOMPARE(set.part(handwear)->side, QString());
}

void TestParts::mergeAndRemove()
{
    PartSet set;
    set.setSource(character(), QStringLiteral("src"));
    const int a = set.addPart(QStringLiteral("face"), {}, {}, rectMask(set.size(), QRect(60, 20, 80, 40)));
    const int b = set.addPart(QStringLiteral("face"), {}, {}, rectMask(set.size(), QRect(60, 60, 80, 40)));
    set.mergeParts({a, b});
    QCOMPARE(set.parts().size(), 1);
    QCOMPARE(set.part(a)->pixels, 80 * 80);
    set.removeParts({a});
    QVERIFY(set.parts().isEmpty());
    QCOMPARE(img::labelHistogram(set.labels())[a], 0);
    set.undoStack()->undo();
    QCOMPARE(set.part(a)->pixels, 80 * 80);
    set.undoStack()->undo();
    QCOMPARE(set.parts().size(), 2);
    QCOMPARE(set.part(b)->pixels, 80 * 40);
}

void TestParts::undoKeepsGenerationState()
{
    PartSet set;
    set.setSource(character(), QStringLiteral("src"));
    const int face = set.addPart(QStringLiteral("face"), {}, {}, rectMask(set.size(), QRect(60, 20, 80, 80)));
    doc::Part p = *set.part(face);
    p.name = QStringLiteral("Renamed");
    set.updatePart(p);
    set.addVariant(face, QStringLiteral("asset1"), QRect(50, 10, 100, 100));
    QCOMPARE(set.part(face)->state, PartState::Ready);
    set.undoStack()->undo(); // rename undone, result stays
    QCOMPARE(set.part(face)->name, QStringLiteral("Face"));
    QCOMPARE(set.part(face)->chosenAssetId(), QStringLiteral("asset1"));
    QCOMPARE(set.part(face)->state, PartState::Ready);
    set.addVariant(face, QStringLiteral("asset2"), QRect(), false);
    QCOMPARE(set.part(face)->chosen, 0);
    set.chooseVariant(face, 1);
    QCOMPARE(set.part(face)->chosenAssetId(), QStringLiteral("asset2"));
    set.removeVariant(face, 1);
    QCOMPARE(set.part(face)->chosenAssetId(), QStringLiteral("asset1"));
}

void TestParts::extensionAreas()
{
    PartSet set;
    set.setSource(character(), QStringLiteral("src"));
    const int body = set.addPart(QStringLiteral("topwear"), {}, {}, rectMask(set.size(), QRect(50, 100, 100, 120)));
    const int arm = set.addPart(QStringLiteral("handwear"), {}, {}, rectMask(set.size(), QRect(130, 110, 60, 20)));
    // The arm (in front) took pixels from the body; the body may extend behind it.
    QVERIFY(set.part(arm)->order > set.part(body)->order);
    const QImage occ = set.occluderMask(body);
    QCOMPARE(img::maskCoverage(occ) > 0, true);
    const QImage ext = set.extensionMask(body);
    QCOMPARE(qGray(ext.pixel(140, 120)), 255);
    QCOMPARE(qGray(ext.pixel(5, 5)), 0);
    // Explicit extension replaces the automatic one; visible pixels are always included.
    set.setExtension(body, rectMask(set.size(), QRect(0, 0, 10, 10)), QStringLiteral("Extend"));
    QVERIFY(!set.part(body)->extensionAuto);
    QCOMPARE(qGray(set.extensionMask(body).pixel(5, 5)), 255);
    QCOMPARE(qGray(set.extensionMask(body).pixel(60, 150)), 255);
    QCOMPARE(qGray(set.extensionMask(body).pixel(140, 120)), 0);
    set.resetExtension(body);
    QVERIFY(set.part(body)->extensionAuto);
    // Non-occludable kinds do not extend.
    const int mouth = set.addPart(QStringLiteral("mouth"), {}, {}, rectMask(set.size(), QRect(90, 70, 20, 8)));
    QCOMPARE(img::maskCoverage(set.extensionMask(mouth)), img::maskCoverage(set.partMask(mouth)));
}

void TestParts::unassigned()
{
    PartSet set;
    set.setSource(character(), QStringLiteral("src"), img::silhouetteMask(character()));
    const qint64 total = qint64(std::lround(img::maskCoverage(set.unassignedMask()) * 200 * 300));
    QCOMPARE(total, 80 * 80 + 100 * 120 + 2 * 40 * 20);
    set.addPart(QStringLiteral("face"), {}, {}, rectMask(set.size(), QRect(60, 20, 80, 80)));
    const qint64 left = qint64(std::lround(img::maskCoverage(set.unassignedMask()) * 200 * 300));
    QCOMPARE(left, total - 80 * 80);
}

void TestParts::encodeDecode()
{
    PartSet set;
    set.setSource(character(), QStringLiteral("src"));
    set.setName(QStringLiteral("Hero"));
    set.setStep(2);
    set.setTaxonomy(QStringLiteral("simple"));
    const int head = set.addPart(QStringLiteral("head"), {}, {}, rectMask(set.size(), QRect(60, 20, 80, 80)));
    set.setExtension(head, rectMask(set.size(), QRect(55, 15, 90, 90)), QStringLiteral("Extend"));
    set.addVariant(head, QStringLiteral("a1"), QRect(1, 2, 3, 4));
    set.setPartState(head, PartState::Generating);

    const QJsonObject json = PartSet::encode(set.snapshot());
    const PartSet::Snapshot s = PartSet::decode(json);
    PartSet copy;
    copy.restore(QStringLiteral("id1"), s, character(), QImage());
    QCOMPARE(copy.id(), QStringLiteral("id1"));
    QCOMPARE(copy.name(), QStringLiteral("Hero"));
    QCOMPARE(copy.step(), 2);
    QCOMPARE(copy.taxonomyId(), QStringLiteral("simple"));
    QCOMPARE(copy.parts().size(), 1);
    const doc::Part *p = copy.part(head);
    QVERIFY(p);
    QCOMPARE(p->pixels, 80 * 80);
    QCOMPARE(p->chosenAssetId(), QStringLiteral("a1"));
    QCOMPARE(p->chosenRect(), QRect(1, 2, 3, 4));
    QCOMPARE(p->state, PartState::Ready); // interrupted generation is not resumed
    QVERIFY(!p->extensionAuto);
    QCOMPARE(p->extensionRect, QRect(55, 15, 90, 90));
    QCOMPARE(copy.labels(), set.labels());
    QVERIFY(copy.undoStack()->count() == 0);
}

QTEST_MAIN(TestParts)
#include "tst_parts.moc"
