#include "filters/Library.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>

namespace lumen::filters {

namespace {

struct Text
{
    Q_DECLARE_TR_FUNCTIONS(lumen::filters::Library)
};

} // namespace

Library::Library(QString folder)
    : m_folder(std::move(folder))
{
}

QList<Library::Entry> Library::entries(const NodeRegistry &registry) const
{
    QList<Entry> out;
    if (m_folder.isEmpty())
        return out;
    const QFileInfoList files = QDir(m_folder).entryInfoList({QStringLiteral("*") + Graph::fileExtension()}, QDir::Files);
    for (const QFileInfo &file : files) {
        Entry e;
        e.path = file.absoluteFilePath();
        e.modified = file.lastModified();
        e.name = file.completeBaseName();
        QString error;
        if (const std::optional<Graph> graph = Graph::load(e.path, registry, &error)) {
            if (!graph->name.isEmpty())
                e.name = graph->name;
            e.description = graph->description;
        } else {
            e.error = error;
        }
        out.append(e);
    }
    std::sort(out.begin(), out.end(), [](const Entry &a, const Entry &b) {
        return QString::localeAwareCompare(a.name, b.name) < 0;
    });
    return out;
}

std::optional<Graph> Library::load(const QString &path, const NodeRegistry &registry, QString *error) const
{
    return Graph::load(path, registry, error);
}

QString Library::suggestedPath(const QString &name) const
{
    QString base = name;
    base.replace(QRegularExpression(QStringLiteral(R"([<>:"/\\|?*\x00-\x1f])")), QStringLiteral(" "));
    base = base.simplified();
    while (base.endsWith(QLatin1Char('.')))
        base.chop(1);
    if (base.isEmpty())
        base = Text::tr("Filter");
    const QDir dir(m_folder);
    QString path = dir.filePath(base + Graph::fileExtension());
    for (int i = 2; QFileInfo::exists(path); ++i)
        path = dir.filePath(QStringLiteral("%1 %2%3").arg(base).arg(i).arg(Graph::fileExtension()));
    return path;
}

QString Library::save(const Graph &graph, const QString &path, QString *error) const
{
    const QString target = path.isEmpty() ? suggestedPath(graph.name) : path;
    if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
        if (error)
            *error = Text::tr("Could not create the folder for filters: %1").arg(QFileInfo(target).absolutePath());
        return {};
    }
    return graph.save(target, error) ? target : QString();
}

} // namespace lumen::filters
