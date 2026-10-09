#include "ai/PartPipeline.h"

#include "core/ImageOps.h"
#include "core/MaskOps.h"
#include "pose/PosePresets.h"

#include <QJsonArray>
#include <QPainter>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace lumen::ai::parts {

namespace {

QString normalizedLabel(const QString &s)
{
    QString t = s.trimmed().toLower();
    t.replace(QLatin1Char('_'), QLatin1Char(' '));
    t.replace(QLatin1Char('-'), QLatin1Char(' '));
    return t.simplified();
}

struct Target
{
    QString tag;
    QString side;
    QString name;
    const doc::PartKind *kind = nullptr;
    QImage mask;
    int priority = 0; // painting order: higher wins overlaps
};

QColor variedColor(const QColor &base, const QList<doc::Part> &parts, const QString &tag)
{
    int same = 0;
    for (const doc::Part &p : parts)
        same += p.tag == tag;
    if (same == 0)
        return base;
    return same % 2 ? base.lighter(135) : base.darker(130);
}

QList<int> freeLabels(const QList<doc::Part> &parts)
{
    QVector<bool> used(256, false);
    for (const doc::Part &p : parts)
        used[p.label] = true;
    QList<int> out;
    for (int l = 1; l <= doc::PartSet::kMaxParts; ++l) {
        if (!used[l])
            out.append(l);
    }
    return out;
}

QRgb premultiplied(int r, int g, int b, int a)
{
    return qRgba(r * a / 255, g * a / 255, b * a / 255, a);
}

double colorDistance(QRgb a, const QColor &b)
{
    const double dr = qRed(a) - b.red(), dg = qGreen(a) - b.green(), db = qBlue(a) - b.blue();
    return std::sqrt(dr * dr + dg * dg + db * db);
}

} // namespace

// ---- Segmentation -----------------------------------------------------------------------------------

SegmentOutcome mergeSegmentation(const doc::PartSet::Snapshot &current, const QImage &silhouetteIn,
                                 const doc::PartTaxonomy &taxonomy, const QList<SegmentLayer> &layers,
                                 const SegmentOptions &options)
{
    SegmentOutcome outcome;
    const QSize size = current.labels.size();
    const qint64 minIsland = options.minIsland > 0 ? options.minIsland
                                                   : std::max<qint64>(24, qint64(size.width()) * size.height() / 10000);
    const QImage silhouette = (options.clipToSilhouette && silhouetteIn.size() == size)
                                  ? img::dilateMask(silhouetteIn.convertToFormat(QImage::Format_Grayscale8), 1)
                                  : QImage();

    // 1. Visible masks: RGBA layers are stacked (occlusion resolves overlaps), masks are thresholded.
    QList<SegmentLayer> stacked, masks;
    for (const SegmentLayer &l : layers) {
        if (l.image.size() != size)
            continue;
        (l.kind == QLatin1String("layer") ? stacked : masks).append(l);
    }
    auto stackKey = [&](const SegmentLayer &l) {
        if (l.order >= 0)
            return double(l.order);
        if (l.depth >= 0)
            return l.depth * 1000.0;
        const doc::PartKind *k = taxonomy.find(l.label);
        return k ? double(k->order) : 500.0;
    };
    std::stable_sort(stacked.begin(), stacked.end(), [&](const SegmentLayer &a, const SegmentLayer &b) { return stackKey(a) < stackKey(b); });
    QList<QImage> stackImages;
    for (const SegmentLayer &l : stacked)
        stackImages.append(l.image);
    const QList<QImage> visible = img::visibleMasksFromLayers(stackImages);

    QList<Target> targets;
    auto addTarget = [&](const QString &label, const QString &side, const QImage &mask, int stackIndex) {
        const doc::PartKind *kind = taxonomy.find(label);
        const QString tag = kind ? kind->tag : normalizedLabel(label);
        if (tag.isEmpty())
            return;
        if (options.scope == SegmentScope::SelectedKinds && !options.kinds.isEmpty() && !options.kinds.contains(tag))
            return;
        for (Target &t : targets) {
            if (t.tag == tag && t.side == side) {
                t.mask = img::combineMasks(t.mask, mask, img::MaskOp::Union);
                return;
            }
        }
        Target t;
        t.tag = tag;
        t.side = side;
        t.kind = kind;
        t.name = kind ? kind->name : label.trimmed();
        t.mask = img::thresholdMask(mask, 128);
        t.priority = stackIndex >= 0 ? stackIndex : (kind ? kind->order : 1000);
        if (!kind)
            outcome.unmatched.append(label.trimmed());
        targets.append(t);
    };
    for (int i = 0; i < stacked.size(); ++i)
        addTarget(stacked[i].label, stacked[i].side, visible[i], -1); // stacked masks are already exclusive
    for (const SegmentLayer &l : masks)
        addTarget(l.label, l.side, l.image.convertToFormat(QImage::Format_Grayscale8), -1);

    // 2. Clean every mask.
    for (Target &t : targets) {
        if (!silhouette.isNull())
            t.mask = img::combineMasks(t.mask, silhouette, img::MaskOp::Intersect);
        t.mask = img::removeSmallIslands(t.mask, minIsland);
        t.mask = img::fillHoles(t.mask, minIsland);
    }
    std::stable_sort(targets.begin(), targets.end(), [](const Target &a, const Target &b) { return a.priority < b.priority; });

    // 3. Merge into the label map according to the scope.
    QList<doc::Part> parts;
    QImage labels;
    if (options.scope == SegmentScope::ReplaceAll) {
        labels = img::createMask(size, 0);
        for (const doc::Part &p : current.parts) {
            if (p.locked)
                parts.append(p);
        }
    } else {
        labels = current.labels.copy();
        parts = current.parts;
    }
    QVector<bool> locked(256, false);
    for (const doc::Part &p : current.parts)
        locked[p.label] = p.locked;
    if (options.scope == SegmentScope::ReplaceAll) {
        // Locked parts keep their pixels.
        for (int y = 0; y < size.height(); ++y) {
            const uchar *src = current.labels.constScanLine(y);
            uchar *dst = labels.scanLine(y);
            for (int x = 0; x < size.width(); ++x) {
                if (locked[src[x]])
                    dst[x] = src[x];
            }
        }
    }

    QList<int> free = freeLabels(parts);
    QStringList found;
    for (const Target &t : targets) {
        if (img::maskIsEmpty(t.mask, 128))
            continue;
        // Reuse an existing unlocked part of this kind (selected / fill scopes).
        int label = 0;
        if (options.scope != SegmentScope::ReplaceAll) {
            for (doc::Part &p : parts) {
                if (p.tag == t.tag && p.side == t.side && !p.locked) {
                    label = p.label;
                    if (options.scope == SegmentScope::SelectedKinds) {
                        img::replaceLabel(labels, label, 0);
                        if (p.state == doc::PartState::Ready || p.state == doc::PartState::Approved)
                            p.state = doc::PartState::Masked; // mask changed: the result is stale
                    }
                    break;
                }
            }
        }
        if (label == 0) {
            if (free.isEmpty())
                break;
            label = free.takeFirst();
            doc::Part p;
            p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            p.label = label;
            p.tag = t.tag;
            p.side = t.side;
            p.name = t.name;
            p.color = variedColor(t.kind ? t.kind->color : QColor::fromHsv((int(parts.size()) * 47) % 360, 160, 220), parts, t.tag);
            p.order = t.kind ? t.kind->order * 4 : 1000 + int(parts.size());
            p.provenance = options.provenance;
            p.state = doc::PartState::Masked;
            parts.append(p);
            outcome.created.append(p.displayName());
        }
        found.append(t.tag);
        const bool fillOnly = options.scope == SegmentScope::FillUnassigned;
        const QRect b = img::maskBounds(t.mask, 128);
        for (int y = b.top(); y <= b.bottom(); ++y) {
            const uchar *m = t.mask.constScanLine(y);
            uchar *row = labels.scanLine(y);
            for (int x = b.left(); x <= b.right(); ++x) {
                if (m[x] < 128 || locked[row[x]])
                    continue;
                if (fillOnly && row[x] != 0)
                    continue;
                row[x] = uchar(label);
            }
        }
    }

    // 4. Drop new parts that ended up empty; mark emptied existing parts.
    const QVector<qint64> hist = img::labelHistogram(labels);
    QList<doc::Part> kept;
    for (doc::Part p : parts) {
        p.pixels = hist.value(p.label);
        const bool isNew = outcome.created.contains(p.displayName()) && !std::any_of(current.parts.begin(), current.parts.end(),
                                                                                       [&](const doc::Part &c) { return c.id == p.id; });
        if (p.pixels == 0 && isNew) {
            outcome.created.removeAll(p.displayName());
            continue;
        }
        if (p.pixels == 0 && p.state == doc::PartState::Masked)
            p.state = doc::PartState::Empty;
        kept.append(p);
    }
    for (const QString &k : options.kinds) {
        if (!found.contains(k))
            outcome.empty.append(k);
    }
    outcome.labels = labels;
    outcome.parts = kept;
    return outcome;
}

// ---- Skeleton -----------------------------------------------------------------------------------------

namespace {

bool vis(const pose::Person &p, int j)
{
    return p.body[size_t(j)].visible;
}

QPointF at(const pose::Person &p, int j)
{
    return p.body[size_t(j)].pos;
}

void addBone(QList<QLineF> &bones, const pose::Person &p, int a, int b)
{
    if (vis(p, a) && vis(p, b))
        bones.append(QLineF(at(p, a), at(p, b)));
}

// Extends the bone a->b beyond b by `factor` of its length (hands, feet, top of the head).
void addExtension(QList<QLineF> &bones, const pose::Person &p, int a, int b, double factor)
{
    if (!vis(p, a) || !vis(p, b))
        return;
    const QPointF d = at(p, b) - at(p, a);
    bones.append(QLineF(at(p, b), at(p, b) + d * factor));
}

struct LimbBones
{
    QList<QLineF> upper, lower, end;
};

LimbBones armBones(const pose::Person &p, bool right)
{
    const int s = right ? pose::RShoulder : pose::LShoulder;
    const int e = right ? pose::RElbow : pose::LElbow;
    const int w = right ? pose::RWrist : pose::LWrist;
    LimbBones b;
    addBone(b.upper, p, s, e);
    addBone(b.lower, p, e, w);
    addExtension(b.end, p, e, w, 0.4);
    return b;
}

LimbBones legBones(const pose::Person &p, bool right)
{
    const int h = right ? pose::RHip : pose::LHip;
    const int k = right ? pose::RKnee : pose::LKnee;
    const int a = right ? pose::RAnkle : pose::LAnkle;
    LimbBones b;
    addBone(b.upper, p, h, k);
    addBone(b.lower, p, k, a);
    addExtension(b.end, p, k, a, 0.22);
    return b;
}

void appendGroup(QList<BoneGroup> &groups, const QString &tag, const QString &side, const QString &name, const QList<QLineF> &bones)
{
    if (bones.isEmpty())
        return;
    groups.append(BoneGroup{tag, side, name, bones});
}

QString kindName(const QString &taxonomyId, const QString &tag)
{
    const doc::PartKind *k = doc::partTaxonomy(taxonomyId).find(tag);
    return k ? k->name : tag;
}

} // namespace

QList<BoneGroup> limbGroups(const pose::Person &p, const QString &taxonomyId, bool arms, bool legs)
{
    QList<BoneGroup> groups;
    const bool simple = taxonomyId == QLatin1String("simple");
    for (bool right : {false, true}) {
        const QString side = right ? QStringLiteral("R") : QStringLiteral("L");
        if (arms) {
            const LimbBones b = armBones(p, right);
            if (simple) {
                appendGroup(groups, QStringLiteral("arms"), side, kindName(taxonomyId, QStringLiteral("arms")), b.upper + b.lower + b.end);
            } else {
                appendGroup(groups, QStringLiteral("upper arm"), side, kindName(taxonomyId, QStringLiteral("upper arm")), b.upper);
                appendGroup(groups, QStringLiteral("forearm"), side, kindName(taxonomyId, QStringLiteral("forearm")), b.lower);
                appendGroup(groups, QStringLiteral("hand"), side, kindName(taxonomyId, QStringLiteral("hand")), b.end);
            }
        }
        if (legs) {
            const LimbBones b = legBones(p, right);
            if (simple) {
                appendGroup(groups, QStringLiteral("legs"), side, kindName(taxonomyId, QStringLiteral("legs")), b.upper + b.lower + b.end);
            } else {
                appendGroup(groups, QStringLiteral("thigh"), side, kindName(taxonomyId, QStringLiteral("thigh")), b.upper);
                appendGroup(groups, QStringLiteral("shin"), side, kindName(taxonomyId, QStringLiteral("shin")), b.lower);
                appendGroup(groups, QStringLiteral("footwear"), side, kindName(taxonomyId, QStringLiteral("footwear")), b.end);
            }
        }
    }
    return groups;
}

QList<BoneGroup> skeletonGroups(const pose::Person &p, const QString &taxonomyId)
{
    QList<BoneGroup> groups;
    const bool simple = taxonomyId == QLatin1String("simple");
    // Head: face points plus the top of the skull (extension of neck -> nose).
    QList<QLineF> head;
    addBone(head, p, pose::Neck, pose::Nose);
    addExtension(head, p, pose::Neck, pose::Nose, 1.1);
    addBone(head, p, pose::Nose, pose::REye);
    addBone(head, p, pose::Nose, pose::LEye);
    addBone(head, p, pose::REye, pose::REar);
    addBone(head, p, pose::LEye, pose::LEar);
    // Ears are often hidden by hair; widen the head with the shoulder line midpoint offset.
    appendGroup(groups, QStringLiteral("head"), QString(), kindName(taxonomyId, QStringLiteral("head")), head);
    // Torso.
    QList<QLineF> torso;
    addBone(torso, p, pose::Neck, pose::RHip);
    addBone(torso, p, pose::Neck, pose::LHip);
    addBone(torso, p, pose::RHip, pose::LHip);
    if (vis(p, pose::Neck) && vis(p, pose::RShoulder))
        torso.append(QLineF(at(p, pose::Neck), at(p, pose::Neck) + (at(p, pose::RShoulder) - at(p, pose::Neck)) * 0.6));
    if (vis(p, pose::Neck) && vis(p, pose::LShoulder))
        torso.append(QLineF(at(p, pose::Neck), at(p, pose::Neck) + (at(p, pose::LShoulder) - at(p, pose::Neck)) * 0.6));
    const QString torsoTag = simple ? QStringLiteral("body") : QStringLiteral("topwear");
    appendGroup(groups, torsoTag, QString(), kindName(taxonomyId, torsoTag), torso);
    groups += limbGroups(p, taxonomyId, true, true);
    return groups;
}

QList<QImage> splitBySkeleton(const QImage &mask, const QList<BoneGroup> &groups)
{
    QList<QLineF> segments;
    QList<int> owner;
    for (int g = 0; g < groups.size(); ++g) {
        for (const QLineF &l : groups[g].bones) {
            segments.append(l);
            owner.append(g);
        }
    }
    return img::splitByNearestSegmentGroups(mask, segments, owner, int(groups.size()));
}

pose::Person fitSkeleton(const QImage &silhouette)
{
    const QRect bounds = img::maskBounds(silhouette, 128);
    const auto preset = pose::findBuiltInPreset(QStringLiteral("live2d_full"));
    if (!preset || bounds.isEmpty())
        return {};
    const QSize canvas(std::max(64, bounds.width()), std::max(64, bounds.height()));
    pose::Pose pose = preset->toPose(QSize(canvas.width() * 2, canvas.height() * 2));
    if (pose.people.isEmpty())
        return {};
    pose::Person person = pose.people.first();
    person.leftHand.clear();
    person.rightHand.clear();
    person.face.clear();
    const QRectF pb = person.bounds();
    if (pb.height() <= 1)
        return person;
    // The skeleton's head top and ankles sit inside the silhouette: scale its height to ~90% of it.
    const double scale = bounds.height() * 0.9 / pb.height();
    person.transform(pb.center(), scale, 0.0);
    const QRectF nb = person.bounds();
    person.translate(QPointF(bounds.center().x() - nb.center().x(), bounds.top() + bounds.height() * 0.06 - nb.top()));
    return person;
}

// ---- Completion -----------------------------------------------------------------------------------

QColor chooseKeyColor(const QImage &sourceIn, const QImage &visible)
{
    const QImage source = sourceIn.convertToFormat(QImage::Format_ARGB32);
    const QList<QColor> candidates{QColor(255, 255, 255), QColor(0, 0, 0), QColor(0, 255, 0), QColor(255, 0, 255),
                                   QColor(0, 0, 255), QColor(128, 128, 128)};
    QList<QRgb> samples;
    const QRect b = img::maskBounds(visible, 128);
    const int step = std::max(1, int(std::sqrt(double(std::max(1, b.width() * b.height())) / 4000.0)));
    for (int y = b.top(); y <= b.bottom(); y += step) {
        const uchar *m = visible.constScanLine(y);
        const QRgb *row = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        for (int x = b.left(); x <= b.right(); x += step) {
            if (m[x] >= 128 && qAlpha(row[x]) > 128)
                samples.append(row[x]);
        }
    }
    if (samples.isEmpty())
        return candidates.first();
    QColor best = candidates.first();
    double bestFraction = 2.0;
    for (const QColor &c : candidates) {
        int close = 0;
        for (QRgb s : samples)
            close += colorDistance(s, c) < 90.0;
        const double fraction = double(close) / samples.size();
        if (fraction < 0.02)
            return c;
        if (fraction < bestFraction) {
            bestFraction = fraction;
            best = c;
        }
    }
    return best;
}

QString colorName(const QColor &c)
{
    if (c == QColor(255, 255, 255))
        return QStringLiteral("white");
    if (c == QColor(0, 0, 0))
        return QStringLiteral("black");
    if (c == QColor(0, 255, 0))
        return QStringLiteral("green");
    if (c == QColor(255, 0, 255))
        return QStringLiteral("magenta");
    if (c == QColor(0, 0, 255))
        return QStringLiteral("blue");
    return QStringLiteral("grey");
}

CompletionContext buildCompletionContext(const QImage &sourceIn, const QImage &visible, const QImage &extension)
{
    CompletionContext ctx;
    const QImage source = img::normalized(sourceIn);
    ctx.key = chooseKeyColor(source, visible);
    ctx.bounds = img::maskBounds(extension, 128);
    QImage image(source.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(ctx.key);
    const QRgb key = ctx.key.rgb();
    for (int y = 0; y < source.height(); ++y) {
        const uchar *m = visible.constScanLine(y);
        const QRgb *src = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        QRgb *dst = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            if (m[x] < 128)
                continue;
            // Source over key (premultiplied).
            const int a = qAlpha(src[x]);
            const int inv = 255 - a;
            dst[x] = qRgba(qRed(src[x]) + qRed(key) * inv / 255, qGreen(src[x]) + qGreen(key) * inv / 255,
                           qBlue(src[x]) + qBlue(key) * inv / 255, 255);
        }
    }
    ctx.image = image;
    // Generate the hidden area; reach 3 px into the visible edge so the seam blends.
    const QImage hidden = img::combineMasks(extension, img::erodeMask(visible, 3), img::MaskOp::Subtract);
    ctx.mask = img::thresholdMask(hidden, 128);
    return ctx;
}

QImage extractCompletedPart(const QImage &generatedIn, const QImage &sourceIn, const QImage &visible,
                            const QImage &extension, const QColor &key)
{
    const QImage generated = generatedIn.convertToFormat(QImage::Format_ARGB32);
    const QImage source = img::normalized(sourceIn);
    QImage out(source.size(), QImage::Format_ARGB32_Premultiplied);
    out.fill(0);
    constexpr double kLow = 28.0, kHigh = 96.0;
    for (int y = 0; y < source.height(); ++y) {
        const uchar *vm = visible.constScanLine(y);
        const uchar *em = extension.constScanLine(y);
        const QRgb *src = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        const QRgb *gen = reinterpret_cast<const QRgb *>(generated.constScanLine(y));
        QRgb *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            if (vm[x] >= 128) {
                dst[x] = src[x];
                continue;
            }
            if (em[x] == 0)
                continue;
            const QRgb c = gen[x];
            const double d = colorDistance(c, key);
            const double a = std::clamp((d - kLow) / (kHigh - kLow), 0.0, 1.0) * (em[x] / 255.0) * (qAlpha(c) / 255.0);
            if (a <= 0.0)
                continue;
            // Remove the key colour that was mixed into semi-transparent edge pixels.
            auto unmix = [a](int v, int k) { return std::clamp(int(std::lround((v - (1.0 - a) * k) / a)), 0, 255); };
            const int alpha = int(std::lround(a * 255.0));
            dst[x] = premultiplied(unmix(qRed(c), key.red()), unmix(qGreen(c), key.green()), unmix(qBlue(c), key.blue()), alpha);
        }
    }
    return out;
}

QImage cutOutPart(const QImage &source, const QImage &visible)
{
    return img::applyMaskToAlpha(img::normalized(source), visible);
}

QImage partFromLayer(const QImage &layerIn, const QImage &sourceIn, const QImage &visible, const QImage &extension)
{
    const QImage layer = img::normalized(layerIn);
    const QImage source = img::normalized(sourceIn);
    QImage out = img::applyMaskToAlpha(layer, extension);
    for (int y = 0; y < out.height(); ++y) {
        const uchar *vm = visible.constScanLine(y);
        const QRgb *src = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        QRgb *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < out.width(); ++x) {
            if (vm[x] >= 128)
                dst[x] = src[x];
        }
    }
    return out;
}

QRect cropToContent(QImage &layer)
{
    const QRect b = img::maskBounds(img::maskFromAlpha(layer), 1);
    if (b.isEmpty()) {
        layer = QImage();
        return {};
    }
    layer = layer.copy(b);
    return b;
}

QString completionPrompt(const doc::Part &part, const doc::PartKind *kind, const QString &characterSummary, const QColor &key)
{
    QString subject = !part.prompt.trimmed().isEmpty() ? part.prompt.trimmed()
                                                       : (kind && !kind->description.isEmpty() ? kind->description : part.name);
    if (part.side == QLatin1String("L"))
        subject += QStringLiteral(" (the character's left side)");
    else if (part.side == QLatin1String("R"))
        subject += QStringLiteral(" (the character's right side)");
    QString prompt = QStringLiteral("Complete %1").arg(subject);
    if (!characterSummary.trimmed().isEmpty())
        prompt += QStringLiteral(", of this character: %1").arg(characterSummary.trimmed());
    prompt += QStringLiteral(". A single isolated part for a Live2D model on a plain flat %1 background, nothing else in the "
                             "image, same art style, line art and colors as the visible part, continuous shapes, no shadows "
                             "from other parts.")
                  .arg(colorName(key));
    return prompt;
}

// ---- Export -----------------------------------------------------------------------------------------

QList<ExportLayer> exportLayers(const doc::PartSet::Snapshot &set, const QImage &source,
                                const QHash<QString, QImage> &results, bool includeCutOuts)
{
    QList<const doc::Part *> order;
    for (const doc::Part &p : set.parts)
        order.append(&p);
    std::stable_sort(order.begin(), order.end(), [](const doc::Part *a, const doc::Part *b) { return a->order < b->order; });
    const doc::PartTaxonomy &taxonomy = doc::partTaxonomy(set.taxonomy);
    QList<ExportLayer> out;
    for (const doc::Part *p : order) {
        ExportLayer l;
        l.name = p->displayName();
        l.tag = p->tag;
        l.side = p->side;
        l.order = p->order;
        l.visible = p->visible;
        const doc::PartKind *kind = taxonomy.find(p->tag);
        l.group = kind ? kind->group : QStringLiteral("custom");
        l.depth = kind ? kind->depth : 0.5;
        const QImage result = results.value(p->chosenAssetId());
        if (!result.isNull() && p->chosenRect().isValid()) {
            l.image = result;
            l.rect = QRect(p->chosenRect().topLeft(), result.size());
        } else if (includeCutOuts) {
            QImage cut = cutOutPart(source, img::maskForLabel(set.labels, p->label));
            l.rect = cropToContent(cut);
            l.image = cut;
        }
        if (l.image.isNull())
            continue;
        out.append(l);
    }
    return out;
}

QJsonObject exportManifest(const QList<ExportLayer> &layers, const QSize &canvas, const QString &name)
{
    QJsonArray arr;
    int index = 0;
    for (const ExportLayer &l : layers) {
        arr.append(QJsonObject{{QStringLiteral("name"), l.name},
                               {QStringLiteral("tag"), l.tag},
                               {QStringLiteral("side"), l.side},
                               {QStringLiteral("group"), l.group},
                               {QStringLiteral("draw_order"), index},
                               {QStringLiteral("depth"), l.depth},
                               {QStringLiteral("visible"), l.visible},
                               {QStringLiteral("x"), l.rect.x()},
                               {QStringLiteral("y"), l.rect.y()},
                               {QStringLiteral("width"), l.rect.width()},
                               {QStringLiteral("height"), l.rect.height()},
                               {QStringLiteral("file"), QStringLiteral("%1_%2.png").arg(index, 2, 10, QLatin1Char('0')).arg(l.name)}});
        ++index;
    }
    return QJsonObject{{QStringLiteral("name"), name},
                       {QStringLiteral("generator"), QStringLiteral("Lumen Studio")},
                       {QStringLiteral("width"), canvas.width()},
                       {QStringLiteral("height"), canvas.height()},
                       {QStringLiteral("layers"), arr}};
}

} // namespace lumen::ai::parts
