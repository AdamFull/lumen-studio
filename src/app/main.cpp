#include "core/Async.h"
#include "core/CrashGuard.h"
#include "core/LatencyWatchdog.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Settings.h"
#include "ui/MainWindow.h"
#include "ui/pages/EditorPage.h"
#include "ui/parts/PartsPage.h"
#include "ui/Studio.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QImageReader>
#include <QFileInfo>
#include <QTimer>

#include <cstdlib>
#include <stdexcept>

using namespace lumen;

int main(int argc, char *argv[])
{
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QElapsedTimer startup;
    startup.start();
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Lumen Studio"));
    QApplication::setOrganizationName(QStringLiteral("Lumen"));
    QApplication::setApplicationVersion(QStringLiteral(LUMEN_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("AI-assisted image generation and editing studio"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption projectOpt(QStringLiteral("project"), QStringLiteral("Open this project file."), QStringLiteral("path"));
    QCommandLineOption screenshotOpt(QStringLiteral("screenshot"), QStringLiteral("Render the window to a PNG and quit."), QStringLiteral("path"));
    QCommandLineOption pageOpt(QStringLiteral("page"), QStringLiteral("Page to show (create, generate, editor, poses, parts, library)."), QStringLiteral("name"));
    QCommandLineOption imageOpt(QStringLiteral("image"), QStringLiteral("Open an image (or layered PSD) in the editor."), QStringLiteral("path"));
    QCommandLineOption sizeOpt(QStringLiteral("size"), QStringLiteral("Window size WxH (screenshot mode)."), QStringLiteral("size"));
    QCommandLineOption sliceOpt(QStringLiteral("slice"), QStringLiteral("Start a part slicing session (Live2D) for this image."), QStringLiteral("path"));
    QCommandLineOption sliceStepOpt(QStringLiteral("slice-step"),
                                    QStringLiteral("With --slice: open this step (1 source, 2 masks, 3 generate, 4 preview); 2+ segments with the skeleton."),
                                    QStringLiteral("step"));
    QCommandLineOption cubismOpt(QStringLiteral("cubism"), QStringLiteral("Preview a Live2D Cubism model (.model3.json)."), QStringLiteral("path"));
    QCommandLineOption toolOpt(QStringLiteral("tool"), QStringLiteral("Editor tool to select (e.g. transform, brush, crop)."), QStringLiteral("id"));
    parser.addOptions({projectOpt, screenshotOpt, pageOpt, imageOpt, sizeOpt, sliceOpt, sliceStepOpt, cubismOpt, toolOpt});
    parser.process(app);

    paths::ensure();
    log::install(paths::logs());
    crash::installHandlers(paths::logs());
    crash::SessionLock session(paths::appData());
    if (session.previousSessionCrashed())
        qCWarning(lcApp) << "The previous session did not exit cleanly:" << session.staleSessions();
    // LUMEN_CRASH_TEST=segv|throw|abort verifies the crash handlers.
    if (const QByteArray test = qgetenv("LUMEN_CRASH_TEST"); !test.isEmpty()) {
        QTimer::singleShot(500, [test] {
            if (test == "throw")
                throw std::runtime_error("crash test");
            if (test == "abort")
                std::abort();
            volatile int *null = nullptr;
            *null = 1;
        });
    }
    async::setMaxThreads(Settings::instance().workerThreads());
    QImageReader::setAllocationLimit(1024);

    QFont font = app.font();
    for (const char *family : {"Segoe UI Variable Text", "Segoe UI", "Inter", "Noto Sans"}) {
        if (QFontDatabase::hasFamily(QString::fromLatin1(family))) {
            font.setFamily(QString::fromLatin1(family));
            break;
        }
    }
    app.setFont(font);
    ui::Theme::instance().apply(app, Settings::instance().theme());

    ui::Studio studio;
    ui::MainWindow window(&studio);

    const QString projectPath = parser.value(projectOpt);
    if (!projectPath.isEmpty())
        studio.openProject(projectPath);
    else
        studio.openDefaultProject();

    const QString page = parser.value(pageOpt).toLower();
    const QHash<QString, ui::Page> pages = {{QStringLiteral("create"), ui::Page::Create},
                                            {QStringLiteral("generate"), ui::Page::Generate},
                                            {QStringLiteral("editor"), ui::Page::Editor},
                                            {QStringLiteral("poses"), ui::Page::Poses},
                                            {QStringLiteral("library"), ui::Page::Library},
                                            {QStringLiteral("parts"), ui::Page::Parts}};
    if (pages.contains(page))
        window.showPage(pages.value(page));
    if (parser.isSet(imageOpt))
        studio.openFile(QFileInfo(parser.value(imageOpt)).absoluteFilePath());
    if (parser.isSet(toolOpt)) {
        if (auto *editor = qobject_cast<ui::EditorPage *>(window.pageWidget(ui::Page::Editor)))
            editor->selectTool(parser.value(toolOpt));
    }
    if (parser.isSet(sliceOpt)) {
        const QString file = QFileInfo(parser.value(sliceOpt)).absoluteFilePath();
        const int step = parser.value(sliceStepOpt).toInt();
        if (auto *parts = qobject_cast<ui::PartsPage *>(window.pageWidget(ui::Page::Parts)); parts && parser.isSet(sliceStepOpt))
            parts->automate(step >= 2, std::clamp(step, 1, 4) - 1);
        auto start = [&studio, file] {
            studio.project()->importFile(file, doc::AssetKind::Reference).then(&studio, [&studio](doc::AssetInfo info) {
                studio.sliceAsset(info.id);
            });
        };
        if (studio.isProjectOpen())
            start();
        else
            QObject::connect(&studio, &ui::Studio::projectOpened, &studio, start, Qt::SingleShotConnection);
    }
    if (parser.isSet(cubismOpt)) {
        window.showPage(ui::Page::Parts);
        if (auto *parts = qobject_cast<ui::PartsPage *>(window.pageWidget(ui::Page::Parts)))
            parts->previewCubismModel(QFileInfo(parser.value(cubismOpt)).absoluteFilePath());
    }
    if (parser.isSet(sizeOpt)) {
        const QStringList wh = parser.value(sizeOpt).split(u'x');
        if (wh.size() == 2)
            window.resize(wh[0].toInt(), wh[1].toInt());
    }

    // GUI stalls are logged (lumen.perf); release builds only report the long ones.
#ifndef NDEBUG
    LatencyWatchdog watchdog(80);
#else
    LatencyWatchdog watchdog(250);
#endif
    watchdog.start();

    window.show();
    QTimer::singleShot(0, &window, [&startup] { qCInfo(lcPerf) << "Window shown" << startup.elapsed() << "ms after start"; });
    if (session.previousSessionCrashed()) {
        QTimer::singleShot(1500, &studio, [&studio] {
            studio.notify(QObject::tr("Lumen Studio did not close properly last time. Open documents were restored from the last "
                                      "autosave; a crash report may be in the log folder (Help → Open log folder)."),
                          QStringLiteral("warning"));
        });
    }

    if (parser.isSet(screenshotOpt)) {
        const QString out = parser.value(screenshotOpt);
        QTimer::singleShot(2500, &window, [&window, out] {
            window.saveScreenshot(out);
            QApplication::exit(0);
        });
    }

    const int rc = app.exec();
    session.release();
    log::shutdown();
    return rc;
}
