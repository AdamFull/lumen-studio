#pragma once

#include <QColor>
#include <QFuture>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUndoStack>

#include <memory>

namespace lumen::doc {

class Project;

// ---- Taxonomy -----------------------------------------------------------------------------------

struct PartKind
{
    QString tag;           // stable id (see-through tag spelling for the Live2D taxonomy)
    QString name;          // display name
    QString group;         // "head", "body" or "extra"
    int order = 0;         // default draw order, higher = in front
    QColor color;          // overlay colour
    bool occludable = false; // usually continues behind other parts (gets an extension area)
    bool core = true;      // created by auto segmentation; false = available in "Add part" / splits
    double depth = 0.5;    // preview parallax depth: 0 = farthest back, 1 = nearest
    QString description;   // completion prompt fragment
    QString segmentPrompt; // text prompt for text-guided segmenters
    QStringList aliases;   // labels returned by segmenters that map to this kind
};

struct PartTaxonomy
{
    QString id;
    QString name;
    QString description;
    QList<PartKind> kinds;

    const PartKind *find(const QString &tagOrAlias) const;
    QStringList groups() const;
};

const QList<PartTaxonomy> &partTaxonomies();
const PartTaxonomy &partTaxonomy(const QString &id); // unknown ids fall back to the first taxonomy
QString partGroupLabel(const QString &group);

// ---- Parts --------------------------------------------------------------------------------------

enum class PartState { Empty, Masked, Queued, Generating, Ready, Approved, Failed };
QString partStateName(PartState state);
QString partStateLabel(PartState state);
PartState partStateFromName(const QString &name);

struct Part
{
    QString id;            // uuid, stable across label changes
    int label = 0;         // 1..254 value in the label map
    QString name;
    QString tag;           // taxonomy kind ("" = custom)
    QString side;          // "", "L" or "R" (character's point of view)
    QColor color;
    int order = 0;         // draw order, higher = in front
    bool visible = true;   // overlay / preview visibility
    bool locked = false;   // protected from painting by other parts
    QString prompt;        // user override of the completion prompt
    QString provenance;    // "auto:<segmenter>", "manual", "split", "merge"
    // Extension area (where the part may continue behind others). Cropped Grayscale8.
    bool extensionAuto = true;
    QImage extension;
    QRect extensionRect;
    // Generation state (not part of mask undo history).
    PartState state = PartState::Empty;
    QString error;
    QStringList variants;  // result asset ids
    QList<QRect> variantRects;
    int chosen = -1;
    // Derived.
    qint64 pixels = 0;

    QString chosenAssetId() const { return chosen >= 0 && chosen < variants.size() ? variants[chosen] : QString(); }
    QRect chosenRect() const { return chosen >= 0 && chosen < variantRects.size() ? variantRects[chosen] : QRect(); }
    bool hasResult() const { return !chosenAssetId().isEmpty(); }
    QString displayName() const;
};

// A slicing session: source image, taxonomy, label map (exclusive part masks), part metadata,
// extension areas and generation results. GUI-thread object with its own undo stack; persisted
// as a project record of type "parts".
class PartSet : public QObject
{
    Q_OBJECT
public:
    static constexpr const char *kRecordType = "parts";
    static constexpr int kMaxParts = 254;

    explicit PartSet(QObject *parent = nullptr);
    ~PartSet() override;

    // ---- identity / source --------------------------------------------------------------------
    QString id() const { return m_id; }
    QString name() const { return m_name; }
    void setName(const QString &name);
    QString sourceAssetId() const { return m_sourceAssetId; }
    const QImage &source() const { return m_source; }       // ARGB32_Premultiplied
    const QImage &silhouette() const { return m_silhouette; } // Grayscale8 character area
    QSize size() const { return m_source.size(); }
    bool hasSource() const { return !m_source.isNull(); }
    // Replaces the source. Masks are kept when the size matches, otherwise cleared.
    void setSource(const QImage &image, const QString &assetId, const QImage &silhouette = QImage());
    void setSilhouette(const QImage &mask);

    QString taxonomyId() const { return m_taxonomy; }
    const PartTaxonomy &taxonomy() const { return partTaxonomy(m_taxonomy); }
    void setTaxonomy(const QString &id);
    int step() const { return m_step; }
    void setStep(int step);
    QJsonObject settings() const { return m_settings; }
    void setSettings(const QJsonObject &settings);
    // Raw layers of a model decomposition (e.g. see-through), tag -> {asset, depth, order}.
    QJsonObject decomposition() const { return m_decomposition; }
    void setDecomposition(const QJsonObject &d);

    // ---- parts --------------------------------------------------------------------------------
    const QList<Part> &parts() const { return m_parts; }
    const Part *part(int label) const;
    const Part *partById(const QString &id) const;
    // Parts sorted by draw order, back to front.
    QList<const Part *> partsInDrawOrder() const;
    int freeLabel() const; // 0 when full
    QColor suggestColor(const QString &tag) const;
    Part makePart(const QString &tag, const QString &name = QString(), const QString &side = QString()) const;

    int addPart(const QString &tag, const QString &name = QString(), const QString &side = QString(),
                const QImage &mask = QImage());
    void removeParts(const QList<int> &labels);
    // Merges every part into the first one.
    void mergeParts(const QList<int> &labels);
    // pieces[0] stays with the part; every further piece becomes a new part named by `names`.
    // Returns the labels of all pieces (the original first).
    // `tags` (optional) assigns taxonomy kinds to the pieces (draw order and colour follow the kind).
    QList<int> splitPart(int label, const QList<QImage> &pieces, const QStringList &names = {},
                         const QStringList &sides = {}, const QString &undoText = QString(), const QStringList &tags = {});
    // Metadata edit (name, tag, side, colour, order, visible, locked, prompt).
    void updatePart(const Part &part, const QString &undoText = QString());
    // Draw order from a list of labels, back to front.
    void reorder(const QList<int> &labelsBackToFront);
    // Replaces every part and the whole label map (auto segmentation) as one undo step.
    void replaceParts(const QImage &labels, const QList<Part> &parts, const QString &undoText);

    // ---- label map ----------------------------------------------------------------------------
    const QImage &labels() const { return m_labels; }
    QImage partMask(int label, const QRect &rect = QRect()) const;
    QImage unassignedMask() const; // silhouette pixels without a part
    // Union of the parts drawn in front of `label`.
    QImage occluderMask(int label) const;
    // Replaces a region of the label map with `region` as one undo step.
    void applyLabels(const QRect &rect, const QImage &region, const QString &undoText);
    // Direct painting: call labelsForPainting(), modify, notifyLabelsChanged() during the stroke,
    // then commitLabelPaint() with the region contents before the stroke.
    QImage &labelsForPainting() { return m_labels; }
    void notifyLabelsChanged(const QRect &rect);
    void commitLabelPaint(const QRect &rect, const QImage &before, const QString &undoText);
    // Lookup table of locked labels (for painting tools).
    QVector<bool> lockedLabels() const;

    // ---- extension areas ----------------------------------------------------------------------
    // Explicit extension, or the automatic one (own ∪ dilated own ∩ occluders) when auto.
    QImage extensionMask(int label) const;
    QImage autoExtensionMask(int label) const;
    void setExtension(int label, const QImage &fullMask, const QString &undoText);
    void resetExtension(int label);

    // ---- generation state (no undo) -----------------------------------------------------------
    void setPartState(int label, PartState state, const QString &error = QString());
    void addVariant(int label, const QString &assetId, const QRect &rect, bool choose = true);
    void chooseVariant(int label, int index);
    void removeVariant(int label, int index);
    // Decoded image of the chosen result (cached; the UI loads it from the project).
    QImage resultImage(int label) const;
    void setResultImage(const QString &assetId, const QImage &image);
    bool hasResultImage(const QString &assetId) const { return m_results.contains(assetId); }

    QUndoStack *undoStack() { return &m_undo; }

    // ---- persistence --------------------------------------------------------------------------
    struct Snapshot
    {
        QString name;
        QString sourceAssetId;
        QString taxonomy;
        int step = 0;
        QJsonObject settings;
        QJsonObject decomposition;
        QImage labels;
        QList<Part> parts;
    };
    Snapshot snapshot() const;
    // Heavy (PNG encoding); safe on worker threads.
    static QJsonObject encode(const Snapshot &snapshot);
    static Snapshot decode(const QJsonObject &json);
    // Restores a decoded snapshot (source image supplied separately). Clears undo history.
    void restore(const QString &id, const Snapshot &snapshot, const QImage &source, const QImage &silhouette);

    // Autosave into the project (debounced, serialised, non-blocking).
    void bind(Project *project);
    void saveSoon();
    QFuture<void> saveNow();

    // Thread-safe variants working on a snapshot (for worker threads).
    static QImage occluderMaskFor(const Snapshot &snapshot, int label);
    static QImage autoExtensionMaskFor(const Snapshot &snapshot, const QImage &silhouette, int label);
    static QImage extensionMaskFor(const Snapshot &snapshot, const QImage &silhouette, int label);

    // Undo machinery (used by the commands).
    struct MaskState
    {
        QList<Part> parts;
        QRect rect;
        QImage labels; // region contents (null = unchanged)
    };
    void applyMaskState(const MaskState &state);

signals:
    void sourceChanged();
    void partsChanged();                 // list/metadata changed
    void labelsChanged(const QRect &rect);
    void partStateChanged(int label);
    void resultChanged(int label);
    void extensionChanged(int label);
    void stepChanged(int step);
    void taxonomyChanged();
    void saved();

private:
    Part *mutablePart(int label);
    void pushMaskChange(const QList<Part> &before, const QRect &rect, const QImage &labelsBefore, const QString &text);
    void updateCoverage();
    void touch();

    QString m_id;
    QString m_name;
    QString m_sourceAssetId;
    QImage m_source;
    QImage m_silhouette;
    QString m_taxonomy;
    int m_step = 0;
    QJsonObject m_settings;
    QJsonObject m_decomposition;
    QImage m_labels;
    QList<Part> m_parts;
    QUndoStack m_undo;
    QHash<QString, QImage> m_results;
    QPointer<Project> m_project;
    QTimer m_saveTimer;
    bool m_saving = false;
    bool m_savePending = false;
};

} // namespace lumen::doc
