#include "core/Async.h"
#include "core/CrashGuard.h"
#include "core/LatencyWatchdog.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Settings.h"
#include "ui/Accessibility.h"
#include "ui/MainWindow.h"
#include "ui/pages/EditorPage.h"
#include "plugins/BuiltInPlugins.h"
#include "plugins/parts/PartsPage.h"
#include "ui/Studio.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QImageReader>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>
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
    QCommandLineOption pageOpt(QStringLiteral("page"),
                               QStringLiteral("Open generate (New from prompt), library (Browse project images) or a plugin's window "
                                              "(create, poses, parts)."),
                               QStringLiteral("name"));
    QCommandLineOption imageOpt(QStringLiteral("image"), QStringLiteral("Open an image (or layered PSD) in the editor."), QStringLiteral("path"));
    QCommandLineOption sizeOpt(QStringLiteral("size"), QStringLiteral("Window size WxH (screenshot mode)."), QStringLiteral("size"));
    QCommandLineOption sliceOpt(QStringLiteral("slice"),
                                QStringLiteral("Start a part slicing session (Live2D) for this image (the Live2D parts plugin must be on)."),
                                QStringLiteral("path"));
    QCommandLineOption sliceStepOpt(QStringLiteral("slice-step"),
                                    QStringLiteral("With --slice: open this step (1 source, 2 masks, 3 generate, 4 preview); 2+ segments with the skeleton."),
                                    QStringLiteral("step"));
    QCommandLineOption cubismOpt(QStringLiteral("cubism"),
                                 QStringLiteral("Preview a Live2D Cubism model (.model3.json) (the Live2D parts plugin must be on)."),
                                 QStringLiteral("path"));
    QCommandLineOption toolOpt(QStringLiteral("tool"), QStringLiteral("Editor tool to select (e.g. transform, brush, crop)."), QStringLiteral("id"));
    parser.addOptions({projectOpt, screenshotOpt, pageOpt, imageOpt, sizeOpt, sliceOpt, sliceStepOpt, cubismOpt, toolOpt});
    parser.process(app);

    paths::ensure();
    log::install(paths::logs());
    // UI language: Qt's own strings (dialog buttons...) and Lumen's.
    {
        const QString lang = Settings::instance().language();
        const QLocale locale = lang.isEmpty() ? QLocale::system() : QLocale(lang);
        QLocale::setDefault(locale);
        auto *qtTranslator = new QTranslator(&app);
        // Deployed builds ship the combined qt_<lang>.qm; a Qt installation also has qtbase_<lang>.qm.
        const QString qtDir = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
        if (qtTranslator->load(locale, QStringLiteral("qt"), QStringLiteral("_"), qtDir)
            || qtTranslator->load(locale, QStringLiteral("qtbase"), QStringLiteral("_"), qtDir))
            app.installTranslator(qtTranslator);
        auto *lumenTranslator = new QTranslator(&app);
        if (lumenTranslator->load(locale, QStringLiteral("lumen"), QStringLiteral("_"), QStringLiteral(":/i18n")))
            app.installTranslator(lumenTranslator);
    }
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
    ui::Accessibility::install();

    plugins::registerBuiltInPlugins();
    ui::Studio studio;
    ui::MainWindow window(&studio);

    const QString projectPath = parser.value(projectOpt);
    if (!projectPath.isEmpty())
        studio.openProject(projectPath);
    else
        studio.openDefaultProject();

    // A plugin's window, New from prompt or Browse project images, once the editor is on screen.
    const QString page = parser.value(pageOpt).toLower();
    if (!page.isEmpty() && page != ui::pages::Editor && studio.hasPage(page))
        QTimer::singleShot(0, &window, [&window, page] { window.showPage(page); });
    if (parser.isSet(imageOpt))
        studio.openFile(QFileInfo(parser.value(imageOpt)).absoluteFilePath());
    if (parser.isSet(toolOpt))
        window.editor()->selectTool(parser.value(toolOpt));
    // Both are the Live2D parts plugin's: say so instead of doing nothing (or importing the file).
    const bool parts = studio.hasPage(QStringLiteral("parts"));
    if ((parser.isSet(sliceOpt) || parser.isSet(cubismOpt)) && !parts) {
        qCWarning(lcApp) << "--slice and --cubism need the Live2D parts plugin, which is off";
        QTimer::singleShot(1000, &studio, [&studio] {
            studio.notify(QObject::tr("The Live2D parts plugin is off, so --slice and --cubism do nothing. Switch it on in Tools → "
                                      "Plugins… and restart Lumen."),
                          QStringLiteral("warning"));
        });
    }
    if (parser.isSet(sliceOpt) && parts) {
        const QString file = QFileInfo(parser.value(sliceOpt)).absoluteFilePath();
        const int step = parser.value(sliceStepOpt).toInt();
        // The Parts window is built when it is first opened.
        window.showPage(QStringLiteral("parts"));
        if (auto *parts = qobject_cast<ui::PartsPage *>(window.pageWidget(QStringLiteral("parts"))); parts && parser.isSet(sliceStepOpt))
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
    if (parser.isSet(cubismOpt) && parts) {
        window.showPage(QStringLiteral("parts"));
        if (auto *parts = qobject_cast<ui::PartsPage *>(window.pageWidget(QStringLiteral("parts"))))
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
    if (!parser.isSet(screenshotOpt)) {
        QTimer::singleShot(3000, &window, &ui::MainWindow::checkLeftoverCloudPods);
        QTimer::singleShot(200, &window, [&window] { window.offerModelsFolderMove(); });
    }
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
