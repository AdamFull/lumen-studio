#include "core/Async.h"
#include "core/LatencyWatchdog.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Settings.h"
#include "ui/MainWindow.h"
#include "ui/parts/PartsPage.h"
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
    QCommandLineOption pageOpt(QStringLiteral("page"), QStringLiteral("Page to show (create, generate, editor, poses, parts, library)."), QStringLiteral("name"));
    QCommandLineOption imageOpt(QStringLiteral("image"), QStringLiteral("Open an image in the editor."), QStringLiteral("path"));
    QCommandLineOption sizeOpt(QStringLiteral("size"), QStringLiteral("Window size WxH (screenshot mode)."), QStringLiteral("size"));
    QCommandLineOption sliceOpt(QStringLiteral("slice"), QStringLiteral("Start a part slicing session (Live2D) for this image."), QStringLiteral("path"));
    QCommandLineOption sliceStepOpt(QStringLiteral("slice-step"),
                                    QStringLiteral("With --slice: open this step (1 source, 2 masks, 3 generate, 4 preview); 2+ segments with the skeleton."),
                                    QStringLiteral("step"));
    parser.addOptions({projectOpt, screenshotOpt, pageOpt, imageOpt, sizeOpt, sliceOpt, sliceStepOpt});
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
                                            {QStringLiteral("library"), ui::Page::Library},
                                            {QStringLiteral("parts"), ui::Page::Parts}};
    if (pages.contains(page))
        window.showPage(pages.value(page));
    if (parser.isSet(imageOpt)) {
        QImageReader reader(parser.value(imageOpt));
        reader.setAutoTransform(true);
        const QImage img = reader.read();
        if (!img.isNull())
            studio.openImageAsDocument(img, QFileInfo(parser.value(imageOpt)).completeBaseName());
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
