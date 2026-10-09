#include "doc/Parts.h"

#include "core/Async.h"
#include "core/ImageOps.h"
#include "core/Log.h"
#include "core/MaskOps.h"
#include "doc/Project.h"

#include <QJsonArray>
#include <QPainter>
#include <QUndoCommand>
#include <QUuid>

#include <algorithm>

namespace lumen::doc {

// ---- Taxonomy -----------------------------------------------------------------------------------

namespace {

PartKind kind(const char *tag, const QString &name, const char *group, int order, QRgb color, bool occludable,
              double depth, const QString &description, const char *segmentPrompt, QStringList aliases = {},
              bool core = true)
{
    PartKind k;
    k.tag = QString::fromLatin1(tag);
    k.name = name;
    k.group = QString::fromLatin1(group);
    k.order = order;
    k.color = QColor::fromRgb(color);
    k.occludable = occludable;
    k.depth = depth;
    k.description = description;
    k.segmentPrompt = QString::fromLatin1(segmentPrompt);
    k.aliases = aliases;
    k.core = core;
    return k;
}

QList<PartTaxonomy> buildTaxonomies()
{
    using QSL = QStringList;
    PartTaxonomy live2d;
    live2d.id = QStringLiteral("live2d");
    live2d.name = QObject::tr("Live2D (see-through tags)");
    live2d.description = QObject::tr("The 23 semantic layers used by see-through plus limb and side-hair parts for "
                                     "detailed rigs. Recommended for Live2D models.");
    // Draw order: back (low) to front (high).
    live2d.kinds = {
        kind("wings", QObject::tr("Wings"), "extra", 0, 0x8e7cc3, true, 0.05,
             QStringLiteral("the complete wings, including the parts hidden behind the body"), "wings"),
        kind("tail", QObject::tr("Tail"), "extra", 1, 0xb4a7d6, true, 0.08,
             QStringLiteral("the complete tail, including the base hidden behind the body"), "tail"),
        kind("back hair", QObject::tr("Back hair"), "head", 2, 0x3d85c6, true, 0.1,
             QStringLiteral("the back layer of the hair behind the head and body, complete and continuous, "
                            "including the parts hidden by the face, neck and body"),
             "hair", QSL{QStringLiteral("hair back"), QStringLiteral("backhair")}),
        kind("thigh", QObject::tr("Thigh"), "body", 3, 0xf6b26b, true, 0.3,
             QStringLiteral("the complete thigh, continuing under the clothing above it"), "thigh", {}, false),
        kind("shin", QObject::tr("Lower leg"), "body", 4, 0xf9cb9c, true, 0.3,
             QStringLiteral("the complete lower leg from knee to ankle"), "leg", {}, false),
        kind("legwear", QObject::tr("Legs / legwear"), "body", 5, 0xe69138, true, 0.3,
             QStringLiteral("the complete legs with stockings or leggings, continuing under the skirt or shorts"),
             "legs", QSL{QStringLiteral("legs"), QStringLiteral("leg")}),
        kind("footwear", QObject::tr("Footwear"), "body", 6, 0xb45f06, false, 0.32,
             QStringLiteral("the shoes or feet"), "shoes", QSL{QStringLiteral("shoes"), QStringLiteral("feet")}),
        kind("body", QObject::tr("Body (skin)"), "body", 7, 0xffd9b3, true, 0.33,
             QStringLiteral("the bare torso skin, complete under the clothing"), "body", {}, false),
        kind("bottomwear", QObject::tr("Bottomwear"), "body", 8, 0x6aa84f, true, 0.36,
             QStringLiteral("the complete skirt or trousers, including the parts hidden by the arms and hands"),
             "skirt . pants", QSL{QStringLiteral("skirt"), QStringLiteral("pants")}),
        kind("topwear", QObject::tr("Topwear / torso"), "body", 9, 0x38761d, true, 0.38,
             QStringLiteral("the complete upper-body clothing and torso, including the areas hidden by the arms "
                            "and the hair"),
             "shirt . jacket . dress", QSL{QStringLiteral("torso"), QStringLiteral("clothes"), QStringLiteral("dress")}),
        kind("upper arm", QObject::tr("Upper arm"), "body", 10, 0xd5a6bd, true, 0.4,
             QStringLiteral("the complete upper arm including the shoulder joint and sleeve"), "upper arm", {}, false),
        kind("forearm", QObject::tr("Forearm"), "body", 11, 0xc27ba0, true, 0.42,
             QStringLiteral("the complete forearm from elbow to wrist"), "forearm", {}, false),
        kind("hand", QObject::tr("Hand"), "body", 12, 0xa64d79, false, 0.45,
             QStringLiteral("the complete hand with all fingers"), "hand", {}, false),
        kind("handwear", QObject::tr("Arms / handwear"), "body", 13, 0x741b47, true, 0.43,
             QStringLiteral("the complete arms and hands with sleeves or gloves"), "arms . hands . gloves",
             QSL{QStringLiteral("arms"), QStringLiteral("arm"), QStringLiteral("hands"), QStringLiteral("gloves")}),
        kind("neck", QObject::tr("Neck"), "body", 14, 0xffe599, true, 0.5,
             QStringLiteral("the neck skin, continuing up under the chin and down under the collar"), "neck"),
        kind("neckwear", QObject::tr("Neckwear"), "body", 15, 0xbf9000, false, 0.52,
             QStringLiteral("the collar, tie, ribbon or necklace"), "necktie . collar . necklace . ribbon",
             QSL{QStringLiteral("collar"), QStringLiteral("tie")}),
        kind("ears", QObject::tr("Ears"), "head", 16, 0xf4cccc, true, 0.55,
             QStringLiteral("the complete ears, including the parts hidden by the hair"), "ears",
             QSL{QStringLiteral("ear"), QStringLiteral("animal ears")}),
        kind("head", QObject::tr("Head (coarse)"), "head", 17, 0xfce5cd, true, 0.6,
             QStringLiteral("the complete head"), "head", {}, false),
        kind("face", QObject::tr("Face"), "head", 18, 0xffe6cc, true, 0.6,
             QStringLiteral("the face skin without hair or facial features, complete including the forehead "
                            "hidden by the bangs"),
             "face", QSL{QStringLiteral("skin"), QStringLiteral("face skin")}),
        kind("nose", QObject::tr("Nose"), "head", 19, 0xe6b8af, false, 0.68, QStringLiteral("the nose"), "nose"),
        kind("mouth", QObject::tr("Mouth"), "head", 20, 0xcc0000, false, 0.66,
             QStringLiteral("the mouth with lips, closed"), "mouth", QSL{QStringLiteral("lips")}),
        kind("eyewhite", QObject::tr("Eye whites"), "head", 21, 0xf3f3f3, true, 0.64,
             QStringLiteral("the complete whites of the eyes (sclera), including the parts hidden by the eyelids"),
             "eyes", QSL{QStringLiteral("sclera"), QStringLiteral("eye white")}),
        kind("irides", QObject::tr("Irises"), "head", 22, 0x0b5394, true, 0.65,
             QStringLiteral("the complete round irises and pupils, including the parts hidden by the eyelids"),
             "iris . pupil", QSL{QStringLiteral("iris"), QStringLiteral("irises"), QStringLiteral("pupils"),
                                 QStringLiteral("eyes")}),
        kind("eyelash", QObject::tr("Eyelashes / lids"), "head", 23, 0x351c75, false, 0.67,
             QStringLiteral("the upper and lower eyelashes and eyelid lines"), "eyelashes",
             QSL{QStringLiteral("eyelashes"), QStringLiteral("eyelids")}),
        kind("eyebrow", QObject::tr("Eyebrows"), "head", 24, 0x674ea7, true, 0.7,
             QStringLiteral("the complete eyebrows, including the parts hidden by the bangs"), "eyebrows",
             QSL{QStringLiteral("eyebrows"), QStringLiteral("brows")}),
        kind("earwear", QObject::tr("Earwear"), "head", 25, 0xffd966, false, 0.62,
             QStringLiteral("the earrings or ear accessories"), "earrings", QSL{QStringLiteral("earrings")}),
        kind("side hair", QObject::tr("Side hair"), "head", 26, 0x6fa8dc, true, 0.78,
             QStringLiteral("the complete side lock of hair framing the face"), "side locks", {}, false),
        kind("front hair", QObject::tr("Front hair"), "head", 27, 0x9fc5e8, false, 0.82,
             QStringLiteral("the bangs and front hair"), "bangs",
             QSL{QStringLiteral("bangs"), QStringLiteral("fringe"), QStringLiteral("hair front"), QStringLiteral("hair")}),
        kind("eyewear", QObject::tr("Eyewear"), "head", 28, 0x76a5af, false, 0.86, QStringLiteral("the glasses"),
             "glasses", QSL{QStringLiteral("glasses")}),
        kind("headwear", QObject::tr("Headwear"), "head", 29, 0x45818e, true, 0.88,
             QStringLiteral("the complete hat, hair ornament or headband"), "hat . hair ornament . headband",
             QSL{QStringLiteral("hat"), QStringLiteral("hair ornament")}),
        kind("objects", QObject::tr("Held objects"), "extra", 30, 0x999999, true, 0.9,
             QStringLiteral("the complete held object, including parts hidden by the hands"), "held object",
             QSL{QStringLiteral("object"), QStringLiteral("weapon"), QStringLiteral("item")}),
    };

    PartTaxonomy simple;
    simple.id = QStringLiteral("simple");
    simple.name = QObject::tr("Simple");
    simple.description = QObject::tr("A few large parts: hair, head, body, arms and legs. Good for quick cut-outs "
                                     "and the skeleton-based segmenter.");
    simple.kinds = {
        kind("back hair", QObject::tr("Back hair"), "head", 0, 0x3d85c6, true, 0.1,
             QStringLiteral("the back layer of the hair behind the head and body"), "hair"),
        kind("legs", QObject::tr("Legs"), "body", 1, 0xe69138, true, 0.3, QStringLiteral("the complete legs and feet"),
             "legs", QSL{QStringLiteral("legwear"), QStringLiteral("footwear"), QStringLiteral("thigh"), QStringLiteral("shin")}),
        kind("body", QObject::tr("Body"), "body", 2, 0x38761d, true, 0.38,
             QStringLiteral("the complete torso with clothing"), "body . clothes",
             QSL{QStringLiteral("topwear"), QStringLiteral("bottomwear"), QStringLiteral("torso")}),
        kind("arms", QObject::tr("Arms"), "body", 3, 0xa64d79, true, 0.43,
             QStringLiteral("the complete arms and hands"), "arms . hands",
             QSL{QStringLiteral("handwear"), QStringLiteral("upper arm"), QStringLiteral("forearm"), QStringLiteral("hand")}),
        kind("head", QObject::tr("Head"), "head", 4, 0xffe6cc, true, 0.6,
             QStringLiteral("the complete head and face with its features"), "head . face",
             QSL{QStringLiteral("face"), QStringLiteral("neck"), QStringLiteral("ears")}),
        kind("front hair", QObject::tr("Front hair"), "head", 5, 0x9fc5e8, false, 0.82,
             QStringLiteral("the bangs and front hair"), "bangs", QSL{QStringLiteral("side hair"), QStringLiteral("hair")}),
        kind("accessories", QObject::tr("Accessories"), "extra", 6, 0x999999, false, 0.9,
             QStringLiteral("the accessories and held objects"), "accessory . hat . held object",
             QSL{QStringLiteral("headwear"), QStringLiteral("objects"), QStringLiteral("eyewear"), QStringLiteral("earwear"),
                 QStringLiteral("neckwear")}),
    };
    return {live2d, simple};
}

QString normalizedTag(const QString &s)
{
    QString t = s.trimmed().toLower();
    t.replace(QLatin1Char('_'), QLatin1Char(' '));
    t.replace(QLatin1Char('-'), QLatin1Char(' '));
    return t.simplified();
}

QString newUuid()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QJsonObject partToJson(const Part &p)
{
    QJsonObject o{{QStringLiteral("id"), p.id},
                  {QStringLiteral("label"), p.label},
                  {QStringLiteral("name"), p.name},
                  {QStringLiteral("tag"), p.tag},
                  {QStringLiteral("side"), p.side},
                  {QStringLiteral("color"), p.color.name()},
                  {QStringLiteral("order"), p.order},
                  {QStringLiteral("visible"), p.visible},
                  {QStringLiteral("locked"), p.locked},
                  {QStringLiteral("prompt"), p.prompt},
                  {QStringLiteral("provenance"), p.provenance},
                  {QStringLiteral("extension_auto"), p.extensionAuto},
                  {QStringLiteral("state"), partStateName(p.state)},
                  {QStringLiteral("error"), p.error},
                  {QStringLiteral("chosen"), p.chosen}};
    QJsonArray variants;
    for (int i = 0; i < p.variants.size(); ++i) {
        const QRect r = p.variantRects.value(i);
        variants.append(QJsonObject{{QStringLiteral("asset"), p.variants[i]},
                                    {QStringLiteral("rect"), QJsonArray{r.x(), r.y(), r.width(), r.height()}}});
    }
    o.insert(QStringLiteral("variants"), variants);
    if (!p.extensionAuto && !p.extension.isNull()) {
        o.insert(QStringLiteral("extension"), QString::fromLatin1(img::toBase64(p.extension, "PNG")));
        const QRect r = p.extensionRect;
        o.insert(QStringLiteral("extension_rect"), QJsonArray{r.x(), r.y(), r.width(), r.height()});
    }
    return o;
}

QRect rectFromJson(const QJsonValue &v)
{
    const QJsonArray a = v.toArray();
    if (a.size() != 4)
        return {};
    return QRect(a[0].toInt(), a[1].toInt(), a[2].toInt(), a[3].toInt());
}

Part partFromJson(const QJsonObject &o)
{
    Part p;
    p.id = o.value(QStringLiteral("id")).toString();
    if (p.id.isEmpty())
        p.id = newUuid();
    p.label = o.value(QStringLiteral("label")).toInt();
    p.name = o.value(QStringLiteral("name")).toString();
    p.tag = o.value(QStringLiteral("tag")).toString();
    p.side = o.value(QStringLiteral("side")).toString();
    p.color = QColor(o.value(QStringLiteral("color")).toString());
    p.order = o.value(QStringLiteral("order")).toInt();
    p.visible = o.value(QStringLiteral("visible")).toBool(true);
    p.locked = o.value(QStringLiteral("locked")).toBool(false);
    p.prompt = o.value(QStringLiteral("prompt")).toString();
    p.provenance = o.value(QStringLiteral("provenance")).toString();
    p.extensionAuto = o.value(QStringLiteral("extension_auto")).toBool(true);
    p.state = partStateFromName(o.value(QStringLiteral("state")).toString());
    p.error = o.value(QStringLiteral("error")).toString();
    p.chosen = o.value(QStringLiteral("chosen")).toInt(-1);
    for (const QJsonValue &v : o.value(QStringLiteral("variants")).toArray()) {
        const QJsonObject vo = v.toObject();
        p.variants.append(vo.value(QStringLiteral("asset")).toString());
        p.variantRects.append(rectFromJson(vo.value(QStringLiteral("rect"))));
    }
    if (p.chosen >= p.variants.size())
        p.chosen = p.variants.isEmpty() ? -1 : 0;
    // Interrupted jobs are not running any more after a reload.
    if (p.state == PartState::Queued || p.state == PartState::Generating)
        p.state = p.hasResult() ? PartState::Ready : PartState::Masked;
    if (!p.extensionAuto) {
        p.extension = img::fromBase64(o.value(QStringLiteral("extension")).toString().toLatin1());
        if (!p.extension.isNull())
            p.extension = p.extension.convertToFormat(QImage::Format_Grayscale8);
        p.extensionRect = rectFromJson(o.value(QStringLiteral("extension_rect")));
        if (p.extension.isNull() || p.extensionRect.size() != p.extension.size()) {
            p.extensionAuto = true;
            p.extension = QImage();
            p.extensionRect = QRect();
        }
    }
    return p;
}

// Keeps generation fields of the live parts when restoring a mask-history state.
void carryGenerationState(QList<Part> &target, const QList<Part> &live)
{
    for (Part &p : target) {
        for (const Part &l : live) {
            if (l.id == p.id) {
                p.state = l.state;
                p.error = l.error;
                p.variants = l.variants;
                p.variantRects = l.variantRects;
                p.chosen = l.chosen;
                break;
            }
        }
    }
}

class MaskCommand : public QUndoCommand
{
public:
    MaskCommand(PartSet *set, const QString &text, PartSet::MaskState before, PartSet::MaskState after)
        : QUndoCommand(text)
        , m_set(set)
        , m_before(std::move(before))
        , m_after(std::move(after))
    {
    }
    void undo() override { m_set->applyMaskState(m_before); }
    void redo() override
    {
        if (m_first) {
            m_first = false; // the change is already applied when the command is pushed
            return;
        }
        m_set->applyMaskState(m_after);
    }

private:
    PartSet *m_set;
    PartSet::MaskState m_before;
    PartSet::MaskState m_after;
    bool m_first = true;
};

} // namespace

const PartKind *PartTaxonomy::find(const QString &tagOrAlias) const
{
    const QString t = normalizedTag(tagOrAlias);
    for (const PartKind &k : kinds) {
        if (normalizedTag(k.tag) == t)
            return &k;
    }
    for (const PartKind &k : kinds) {
        for (const QString &a : k.aliases) {
            if (normalizedTag(a) == t)
                return &k;
        }
    }
    return nullptr;
}

QStringList PartTaxonomy::groups() const
{
    QStringList g;
    for (const PartKind &k : kinds) {
        if (!g.contains(k.group))
            g.append(k.group);
    }
    return g;
}

const QList<PartTaxonomy> &partTaxonomies()
{
    static const QList<PartTaxonomy> list = buildTaxonomies();
    return list;
}

const PartTaxonomy &partTaxonomy(const QString &id)
{
    for (const PartTaxonomy &t : partTaxonomies()) {
        if (t.id == id)
            return t;
    }
    return partTaxonomies().first();
}

QString partGroupLabel(const QString &group)
{
    if (group == QLatin1String("head"))
        return QObject::tr("Head");
    if (group == QLatin1String("body"))
        return QObject::tr("Body");
    if (group == QLatin1String("extra"))
        return QObject::tr("Extras");
    return QObject::tr("Custom");
}

QString partStateName(PartState state)
{
    switch (state) {
    case PartState::Empty: return QStringLiteral("empty");
    case PartState::Masked: return QStringLiteral("masked");
    case PartState::Queued: return QStringLiteral("queued");
    case PartState::Generating: return QStringLiteral("generating");
    case PartState::Ready: return QStringLiteral("ready");
    case PartState::Approved: return QStringLiteral("approved");
    case PartState::Failed: return QStringLiteral("failed");
    }
    return QStringLiteral("empty");
}

QString partStateLabel(PartState state)
{
    switch (state) {
    case PartState::Empty: return QObject::tr("No mask");
    case PartState::Masked: return QObject::tr("Masked");
    case PartState::Queued: return QObject::tr("Queued");
    case PartState::Generating: return QObject::tr("Generating");
    case PartState::Ready: return QObject::tr("Ready");
    case PartState::Approved: return QObject::tr("Approved");
    case PartState::Failed: return QObject::tr("Failed");
    }
    return {};
}

PartState partStateFromName(const QString &name)
{
    for (PartState s : {PartState::Empty, PartState::Masked, PartState::Queued, PartState::Generating, PartState::Ready,
                        PartState::Approved, PartState::Failed}) {
        if (partStateName(s) == name)
            return s;
    }
    return PartState::Empty;
}

QString Part::displayName() const
{
    if (side.isEmpty())
        return name;
    return QStringLiteral("%1 %2").arg(name, side);
}

// ---- PartSet ------------------------------------------------------------------------------------

PartSet::PartSet(QObject *parent)
    : QObject(parent)
    , m_taxonomy(partTaxonomies().first().id)
{
    m_id = newUuid();
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(1500);
    connect(&m_saveTimer, &QTimer::timeout, this, [this] { saveNow(); });
    connect(&m_undo, &QUndoStack::indexChanged, this, [this] { touch(); });
}

PartSet::~PartSet() = default;

void PartSet::setName(const QString &name)
{
    if (m_name == name)
        return;
    m_name = name;
    touch();
}

void PartSet::setSource(const QImage &image, const QString &assetId, const QImage &silhouette)
{
    const QImage normalized = img::normalized(image);
    const bool keep = !m_labels.isNull() && m_labels.size() == normalized.size();
    m_source = normalized;
    m_sourceAssetId = assetId;
    m_silhouette = silhouette.isNull() ? QImage() : silhouette.convertToFormat(QImage::Format_Grayscale8);
    if (!keep) {
        m_labels = img::createMask(normalized.size(), 0);
        m_parts.clear();
        m_results.clear();
        m_decomposition = QJsonObject();
    }
    m_undo.clear();
    updateCoverage();
    emit sourceChanged();
    emit partsChanged();
    emit labelsChanged(m_labels.rect());
    touch();
}

void PartSet::setSilhouette(const QImage &mask)
{
    m_silhouette = mask.isNull() ? QImage() : mask.convertToFormat(QImage::Format_Grayscale8);
    emit labelsChanged(m_labels.rect());
}

void PartSet::setTaxonomy(const QString &id)
{
    if (m_taxonomy == id)
        return;
    m_taxonomy = partTaxonomy(id).id;
    emit taxonomyChanged();
    touch();
}

void PartSet::setStep(int step)
{
    if (m_step == step)
        return;
    m_step = step;
    emit stepChanged(step);
    touch();
}

void PartSet::setSettings(const QJsonObject &settings)
{
    if (m_settings == settings)
        return;
    m_settings = settings;
    touch();
}

void PartSet::setDecomposition(const QJsonObject &d)
{
    m_decomposition = d;
    touch();
}

const Part *PartSet::part(int label) const
{
    for (const Part &p : m_parts) {
        if (p.label == label)
            return &p;
    }
    return nullptr;
}

Part *PartSet::mutablePart(int label)
{
    for (Part &p : m_parts) {
        if (p.label == label)
            return &p;
    }
    return nullptr;
}

const Part *PartSet::partById(const QString &id) const
{
    for (const Part &p : m_parts) {
        if (p.id == id)
            return &p;
    }
    return nullptr;
}

QList<const Part *> PartSet::partsInDrawOrder() const
{
    QList<const Part *> list;
    for (const Part &p : m_parts)
        list.append(&p);
    std::stable_sort(list.begin(), list.end(), [](const Part *a, const Part *b) { return a->order < b->order; });
    return list;
}

int PartSet::freeLabel() const
{
    QVector<bool> used(256, false);
    for (const Part &p : m_parts)
        used[p.label] = true;
    for (int l = 1; l <= kMaxParts; ++l) {
        if (!used[l])
            return l;
    }
    return 0;
}

QColor PartSet::suggestColor(const QString &tag) const
{
    QColor base;
    if (const PartKind *k = taxonomy().find(tag))
        base = k->color;
    if (!base.isValid())
        base = QColor::fromHsv((int(m_parts.size()) * 47) % 360, 160, 220);
    // Same kind twice (left/right splits): vary the lightness so both stay distinguishable.
    int same = 0;
    for (const Part &p : m_parts)
        same += p.tag == tag && !tag.isEmpty();
    if (same > 0)
        base = same % 2 ? base.lighter(135) : base.darker(130);
    return base;
}

Part PartSet::makePart(const QString &tag, const QString &name, const QString &side) const
{
    Part p;
    p.id = newUuid();
    p.label = freeLabel();
    p.tag = tag;
    p.side = side;
    const PartKind *k = taxonomy().find(tag);
    p.name = !name.isEmpty() ? name : (k ? k->name : tag);
    p.color = suggestColor(tag);
    if (k) {
        p.order = k->order * 4;
    } else {
        int maxOrder = 0;
        for (const Part &o : m_parts)
            maxOrder = std::max(maxOrder, o.order);
        p.order = maxOrder + 1;
    }
    p.provenance = QStringLiteral("manual");
    return p;
}

void PartSet::pushMaskChange(const QList<Part> &before, const QRect &rect, const QImage &labelsBefore, const QString &text)
{
    MaskState b{before, rect, labelsBefore};
    MaskState a{m_parts, rect, rect.isNull() ? QImage() : m_labels.copy(rect)};
    m_undo.push(new MaskCommand(this, text, std::move(b), std::move(a)));
}

void PartSet::applyMaskState(const MaskState &state)
{
    QList<Part> parts = state.parts;
    carryGenerationState(parts, m_parts);
    m_parts = parts;
    if (!state.rect.isNull() && !state.labels.isNull()) {
        for (int y = 0; y < state.rect.height(); ++y)
            memcpy(m_labels.scanLine(state.rect.y() + y) + state.rect.x(), state.labels.constScanLine(y),
                   size_t(state.rect.width()));
    }
    updateCoverage();
    emit partsChanged();
    if (!state.rect.isNull())
        emit labelsChanged(state.rect);
}

int PartSet::addPart(const QString &tag, const QString &name, const QString &side, const QImage &mask)
{
    if (!hasSource())
        return 0;
    Part p = makePart(tag, name, side);
    if (p.label == 0)
        return 0;
    const QList<Part> before = m_parts;
    QRect rect;
    QImage labelsBefore;
    if (!mask.isNull()) {
        rect = img::maskBounds(mask, 128);
        if (!rect.isEmpty()) {
            labelsBefore = m_labels.copy(rect);
            img::paintLabel(m_labels, mask, p.label);
            p.state = PartState::Masked;
        } else {
            rect = QRect();
        }
    }
    m_parts.append(p);
    pushMaskChange(before, rect, labelsBefore, tr("Add part %1").arg(p.displayName()));
    updateCoverage();
    emit partsChanged();
    if (!rect.isNull())
        emit labelsChanged(rect);
    return p.label;
}

void PartSet::removeParts(const QList<int> &labels)
{
    if (labels.isEmpty())
        return;
    const QList<Part> before = m_parts;
    QRect rect;
    for (int l : labels)
        rect |= img::labelBounds(m_labels, l);
    const QImage labelsBefore = rect.isEmpty() ? QImage() : m_labels.copy(rect);
    for (int l : labels)
        img::replaceLabel(m_labels, l, 0);
    m_parts.erase(std::remove_if(m_parts.begin(), m_parts.end(), [&](const Part &p) { return labels.contains(p.label); }),
                  m_parts.end());
    pushMaskChange(before, rect.isEmpty() ? QRect() : rect, labelsBefore,
                   labels.size() == 1 ? tr("Delete part") : tr("Delete %n parts", nullptr, int(labels.size())));
    updateCoverage();
    emit partsChanged();
    if (!rect.isEmpty())
        emit labelsChanged(rect);
}

void PartSet::mergeParts(const QList<int> &labels)
{
    if (labels.size() < 2)
        return;
    const int target = labels.first();
    if (!part(target))
        return;
    const QList<Part> before = m_parts;
    QRect rect;
    for (int l : labels)
        rect |= img::labelBounds(m_labels, l);
    const QImage labelsBefore = rect.isEmpty() ? QImage() : m_labels.copy(rect);
    for (int i = 1; i < labels.size(); ++i)
        img::replaceLabel(m_labels, labels[i], target);
    m_parts.erase(std::remove_if(m_parts.begin(), m_parts.end(),
                                 [&](const Part &p) { return p.label != target && labels.contains(p.label); }),
                  m_parts.end());
    if (Part *t = mutablePart(target)) {
        t->provenance = QStringLiteral("merge");
        t->side.clear();
        t->extensionAuto = true;
        t->extension = QImage();
        t->extensionRect = QRect();
        if (t->state == PartState::Ready || t->state == PartState::Approved)
            t->state = PartState::Masked; // the result no longer matches the mask
    }
    pushMaskChange(before, rect.isEmpty() ? QRect() : rect, labelsBefore, tr("Merge parts"));
    updateCoverage();
    emit partsChanged();
    if (!rect.isEmpty())
        emit labelsChanged(rect);
}

QList<int> PartSet::splitPart(int label, const QList<QImage> &pieces, const QStringList &names, const QStringList &sides,
                              const QString &undoText)
{
    const Part *orig = part(label);
    if (!orig || pieces.size() < 2)
        return {label};
    const QList<Part> before = m_parts;
    const QRect rect = img::labelBounds(m_labels, label);
    const QImage labelsBefore = rect.isEmpty() ? QImage() : m_labels.copy(rect);
    const Part base = *orig;
    QList<int> result{label};
    if (Part *first = mutablePart(label)) {
        if (!names.value(0).isEmpty())
            first->name = names.value(0);
        if (!sides.value(0).isNull())
            first->side = sides.value(0);
        first->provenance = QStringLiteral("split");
        first->extensionAuto = true;
        first->extension = QImage();
        first->extensionRect = QRect();
        if (first->state == PartState::Ready || first->state == PartState::Approved)
            first->state = PartState::Masked;
    }
    for (int i = 1; i < pieces.size(); ++i) {
        Part p = makePart(base.tag, names.value(i, base.name), sides.value(i, base.side));
        if (p.label == 0)
            break;
        p.order = base.order;
        p.prompt = base.prompt;
        p.provenance = QStringLiteral("split");
        p.state = PartState::Masked;
        m_parts.append(p);
        // Only pixels that belonged to the original part move.
        const QImage &piece = pieces[i];
        const QRect b = img::maskBounds(piece, 128).intersected(rect);
        for (int y = b.top(); y <= b.bottom(); ++y) {
            const uchar *m = piece.constScanLine(y);
            uchar *row = m_labels.scanLine(y);
            for (int x = b.left(); x <= b.right(); ++x) {
                if (m[x] >= 128 && row[x] == label)
                    row[x] = uchar(p.label);
            }
        }
        result.append(p.label);
    }
    pushMaskChange(before, rect.isEmpty() ? QRect() : rect, labelsBefore, undoText.isEmpty() ? tr("Split part") : undoText);
    updateCoverage();
    emit partsChanged();
    if (!rect.isEmpty())
        emit labelsChanged(rect);
    return result;
}

void PartSet::updatePart(const Part &updated, const QString &undoText)
{
    if (!part(updated.label))
        return;
    const QList<Part> before = m_parts; // copy before taking a mutable pointer (implicit sharing)
    Part *p = mutablePart(updated.label);
    p->name = updated.name;
    p->tag = updated.tag;
    p->side = updated.side;
    p->color = updated.color;
    p->order = updated.order;
    p->visible = updated.visible;
    p->locked = updated.locked;
    p->prompt = updated.prompt;
    pushMaskChange(before, QRect(), QImage(), undoText.isEmpty() ? tr("Edit part") : undoText);
    emit partsChanged();
    emit labelsChanged(m_labels.rect()); // colours / visibility affect the overlay
}

void PartSet::reorder(const QList<int> &labelsBackToFront)
{
    const QList<Part> before = m_parts;
    int order = 0;
    for (int l : labelsBackToFront) {
        if (Part *p = mutablePart(l))
            p->order = order++;
    }
    for (Part &p : m_parts) {
        if (!labelsBackToFront.contains(p.label))
            p.order = order++;
    }
    pushMaskChange(before, QRect(), QImage(), tr("Reorder parts"));
    emit partsChanged();
}

void PartSet::replaceParts(const QImage &labels, const QList<Part> &parts, const QString &undoText)
{
    if (labels.size() != m_labels.size())
        return;
    const QList<Part> before = m_parts;
    const QRect rect = m_labels.rect();
    const QImage labelsBefore = m_labels;
    m_labels = labels.convertToFormat(QImage::Format_Grayscale8).copy();
    m_parts = parts;
    pushMaskChange(before, rect, labelsBefore, undoText);
    updateCoverage();
    emit partsChanged();
    emit labelsChanged(rect);
}

QImage PartSet::partMask(int label, const QRect &rect) const
{
    return img::maskForLabel(m_labels, label, rect);
}

QImage PartSet::unassignedMask() const
{
    QImage out = img::createMask(m_labels.size(), 0);
    if (m_labels.isNull())
        return out;
    const bool all = m_silhouette.isNull() || m_silhouette.size() != m_labels.size();
    for (int y = 0; y < m_labels.height(); ++y) {
        const uchar *l = m_labels.constScanLine(y);
        const uchar *s = all ? nullptr : m_silhouette.constScanLine(y);
        uchar *o = out.scanLine(y);
        for (int x = 0; x < m_labels.width(); ++x)
            o[x] = (!l[x] && (!s || s[x] >= 128)) ? 255 : 0;
    }
    return out;
}

QImage PartSet::occluderMask(int label) const
{
    const Part *p = part(label);
    QImage out = img::createMask(m_labels.size(), 0);
    if (!p)
        return out;
    QVector<bool> front(256, false);
    for (const Part &o : m_parts)
        front[o.label] = o.label != label && o.order > p->order;
    for (int y = 0; y < m_labels.height(); ++y) {
        const uchar *l = m_labels.constScanLine(y);
        uchar *o = out.scanLine(y);
        for (int x = 0; x < m_labels.width(); ++x)
            o[x] = front[l[x]] ? 255 : 0;
    }
    return out;
}

void PartSet::applyLabels(const QRect &rectIn, const QImage &region, const QString &undoText)
{
    const QRect rect = rectIn.intersected(m_labels.rect());
    if (rect.isEmpty() || region.size() != rectIn.size())
        return;
    const QList<Part> before = m_parts;
    const QImage labelsBefore = m_labels.copy(rect);
    const QImage src = region.convertToFormat(QImage::Format_Grayscale8);
    const QPoint off = rect.topLeft() - rectIn.topLeft();
    for (int y = 0; y < rect.height(); ++y)
        memcpy(m_labels.scanLine(rect.y() + y) + rect.x(), src.constScanLine(off.y() + y) + off.x(), size_t(rect.width()));
    // Parts that gained pixels are now masked.
    for (Part &p : m_parts) {
        if (p.state == PartState::Empty)
            p.state = PartState::Masked;
    }
    pushMaskChange(before, rect, labelsBefore, undoText);
    updateCoverage();
    emit partsChanged();
    emit labelsChanged(rect);
}

void PartSet::notifyLabelsChanged(const QRect &rect)
{
    emit labelsChanged(rect);
}

void PartSet::commitLabelPaint(const QRect &rectIn, const QImage &before, const QString &undoText)
{
    const QRect rect = rectIn.intersected(m_labels.rect());
    if (rect.isEmpty())
        return;
    const QList<Part> partsBefore = m_parts;
    updateCoverage();
    for (Part &p : m_parts) {
        if (p.state == PartState::Empty && p.pixels > 0)
            p.state = PartState::Masked;
        else if (p.state == PartState::Masked && p.pixels == 0)
            p.state = PartState::Empty;
    }
    pushMaskChange(partsBefore, rect, before.copy(rect.translated(-rectIn.topLeft())), undoText);
    emit partsChanged();
}

QVector<bool> PartSet::lockedLabels() const
{
    QVector<bool> locked(256, false);
    for (const Part &p : m_parts)
        locked[p.label] = p.locked;
    return locked;
}

QImage PartSet::autoExtensionMask(int label) const
{
    const QImage own = partMask(label);
    const Part *p = part(label);
    if (!p)
        return own;
    const PartKind *k = taxonomy().find(p->tag);
    if (k && !k->occludable)
        return own;
    const qint64 area = p->pixels;
    QImage ext = img::extensionMask(own, occluderMask(label), img::adaptiveExtensionRadius(area));
    // The part never extends outside the character silhouette (grown slightly for anti-aliasing).
    if (!m_silhouette.isNull() && m_silhouette.size() == ext.size())
        ext = img::combineMasks(ext, img::dilateMask(m_silhouette, 2), img::MaskOp::Intersect);
    return ext;
}

QImage PartSet::extensionMask(int label) const
{
    const Part *p = part(label);
    if (!p)
        return img::createMask(m_labels.size(), 0);
    if (p->extensionAuto || p->extension.isNull())
        return autoExtensionMask(label);
    QImage full = img::createMask(m_labels.size(), 0);
    QPainter painter(&full);
    painter.drawImage(p->extensionRect.topLeft(), p->extension);
    painter.end();
    // The visible pixels always belong to the part.
    return img::combineMasks(full, partMask(label), img::MaskOp::Union);
}

void PartSet::setExtension(int label, const QImage &fullMask, const QString &undoText)
{
    if (!part(label))
        return;
    const QList<Part> before = m_parts;
    Part *p = mutablePart(label);
    const QImage mask = fullMask.convertToFormat(QImage::Format_Grayscale8);
    const QRect b = img::maskBounds(mask, 1);
    p->extensionAuto = false;
    p->extensionRect = b;
    p->extension = b.isEmpty() ? img::createMask(QSize(1, 1), 0) : mask.copy(b);
    if (b.isEmpty())
        p->extensionRect = QRect(0, 0, 1, 1);
    pushMaskChange(before, QRect(), QImage(), undoText);
    emit partsChanged();
    emit extensionChanged(label);
}

void PartSet::resetExtension(int label)
{
    const Part *current = part(label);
    if (!current || current->extensionAuto)
        return;
    const QList<Part> before = m_parts;
    Part *p = mutablePart(label);
    p->extensionAuto = true;
    p->extension = QImage();
    p->extensionRect = QRect();
    pushMaskChange(before, QRect(), QImage(), tr("Reset extension area"));
    emit partsChanged();
    emit extensionChanged(label);
}

void PartSet::setPartState(int label, PartState state, const QString &error)
{
    Part *p = mutablePart(label);
    if (!p || (p->state == state && p->error == error))
        return;
    p->state = state;
    p->error = error;
    emit partStateChanged(label);
    touch();
}

void PartSet::addVariant(int label, const QString &assetId, const QRect &rect, bool choose)
{
    Part *p = mutablePart(label);
    if (!p)
        return;
    p->variants.append(assetId);
    p->variantRects.append(rect);
    if (choose || p->chosen < 0)
        p->chosen = int(p->variants.size()) - 1;
    p->state = PartState::Ready;
    p->error.clear();
    emit resultChanged(label);
    emit partStateChanged(label);
    touch();
}

void PartSet::chooseVariant(int label, int index)
{
    Part *p = mutablePart(label);
    if (!p || index < 0 || index >= p->variants.size() || p->chosen == index)
        return;
    p->chosen = index;
    emit resultChanged(label);
    touch();
}

void PartSet::removeVariant(int label, int index)
{
    Part *p = mutablePart(label);
    if (!p || index < 0 || index >= p->variants.size())
        return;
    m_results.remove(p->variants[index]);
    p->variants.removeAt(index);
    p->variantRects.removeAt(index);
    if (p->chosen >= p->variants.size())
        p->chosen = int(p->variants.size()) - 1;
    if (p->variants.isEmpty() && (p->state == PartState::Ready || p->state == PartState::Approved))
        p->state = p->pixels > 0 ? PartState::Masked : PartState::Empty;
    emit resultChanged(label);
    emit partStateChanged(label);
    touch();
}

QImage PartSet::resultImage(int label) const
{
    const Part *p = part(label);
    if (!p || !p->hasResult())
        return {};
    return m_results.value(p->chosenAssetId());
}

void PartSet::setResultImage(const QString &assetId, const QImage &image)
{
    m_results.insert(assetId, image);
    for (const Part &p : std::as_const(m_parts)) {
        if (p.chosenAssetId() == assetId)
            emit resultChanged(p.label);
    }
}

void PartSet::updateCoverage()
{
    if (m_labels.isNull()) {
        for (Part &p : m_parts)
            p.pixels = 0;
        return;
    }
    const QVector<qint64> hist = img::labelHistogram(m_labels);
    for (Part &p : m_parts)
        p.pixels = hist.value(p.label);
}

void PartSet::touch()
{
    if (m_project)
        saveSoon();
}

// ---- persistence --------------------------------------------------------------------------------

PartSet::Snapshot PartSet::snapshot() const
{
    Snapshot s;
    s.name = m_name;
    s.sourceAssetId = m_sourceAssetId;
    s.taxonomy = m_taxonomy;
    s.step = m_step;
    s.settings = m_settings;
    s.decomposition = m_decomposition;
    s.labels = m_labels;
    s.parts = m_parts;
    return s;
}

QJsonObject PartSet::encode(const Snapshot &s)
{
    QJsonArray parts;
    for (const Part &p : s.parts)
        parts.append(partToJson(p));
    return QJsonObject{{QStringLiteral("version"), 1},
                       {QStringLiteral("name"), s.name},
                       {QStringLiteral("source"), s.sourceAssetId},
                       {QStringLiteral("taxonomy"), s.taxonomy},
                       {QStringLiteral("step"), s.step},
                       {QStringLiteral("settings"), s.settings},
                       {QStringLiteral("decomposition"), s.decomposition},
                       {QStringLiteral("labels"), s.labels.isNull() ? QString()
                                                                    : QString::fromLatin1(img::toBase64(s.labels, "PNG", 6))},
                       {QStringLiteral("parts"), parts}};
}

PartSet::Snapshot PartSet::decode(const QJsonObject &o)
{
    Snapshot s;
    s.name = o.value(QStringLiteral("name")).toString();
    s.sourceAssetId = o.value(QStringLiteral("source")).toString();
    s.taxonomy = o.value(QStringLiteral("taxonomy")).toString();
    s.step = o.value(QStringLiteral("step")).toInt();
    s.settings = o.value(QStringLiteral("settings")).toObject();
    s.decomposition = o.value(QStringLiteral("decomposition")).toObject();
    const QString labels = o.value(QStringLiteral("labels")).toString();
    if (!labels.isEmpty()) {
        s.labels = img::fromBase64(labels.toLatin1());
        if (!s.labels.isNull())
            s.labels = s.labels.convertToFormat(QImage::Format_Grayscale8);
    }
    for (const QJsonValue &v : o.value(QStringLiteral("parts")).toArray())
        s.parts.append(partFromJson(v.toObject()));
    return s;
}

void PartSet::restore(const QString &id, const Snapshot &s, const QImage &source, const QImage &silhouette)
{
    m_saveTimer.stop();
    m_id = id.isEmpty() ? newUuid() : id;
    m_name = s.name;
    m_sourceAssetId = s.sourceAssetId;
    m_taxonomy = partTaxonomy(s.taxonomy).id;
    m_step = s.step;
    m_settings = s.settings;
    m_decomposition = s.decomposition;
    m_source = img::normalized(source);
    m_silhouette = silhouette.isNull() ? QImage() : silhouette.convertToFormat(QImage::Format_Grayscale8);
    m_labels = (s.labels.size() == m_source.size()) ? s.labels : img::createMask(m_source.size(), 0);
    m_parts = s.parts;
    if (s.labels.size() != m_source.size()) {
        for (Part &p : m_parts)
            p.state = PartState::Empty;
    }
    m_results.clear();
    m_undo.clear();
    updateCoverage();
    emit taxonomyChanged();
    emit sourceChanged();
    emit partsChanged();
    emit labelsChanged(m_labels.rect());
    emit stepChanged(m_step);
    m_saveTimer.stop(); // restoring is not a modification
}

void PartSet::bind(Project *project)
{
    m_project = project;
}

void PartSet::saveSoon()
{
    m_saveTimer.start();
}

QFuture<void> PartSet::saveNow()
{
    m_saveTimer.stop();
    if (!m_project || !m_project->isOpen() || !hasSource())
        return QtFuture::makeReadyVoidFuture();
    if (m_saving) {
        m_savePending = true;
        return QtFuture::makeReadyVoidFuture();
    }
    m_saving = true;
    const Snapshot snap = snapshot();
    const QString id = m_id;
    const QString name = m_name.isEmpty() ? tr("Untitled parts") : m_name;
    const QImage source = m_source;
    QVector<QRgb> palette(256, 0);
    for (const Part &p : m_parts) {
        QColor c = p.color;
        c.setAlpha(150);
        palette[p.label] = c.rgba();
    }
    QPointer<PartSet> self(this);
    QPointer<Project> project(m_project);
    return async::run([snap, source, palette]() {
               // Thumbnail: source with the part overlay.
               QImage thumb = img::thumbnail(source, Project::kThumbnailSize);
               if (!snap.labels.isNull() && !thumb.isNull()) {
                   QImage overlay = img::colorizeLabels(snap.labels, palette, snap.labels.rect())
                                        .scaled(thumb.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation);
                   QImage t = thumb.convertToFormat(QImage::Format_ARGB32_Premultiplied);
                   QPainter p(&t);
                   p.setOpacity(0.6);
                   p.drawImage(0, 0, overlay);
                   p.end();
                   thumb = t;
               }
               return std::make_pair(encode(snap), thumb);
           })
        .then(this, [self, project, id, name](std::pair<QJsonObject, QImage> encoded) {
            if (!project || !project->isOpen())
                return QtFuture::makeReadyVoidFuture();
            Record r;
            r.id = id;
            r.type = QString::fromLatin1(kRecordType);
            r.name = name;
            r.json = encoded.first;
            r.thumbnail = encoded.second;
            return project->saveRecord(r).then([](Record) {});
        })
        .unwrap()
        .then(this, [self]() {
            if (!self)
                return;
            self->m_saving = false;
            emit self->saved();
            if (self->m_savePending) {
                self->m_savePending = false;
                self->saveNow();
            }
        })
        .onFailed(this, [self](const std::exception &e) {
            qCWarning(lcDoc) << "Saving part set failed:" << e.what();
            if (self) {
                self->m_saving = false;
                if (self->m_savePending) {
                    self->m_savePending = false;
                    self->saveSoon();
                }
            }
        });
}

} // namespace lumen::doc
