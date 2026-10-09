#include "core/Async.h"
#include "core/LatencyWatchdog.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Settings.h"
#include "ui/MainWindow.h"
#include "ui/Studio.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QFontDatabase>
#include <QImageReader>
#include <QFileInfo>
#include <QTimer>

using namespace lumen;

int main(int argc, char *argv[])
{
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
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
    QCommandLineOption pageOpt(QStringLiteral("page"), QStringLiteral("Page to show (create, generate, editor, poses, library)."), QStringLiteral("name"));
    QCommandLineOption imageOpt(QStringLiteral("image"), QStringLiteral("Open an image in the editor."), QStringLiteral("path"));
    QCommandLineOption sizeOpt(QStringLiteral("size"), QStringLiteral("Window size WxH (screenshot mode)."), QStringLiteral("size"));
    parser.addOptions({projectOpt, screenshotOpt, pageOpt, imageOpt, sizeOpt});
    parser.process(app);

    paths::ensure();
    log::install(paths::logs());
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
                                            {QStringLiteral("library"), ui::Page::Library}};
    if (pages.contains(page))
        window.showPage(pages.value(page));
    if (parser.isSet(imageOpt)) {
        QImageReader reader(parser.value(imageOpt));
        reader.setAutoTransform(true);
        const QImage img = reader.read();
        if (!img.isNull())
            studio.openImageAsDocument(img, QFileInfo(parser.value(imageOpt)).completeBaseName());
    }
    if (parser.isSet(sizeOpt)) {
        const QStringList wh = parser.value(sizeOpt).split(u'x');
        if (wh.size() == 2)
            window.resize(wh[0].toInt(), wh[1].toInt());
    }

#ifndef NDEBUG
    LatencyWatchdog watchdog(80);
    watchdog.start();
#endif

    window.show();

    if (parser.isSet(screenshotOpt)) {
        const QString out = parser.value(screenshotOpt);
        QTimer::singleShot(2500, &window, [&window, out] {
            window.saveScreenshot(out);
            QApplication::exit(0);
        });
    }

    const int rc = app.exec();
    log::shutdown();
    return rc;
}
