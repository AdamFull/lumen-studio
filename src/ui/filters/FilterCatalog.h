#pragma once

#include "filters/Evaluator.h"
#include "filters/Graph.h"

#include <QDateTime>
#include <QFileSystemWatcher>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QTimer>

#include <memory>
#include <optional>

namespace lumen::ui {

class Studio;

// The filters the Filters panel offers (M17 §17.6): built-in ones, read-only resources
// (resources/filters, ":/filters/builtin/…"), and saved ones, .lumenfilter files in the filters
// folder next to the models folder (paths::defaultFilters()). The filter editor saves through the
// catalog, so every panel that shows it refreshes; files added to the folder by hand appear too.
// Names of saved filters made through it are unique, so the list can tell them apart.
class FilterCatalog : public QObject
{
    Q_OBJECT
public:
    explicit FilterCatalog(const QString &folder, QObject *parent = nullptr);
    ~FilterCatalog() override;

    // The catalog of the app on the standard folder: one per Studio (its child), made on first use.
    static FilterCatalog *of(Studio *studio);

    struct Entry
    {
        QString path; // ":/filters/builtin/…" for a built-in filter
        QString name; // a built-in one's translated
        QString description;
        bool builtIn = false;
        bool usesAi = false; // has a step that previews only after Run
        // Why the filter cannot be used (a newer version, a node of a plugin that is off), in plain
        // words; empty when it loads.
        QString error;
        QDateTime modified;
    };

    QString folder() const { return m_folder; }
    // The built-in filters first, in their order, then the saved ones by name.
    QList<Entry> entries() const { return m_entries; }
    std::optional<Entry> entry(const QString &path) const;
    // A filter's graph; a built-in one's name, description and slider labels translated.
    std::optional<filters::Graph> load(const QString &path, QString *error = nullptr) const;
    static bool isBuiltIn(const QString &path);

    // Saves to `path` (a saved filter) or, with an empty or built-in path, to a new file in the
    // folder named after the filter. Returns the path, or empty with the error.
    QString save(const filters::Graph &graph, const QString &path = {}, QString *error = nullptr);
    // A copy in the folder named "<name> copy" ("<name> copy 2"… when taken; the way to edit a
    // built-in filter). Returns its path.
    QString duplicate(const QString &path, QString *error = nullptr);
    // A saved filter that is still an unchanged copy of the filter at `path` (made by duplicate()
    // and not changed since): "Edit a copy…" opens it rather than making another. Empty when none.
    QString unchangedCopyOf(const QString &path) const;
    // A new filter (the input linked to the output) named `name` (with a number when taken).
    // Returns its path.
    QString create(const QString &name, QString *error = nullptr);
    // `name`, or "<name> 2", "<name> 3"… when a saved filter other than `except` has it already.
    QString uniqueName(const QString &name, const QString &except = {}) const;
    // Deletes a saved filter's file; built-in filters cannot be deleted.
    bool remove(const QString &path, QString *error = nullptr);
    // Creates the folder when it is not there yet (before it is shown in the file manager).
    bool ensureFolder(QString *error = nullptr);

    // A small picture of the filter applied to a sample image. Null until it is computed
    // (thumbnailReady), and for filters with AI steps or errors, which the panel shows as icons.
    QImage thumbnail(const QString &path);
    static QSize thumbnailSize() { return {64, 48}; }

    // Re-reads the folder (also done by itself when files change there).
    void refresh();

    // The results of AI steps that the filter editors and the Filters panels of this catalog share:
    // a step run in the editor is not run again when the filter is applied from the panel.
    const std::shared_ptr<filters::SharedResults> &sharedResults() const { return m_sharedResults; }

signals:
    void changed();
    // A filter was saved through the catalog (the node editor): its settings may have changed.
    void saved(const QString &path);
    void thumbnailReady(const QString &path);

private:
    void watchFolder();
    void scheduleRefresh();
    void startThumbnail(const Entry &e);
    // "<name> copy", before it is made unique.
    static QString copyName(const filters::Graph &graph, const QString &path);

    QString m_folder;
    QList<Entry> m_entries;
    QFileSystemWatcher m_watcher;
    QTimer m_refreshTimer;
    // Path -> picture, kept while the file's time stamp is the same.
    struct Thumb
    {
        QDateTime modified;
        QImage image;
        bool pending = false;
    };
    QHash<QString, Thumb> m_thumbs;
    std::unique_ptr<filters::Evaluator> m_thumbEvaluator;
    std::shared_ptr<filters::SharedResults> m_sharedResults = std::make_shared<filters::SharedResults>();
    QImage m_sample;
    // False once the destructor runs: continuations of pending thumbnails run (cancelled) while
    // QObject's destructor runs, after the members are gone.
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

// The picture filter thumbnails and the node editor show when no document is open: sky, sun,
// hills and a house, with colours, edges and soft gradients for every kind of filter to change.
QImage filterSampleImage(const QSize &size = QSize(640, 480));

} // namespace lumen::ui
