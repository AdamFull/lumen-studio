#include "ui/filters/FilterCatalog.h"

#include "core/Error.h"
#include "core/Paths.h"
#include "filters/Library.h"
#include "ui/Studio.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>

#include <algorithm>

namespace lumen::ui {

namespace {

const QString kBuiltInRoot = QStringLiteral(":/filters/builtin");
const char *const kBuiltInContext = "lumen::ui::BuiltInFilters";

// The order the built-in filters are listed in (file names without the extension): the classic
// ones, then those with AI steps (the Filters panel gives them a heading of their own). Files
// there that are not named here (added later) follow, by name.
const QStringList &builtInOrder()
{
    static const QStringList order{QStringLiteral("black-and-white"),     QStringLiteral("warm-tones"),
                                   QStringLiteral("cool-tones"),          QStringLiteral("sharpen"),
                                   QStringLiteral("soft-focus"),          QStringLiteral("film-grain"),
                                   QStringLiteral("old-photo"),           QStringLiteral("colorize-with-ai"),
                                   QStringLiteral("clean-up-background"), QStringLiteral("remove-background"),
                                   QStringLiteral("upscale-2x")};
    return order;
}

// The texts of the built-in filter files, for the translation files; the files hold the English.
[[maybe_unused]] const char *const kBuiltInTexts[] = {
    QT_TRANSLATE_NOOP("lumen::ui::BuiltInFilters", "Black and white"),
    QT_TRANSLATE_NOOP("lumen::ui::BuiltInFilters", "Removes the colours and keeps the brightness."),
    QT_TRANSLATE_NOOP("lumen::ui::BuiltInFilters", "Brightness from"),
    QT_TRANSLATE_NOOP("lumen::ui::BuiltInFilters", "Warm tones"),
    QT_TRANSLATE_NOOP("lumen::ui::BuiltInFilters", "Makes the colours warmer, like late afternoon light."),
    QT_TRANSLATE_NOOP("lumen::ui::BuiltInFilters", "Cool tones"),
    QT_TRANSLATE_NOOP("lumen::ui::BuiltInFilters", "Makes the colours cooler, like early morning light."),
    QT_TRANSLATE_NOOP("lumen::ui::BuiltInFilters", "Strength"),
    QT_TRANSLATE_NOOP("lumen::ui::BuiltInFilters", "Sharpen"),
    QT_TRANSLATE_NOOP("lumen::ui::BuiltInFilters", "Makes edges and fine detail crisper."),
    QT_TRANSLATE_NOOP("lumen::ui::BuiltInFilters", "Amount"),
    QT_TRANSLATE_NOOP("lumen::ui::BuiltInFilters", "Edge width"),
    QT_TRANSLATE_NOOP("lumen::ui::BuiltInFilters", "Soft focus"),
    QT_TRANSLATE_NOOP("lumen::ui::BuiltInFilters", "A soft, dreamy glow over the picture."),
    QT_TRANSLATE_NOOP("lumen::ui::BuiltInFilters", "Softness"),
    QT_TRANSLATE_NOOP("lumen::ui::BuiltInFilters", "Film grain"),
    QT_TRANSLATE_NOOP("lumen::ui::BuiltInFilters", "Adds the grain of photographic film."),
    QT_TRANSLATE_NOOP("lumen::ui::BuiltInFilters", "Grain"),
    QT_TRANSLATE_NOOP("lumen::ui::BuiltInFilters", "Old photo"),
    QT_TRANSLATE_NOOP("lumen::ui::BuiltInFilters", "Brownish black and white with a little grain, like an old print."),
    QT_TRANSLATE_NOOP("lumen::ui::BuiltInFilters", "Brown tint"),
    // With AI steps. The instructions sent to the model stay in English, which the models follow
    // best; only what the panel shows is translated.
    QT_TRANSLATE_NOOP("lumen::ui::BuiltInFilters", "Colorize with AI"),
    QT_TRANSLATE_NOOP("lumen::ui::BuiltInFilters", "Gives a black-and-white photo natural colours and keeps everything else as it is. "
                                                   "With a selection, only the selected area is coloured."),
    QT_TRANSLATE_NOOP("lumen::ui::BuiltInFilters", "Instruction"),
    QT_TRANSLATE_NOOP("lumen::ui::BuiltInFilters", "Clean up background"),
    QT_TRANSLATE_NOOP("lumen::ui::BuiltInFilters", "Tidies the background (clutter, stray objects, spots) and keeps the main subject "
                                                   "as it is. Select the background to change only that."),
    QT_TRANSLATE_NOOP("lumen::ui::BuiltInFilters", "Remove background"),
    QT_TRANSLATE_NOOP("lumen::ui::BuiltInFilters", "Cuts out the main subject: the background becomes transparent."),
    QT_TRANSLATE_NOOP("lumen::ui::BuiltInFilters", "Upscale ×2"),
    QT_TRANSLATE_NOOP("lumen::ui::BuiltInFilters", "Makes the picture twice as large with an AI upscaler, which adds detail. With "
                                                   "nothing selected, the whole picture grows."),
};

QString translated(const QString &text)
{
    return text.isEmpty() ? text : QCoreApplication::translate(kBuiltInContext, text.toUtf8().constData());
}

QStringList builtInPaths()
{
    QStringList files = QDir(kBuiltInRoot).entryList({QStringLiteral("*") + filters::Graph::fileExtension()}, QDir::Files,
                                                     QDir::Name);
    auto rank = [](const QString &file) {
        const qsizetype i = builtInOrder().indexOf(QFileInfo(file).completeBaseName());
        return i < 0 ? builtInOrder().size() : i;
    };
    std::stable_sort(files.begin(), files.end(), [&](const QString &a, const QString &b) { return rank(a) < rank(b); });
    QStringList out;
    for (const QString &f : files)
        out << kBuiltInRoot + QLatin1Char('/') + f;
    return out;
}

bool sameEntries(const QList<FilterCatalog::Entry> &a, const QList<FilterCatalog::Entry> &b)
{
    if (a.size() != b.size())
        return false;
    for (qsizetype i = 0; i < a.size(); ++i) {
        const FilterCatalog::Entry &x = a[i], &y = b[i];
        if (x.path != y.path || x.name != y.name || x.description != y.description || x.error != y.error
            || x.modified != y.modified || x.usesAi != y.usesAi)
            return false;
    }
    return true;
}

} // namespace

QImage filterSampleImage(const QSize &size)
{
    QImage im(size, QImage::Format_ARGB32_Premultiplied);
    const double w = size.width(), h = size.height();
    QPainter p(&im);
    p.setRenderHint(QPainter::Antialiasing);
    QLinearGradient sky(0, 0, 0, h * 0.65);
    sky.setColorAt(0, QColor(46, 104, 178));
    sky.setColorAt(1, QColor(250, 196, 140));
    p.fillRect(im.rect(), sky);
    QRadialGradient sun(QPointF(w * 0.74, h * 0.28), h * 0.16);
    sun.setColorAt(0, QColor(255, 246, 200));
    sun.setColorAt(0.55, QColor(255, 214, 92));
    sun.setColorAt(1, QColor(255, 214, 92, 0));
    p.setPen(Qt::NoPen);
    p.setBrush(sun);
    p.drawEllipse(QPointF(w * 0.74, h * 0.28), h * 0.16, h * 0.16);
    QPainterPath far;
    far.moveTo(0, h * 0.62);
    far.cubicTo(w * 0.25, h * 0.48, w * 0.45, h * 0.66, w * 0.7, h * 0.55);
    far.cubicTo(w * 0.85, h * 0.5, w * 0.95, h * 0.58, w, h * 0.56);
    far.lineTo(w, h);
    far.lineTo(0, h);
    p.setBrush(QColor(74, 128, 92));
    p.drawPath(far);
    QPainterPath near;
    near.moveTo(0, h * 0.78);
    near.cubicTo(w * 0.3, h * 0.68, w * 0.6, h * 0.86, w, h * 0.74);
    near.lineTo(w, h);
    near.lineTo(0, h);
    QLinearGradient grass(0, h * 0.7, 0, h);
    grass.setColorAt(0, QColor(118, 170, 64));
    grass.setColorAt(1, QColor(52, 96, 38));
    p.setBrush(grass);
    p.drawPath(near);
    // A house: hard edges, a saturated red and a white window.
    const QRectF wall(w * 0.18, h * 0.5, w * 0.2, h * 0.2);
    p.setBrush(QColor(196, 54, 44));
    p.drawRect(wall);
    QPolygonF roof{QPointF(wall.left() - w * 0.02, wall.top()), QPointF(wall.center().x(), wall.top() - h * 0.12),
                   QPointF(wall.right() + w * 0.02, wall.top())};
    p.setBrush(QColor(60, 44, 40));
    p.drawPolygon(roof);
    p.setBrush(QColor(246, 242, 228));
    p.drawRect(QRectF(wall.left() + wall.width() * 0.15, wall.top() + wall.height() * 0.25, wall.width() * 0.25,
                      wall.height() * 0.3));
    p.setBrush(QColor(92, 62, 38));
    p.drawRect(QRectF(wall.left() + wall.width() * 0.58, wall.top() + wall.height() * 0.4, wall.width() * 0.22,
                      wall.height() * 0.6));
    return im;
}

FilterCatalog::FilterCatalog(const QString &folder, QObject *parent)
    : QObject(parent)
    , m_folder(QDir::cleanPath(folder))
    , m_thumbEvaluator(std::make_unique<filters::Evaluator>())
{
    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(150);
    connect(&m_refreshTimer, &QTimer::timeout, this, &FilterCatalog::refresh);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &FilterCatalog::scheduleRefresh);
    refresh();
}

FilterCatalog::~FilterCatalog()
{
    *m_alive = false;
}

FilterCatalog *FilterCatalog::of(Studio *studio)
{
    static const QString name = QStringLiteral("lumen-filter-catalog");
    auto *catalog = studio->findChild<FilterCatalog *>(name, Qt::FindDirectChildrenOnly);
    if (!catalog) {
        catalog = new FilterCatalog(paths::defaultFilters(), studio);
        catalog->setObjectName(name);
    }
    return catalog;
}

bool FilterCatalog::isBuiltIn(const QString &path)
{
    return path.startsWith(QLatin1String(":/"));
}

std::optional<FilterCatalog::Entry> FilterCatalog::entry(const QString &path) const
{
    for (const Entry &e : m_entries) {
        if (e.path == path)
            return e;
    }
    return std::nullopt;
}

std::optional<filters::Graph> FilterCatalog::load(const QString &path, QString *error) const
{
    std::optional<filters::Graph> graph = filters::Graph::load(path, filters::NodeRegistry::global(), error);
    if (graph && isBuiltIn(path)) {
        graph->name = translated(graph->name);
        graph->description = translated(graph->description);
        QList<filters::PublishedParam> published = graph->published();
        for (filters::PublishedParam &p : published)
            p.label = translated(p.label);
        graph->setPublished(published);
    }
    return graph;
}

void FilterCatalog::watchFolder()
{
    if (!m_folder.isEmpty() && QFileInfo(m_folder).isDir() && !m_watcher.directories().contains(m_folder))
        m_watcher.addPath(m_folder);
}

void FilterCatalog::scheduleRefresh()
{
    m_refreshTimer.start();
}

void FilterCatalog::refresh()
{
    watchFolder();
    QList<Entry> entries;
    auto describe = [this](const QString &path, bool builtIn) {
        Entry e;
        e.path = path;
        e.builtIn = builtIn;
        const QFileInfo info(path);
        e.modified = builtIn ? QDateTime() : info.lastModified();
        e.name = info.completeBaseName();
        QString error;
        if (const std::optional<filters::Graph> g = load(path, &error)) {
            if (!g->name.isEmpty())
                e.name = g->name;
            e.description = g->description;
            e.usesAi = g->needsExplicitRun();
        } else {
            e.error = error;
        }
        return e;
    };
    for (const QString &path : builtInPaths())
        entries << describe(path, true);
    QList<Entry> saved;
    if (!m_folder.isEmpty()) {
        const QFileInfoList files = QDir(m_folder).entryInfoList({QStringLiteral("*") + filters::Graph::fileExtension()},
                                                                 QDir::Files);
        for (const QFileInfo &file : files)
            saved << describe(file.absoluteFilePath(), false);
    }
    std::sort(saved.begin(), saved.end(), [](const Entry &a, const Entry &b) {
        const int c = QString::localeAwareCompare(a.name, b.name);
        return c != 0 ? c < 0 : a.path < b.path;
    });
    entries << saved;
    if (sameEntries(entries, m_entries))
        return;
    m_entries = entries;
    emit changed();
}

bool FilterCatalog::ensureFolder(QString *error)
{
    if (m_folder.isEmpty() || !QDir().mkpath(m_folder)) {
        if (error)
            *error = tr("Could not create the filters folder: %1").arg(QDir::toNativeSeparators(m_folder));
        return false;
    }
    watchFolder();
    return true;
}

QString FilterCatalog::save(const filters::Graph &graph, const QString &path, QString *error)
{
    if (!ensureFolder(error))
        return {};
    const filters::Library library(m_folder);
    const QString target = library.save(graph, isBuiltIn(path) ? QString() : path, error);
    if (target.isEmpty())
        return {};
    m_thumbs.remove(target);
    refresh();
    emit saved(target);
    return target;
}

QString FilterCatalog::duplicate(const QString &path, QString *error)
{
    std::optional<filters::Graph> graph = load(path, error);
    if (!graph)
        return {};
    const QString name = graph->name.isEmpty() ? QFileInfo(path).completeBaseName() : graph->name;
    graph->name = tr("%1 copy").arg(name);
    return save(*graph, {}, error);
}

QString FilterCatalog::create(const QString &name, QString *error)
{
    filters::Graph graph = filters::Graph::createDefault();
    graph.name = name.trimmed().isEmpty() ? tr("New filter") : name.trimmed();
    return save(graph, {}, error);
}

bool FilterCatalog::remove(const QString &path, QString *error)
{
    if (isBuiltIn(path)) {
        if (error)
            *error = tr("Built-in filters cannot be deleted.");
        return false;
    }
    QFile file(path);
    if (!file.remove()) {
        if (error)
            *error = tr("Could not delete the filter: %1").arg(file.errorString());
        return false;
    }
    m_thumbs.remove(path);
    refresh();
    return true;
}

QImage FilterCatalog::thumbnail(const QString &path)
{
    const std::optional<Entry> e = entry(path);
    if (!e)
        return {};
    auto it = m_thumbs.find(path);
    if (it != m_thumbs.end() && it->modified == e->modified)
        return it->image;
    startThumbnail(*e);
    return {};
}

void FilterCatalog::startThumbnail(const Entry &e)
{
    Thumb &thumb = m_thumbs[e.path];
    thumb.modified = e.modified;
    thumb.image = QImage();
    thumb.pending = false;
    if (!e.error.isEmpty() || e.usesAi)
        return; // shown as an icon: AI steps run only on request
    const std::optional<filters::Graph> graph = load(e.path);
    if (!graph)
        return;
    if (m_sample.isNull())
        m_sample = filterSampleImage();
    thumb.pending = true;
    filters::EvalOptions options;
    options.scale = double(thumbnailSize().width()) / m_sample.width();
    options.runExpensive = false;
    const QString path = e.path;
    const QDateTime modified = e.modified;
    auto done = [this, alive = m_alive, path, modified](const QImage &image) {
        if (!*alive)
            return;
        auto it = m_thumbs.find(path);
        if (it == m_thumbs.end() || it->modified != modified)
            return; // the file changed meanwhile; a new thumbnail is on its way
        it->pending = false;
        it->image = image;
        if (!image.isNull())
            emit thumbnailReady(path);
    };
    m_thumbEvaluator->evaluate(*graph, {m_sample, {}}, options)
        .then(this, [done](const filters::EvalResult &r) { done(r.needsRun ? QImage() : r.image); })
        .onFailed(this, [done](const std::exception &) { done({}); })
        .onCanceled(this, [done] { done({}); });
}

} // namespace lumen::ui
