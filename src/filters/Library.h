#pragma once

#include "filters/Graph.h"

#include <QDateTime>
#include <QList>
#include <QString>

#include <optional>

namespace lumen::filters {

// The saved filters of one folder (*.lumenfilter). The folder is given by the caller: the app
// keeps it next to the models folder (a later step chooses it).
class Library
{
public:
    explicit Library(QString folder = {});

    QString folder() const { return m_folder; }
    void setFolder(const QString &folder) { m_folder = folder; }

    struct Entry
    {
        QString path;
        QString name; // the filter's name; the file name when it has none
        QString description;
        QDateTime modified;
        // Why the filter cannot be used (made with a newer version, a node of a plugin that is
        // off, not a filter file), in plain words; empty when it loads.
        QString error;
    };
    // Every filter file of the folder, by name. Files that do not load are listed with their error.
    QList<Entry> entries(const NodeRegistry &registry = NodeRegistry::global()) const;

    std::optional<Graph> load(const QString &path, const NodeRegistry &registry = NodeRegistry::global(),
                              QString *error = nullptr) const;
    // Saves to `path`, or to a new file named after the filter in the folder; returns the path
    // (empty on failure, with the error).
    QString save(const Graph &graph, const QString &path = {}, QString *error = nullptr) const;
    // A file name for a new filter: its name without characters files cannot have, made unique.
    QString suggestedPath(const QString &name) const;

private:
    QString m_folder;
};

} // namespace lumen::filters
