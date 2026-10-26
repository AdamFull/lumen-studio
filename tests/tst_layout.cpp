// Layout audit: opens the main window (the start screen and the editor), every plugin page's window,
// the panels and the dialogs at common window sizes and reports controls that are cut off, squeezed
// below their minimum size, overlap a sibling, stick out of their parent, or text fields that scroll
// although their text fits, and dialogs that open larger than the screen or cannot fit a small one.
//
// Strict: a test function fails on any finding of its own (the test suite is the gate; there is no CI
// service). LUMEN_LAYOUT_REPORT=1 only reports, for building a checklist. With LUMEN_SCREENSHOT_DIR,
// every state is saved as a PNG next to layout-report.md (in layout/, at 150 % in layout-150/).
//
// tst_layout_150 is the same audit at a 150 % scale factor (CMake builds it from this file with
// LUMEN_LAYOUT_SCALE=1.5): a 1920×1080 screen at 144 dpi, which Qt shows as a 1280×720 screen of
// device-independent pixels at a device pixel ratio of 1.5, as Windows does at 150 %. The scale
// factor must be set before the application starts, so it is a second program.

#include "core/JobQueue.h"
#include "core/Settings.h"
#include "doc/Project.h"
#include "plugins/BuiltInPlugins.h"
#include "plugins/characters/assistant/AssistantPanel.h"
#include "pose/PosePresets.h"
#include "pose/Skeleton.h"
#include "runtime/Download.h"
#include "runtime/ModelHub.h"
#include "runtime/ModelLibrary.h"
#include "runtime/ModelPresets.h"
#include "runtime/RuntimeStore.h"
#include "runtime/ServerProfile.h"
#include "runtime/Supervisor.h"
#include "ui/Accessibility.h"
#include "ui/DocumentFiles.h"
#include "ui/MainWindow.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/canvas/CanvasView.h"
#include "ui/canvas/EditorState.h"
#include "ui/dialogs/LocalServersPage.h"
#include "ui/dialogs/LocalSetupDialog.h"
#include "ui/dialogs/ModelHubDialog.h"
#include "ui/dialogs/NewFromPromptDialog.h"
#include "ui/dialogs/NewCanvasDialog.h"
#include "ui/dialogs/PoseEditDialog.h"
#include "ui/dialogs/SettingsDialog.h"
#include "ui/dialogs/SetupWizard.h"
#include "ui/filters/FilterCatalog.h"
#include "ui/filters/FiltersPanel.h"
#include "ui/filters/NodeEditorWindow.h"
#include "ui/ext/Extension.h"
#include "core/ImageOps.h"
#include "ui/pages/EditorPage.h"
#include "ui/pages/PromptBar.h"
#include "ui/pages/StartScreen.h"
#include "ui/pages/VariantStrip.h"
#include "ui/widgets/Controls.h"

#include <QAbstractButton>
#include <QAbstractScrollArea>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSlider>
#include <QSplitter>
#include <QStandardPaths>
#include <QTabBar>
#include <QTabWidget>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocumentFragment>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtTest>

using namespace lumen;
using namespace lumen::ui;

namespace {

#ifndef LUMEN_LAYOUT_SCALE
#define LUMEN_LAYOUT_SCALE 1.0
#endif

// The scale factor of the audit's screen: 1, or 1.5 in tst_layout_150.
double layoutScale()
{
    return LUMEN_LAYOUT_SCALE;
}

bool scaled()
{
    return layoutScale() > 1.0;
}

// Logical sizes: 1920×1080 at 150 % and 125 %, and at 100 %. At 150 % the screen is 1280×720.
QList<QSize> windowSizes()
{
    if (scaled())
        return {QSize(1280, 720)};
    return {QSize(1280, 720), QSize(1536, 864), QSize(1920, 1080)};
}

// The smallest screen a dialog must fit on (1920×1080 at 150 %, minus the taskbar).
const QSize kSmallScreen(1280, 680);

// Where the screenshots and the report go, under LUMEN_SCREENSHOT_DIR.
QString shotFolder()
{
    return scaled() ? QStringLiteral("layout-150") : QStringLiteral("layout");
}

struct Finding
{
    QString surface;
    QString kind;
    QString widget;
    QString detail;
};

QString shortText(QString s)
{
    s.replace(QLatin1Char('\n'), QLatin1Char(' '));
    s.remove(QLatin1Char('&'));
    return s.size() > 40 ? s.left(37) + QStringLiteral("…") : s;
}

QString widgetText(const QWidget *w)
{
    if (const auto *l = qobject_cast<const QLabel *>(w))
        return l->text().contains(QLatin1Char('<')) ? QTextDocumentFragment::fromHtml(l->text()).toPlainText() : l->text();
    if (const auto *b = qobject_cast<const QAbstractButton *>(w))
        return b->text().isEmpty() ? b->toolTip() : b->text();
    if (const auto *c = qobject_cast<const QComboBox *>(w))
        return c->currentText();
    return w->accessibleName();
}

// "QPushButton 'Save' in ProviderForm < SettingsDialog".
QString describe(const QWidget *w, const QWidget *root)
{
    QString s = QLatin1String(w->metaObject()->className());
    if (!w->objectName().isEmpty() && !w->objectName().startsWith(QLatin1String("qt_")))
        s += u':' + w->objectName();
    const QString text = shortText(widgetText(w));
    if (!text.isEmpty())
        s += QStringLiteral(" '%1'").arg(text);
    QStringList chain;
    for (const QWidget *p = w->parentWidget(); p && p != root && chain.size() < 3; p = p->parentWidget()) {
        if (p->objectName().startsWith(QLatin1String("qt_")))
            continue;
        chain << QLatin1String(p->metaObject()->className()) + (p->objectName().isEmpty() ? QString() : u':' + p->objectName());
    }
    if (!chain.isEmpty())
        s += QStringLiteral(" in ") + chain.join(QStringLiteral(" < "));
    return s;
}

bool inLayout(const QWidget *w)
{
    const QWidget *p = w->parentWidget();
    return p && p->layout() && p->layout()->indexOf(const_cast<QWidget *>(w)) >= 0;
}

// Content of a scroll area is meant to be larger than its viewport.
bool insideScrollViewport(const QWidget *w)
{
    for (const QWidget *p = w->parentWidget(); p; p = p->parentWidget()) {
        if (p->objectName() == QLatin1String("qt_scrollarea_viewport"))
            return true;
    }
    return false;
}

bool ignored(const QWidget *w)
{
    // An ElidedLabel shortens its text on purpose (the full text is in its tooltip).
    return qobject_cast<const QScrollBar *>(w) || qobject_cast<const QSplitterHandle *>(w) || w->inherits("lumen::ui::ElidedLabel")
           || w->objectName().startsWith(QLatin1String("qt_")) || w->isWindow();
}

QSize effectiveMinimum(const QWidget *w)
{
    const QSize msh = w->minimumSizeHint();
    const QSize min = w->minimumSize();
    return QSize(min.width() > 0 ? min.width() : msh.width(), min.height() > 0 ? min.height() : msh.height());
}

class Auditor
{
public:
    void audit(QWidget *root, const QString &surface)
    {
        QSet<QString> seen; // one finding per widget and kind, however many list rows repeat it
        auto add = [&](const QString &kind, const QWidget *w, const QString &detail) {
            const QString who = describe(w, root);
            if (seen.contains(kind + who))
                return;
            seen.insert(kind + who);
            m_findings << Finding{surface, kind, who, detail};
        };

        const QSize need = root->minimumSizeHint().expandedTo(root->minimumSize());
        if (need.width() > root->width() || need.height() > root->height())
            add(QStringLiteral("window too small"), root,
                QStringLiteral("needs %1×%2, has %3×%4").arg(need.width()).arg(need.height()).arg(root->width()).arg(root->height()));

        const QList<QWidget *> all = root->findChildren<QWidget *>();
        for (QWidget *w : all) {
            if (!w->isVisibleTo(root) || ignored(w) || w->width() <= 0 || w->height() <= 0)
                continue;
            // Only widgets the user can see: inside every ancestor's rectangle at least partly.
            const QRect onRoot(w->mapTo(root, QPoint(0, 0)), w->size());
            if (!onRoot.intersects(root->rect()))
                continue;
            const bool managed = inLayout(w) && !insideScrollViewport(w);

            // Squeezed below what the widget needs.
            const QSizePolicy sp = w->sizePolicy();
            const QSize min = effectiveMinimum(w);
            if (managed && min.isValid()) {
                const bool narrow = sp.horizontalPolicy() != QSizePolicy::Ignored && w->width() + 1 < min.width();
                const bool flat = sp.verticalPolicy() != QSizePolicy::Ignored && w->height() + 1 < min.height();
                if (narrow || flat)
                    add(QStringLiteral("squeezed"), w,
                        QStringLiteral("%1×%2, needs %3×%4").arg(w->width()).arg(w->height()).arg(min.width()).arg(min.height()));
            }

            // Text cut off.
            if (const auto *l = qobject_cast<QLabel *>(w); l && !l->text().isEmpty() && l->pixmap().isNull()) {
                if (l->wordWrap()) {
                    const int h = l->heightForWidth(l->width());
                    if (h > l->height() + 2)
                        add(QStringLiteral("text cut"), w, QStringLiteral("wrapped text needs %1 px height, has %2").arg(h).arg(l->height()));
                } else if (l->sizeHint().width() > l->width() + 2) {
                    add(QStringLiteral("text cut"), w, QStringLiteral("needs %1 px width, has %2").arg(l->sizeHint().width()).arg(l->width()));
                }
            } else if (const auto *b = qobject_cast<QAbstractButton *>(w); b && !b->text().isEmpty()) {
                const auto *tb = qobject_cast<const QToolButton *>(b);
                const bool textShown = !tb || tb->toolButtonStyle() != Qt::ToolButtonIconOnly;
                // The widest line (a tile's name may take two).
                int textWidth = 0;
                for (const QString &line : b->text().remove(QLatin1Char('&')).split(u'\n'))
                    textWidth = std::max(textWidth, b->fontMetrics().horizontalAdvance(line));
                if (textShown && (b->sizeHint().width() > b->width() + 2 || textWidth > b->width()))
                    add(QStringLiteral("text cut"), w, QStringLiteral("needs %1 px width, has %2").arg(b->sizeHint().width()).arg(b->width()));
            } else if (const auto *t = qobject_cast<const AutoTextEdit *>(w)) {
                // A field that grows with its text shows a scroll bar only beyond its last line.
                int lines = 0;
                for (QTextBlock b = t->document()->begin(); b.isValid(); b = b.next())
                    lines += std::max(1, b.layout() ? b.layout()->lineCount() : 1);
                if (lines <= t->maxLines() && t->verticalScrollBar()->isVisibleTo(root))
                    add(QStringLiteral("text cut"), w,
                        QStringLiteral("%1 line(s) of %2 scroll in %3 px").arg(lines).arg(t->maxLines()).arg(t->viewport()->height()));
            } else if (const auto *c = qobject_cast<QComboBox *>(w); c && !c->isEditable() && !c->currentText().isEmpty()) {
                // The current item: its text plus the icon, frame and drop-down arrow.
                QStyleOptionComboBox opt;
                opt.initFrom(c);
                const QRect field = c->style()->subControlRect(QStyle::CC_ComboBox, &opt, QStyle::SC_ComboBoxEditField, c);
                const int iconWidth = c->itemIcon(c->currentIndex()).isNull() ? 0 : c->iconSize().width() + 4;
                const int textWidth = c->fontMetrics().horizontalAdvance(c->currentText()) + iconWidth;
                if (textWidth > field.width() + 2)
                    add(QStringLiteral("text cut"), w, QStringLiteral("current item needs %1 px, field has %2").arg(textWidth).arg(field.width()));
            }

            // Sticking out of a parent, and so clipped by it (up to a scroll area, whose content may be
            // larger than its viewport). Nested layouts (status bar, tool bars) count too.
            for (const QWidget *a = w->parentWidget(); a; a = a->parentWidget()) {
                if (a->objectName() == QLatin1String("qt_scrollarea_viewport"))
                    break;
                const QRect inAncestor(w->mapTo(a, QPoint(0, 0)), w->size());
                if (!a->rect().adjusted(-1, -1, 1, 1).contains(inAncestor)) {
                    const QRect cut = inAncestor.intersected(a->rect());
                    add(QStringLiteral("clipped"), w,
                        QStringLiteral("%1×%2, %3×%4 of it inside %5").arg(w->width()).arg(w->height()).arg(cut.width()).arg(cut.height())
                            .arg(a == root ? QStringLiteral("the window") : QLatin1String(a->metaObject()->className())));
                    break;
                }
                if (a == root)
                    break;
            }
        }

        // Siblings of one layout that overlap (a layout squeezed below its minimum).
        for (QWidget *p : all) {
            if (!p->isVisibleTo(root) || !p->layout() || insideScrollViewport(p) || qobject_cast<QSplitter *>(p))
                continue;
            QList<QWidget *> kids;
            for (QObject *o : p->children()) {
                auto *k = qobject_cast<QWidget *>(o);
                if (k && k->isVisibleTo(root) && !ignored(k) && p->layout()->indexOf(k) >= 0)
                    kids << k;
            }
            for (int i = 0; i < kids.size(); ++i) {
                for (int j = i + 1; j < kids.size(); ++j) {
                    const QRect overlap = kids[i]->geometry().intersected(kids[j]->geometry());
                    if (overlap.width() > 2 && overlap.height() > 2)
                        add(QStringLiteral("overlap"), kids[i], QStringLiteral("overlaps %1 by %2×%3").arg(describe(kids[j], root))
                                                                     .arg(overlap.width()).arg(overlap.height()));
                }
            }
        }
    }

    // A dialog: reports when it opens larger than its screen (at 150 %, than a small screen with its
    // taskbar) or cannot be made small enough for a small screen, then audits it at the size it gets
    // there.
    void auditDialog(QWidget *dialog, const QString &surface)
    {
        dialog->show();
        QVERIFY(QTest::qWaitForWindowExposed(dialog));
        QTest::qWait(20);
        QSize screen = dialog->screen()->availableGeometry().size();
        if (scaled())
            screen = screen.boundedTo(kSmallScreen);
        const QSize natural = dialog->size().expandedTo(dialog->minimumSizeHint());
        if (natural.width() > screen.width() || natural.height() > screen.height())
            m_findings << Finding{surface, QStringLiteral("larger than screen"), QLatin1String(dialog->metaObject()->className()),
                                  QStringLiteral("opens at %1×%2 on a %3×%4 screen")
                                      .arg(natural.width()).arg(natural.height()).arg(screen.width()).arg(screen.height())};
        const QSize minimum = dialog->minimumSizeHint().expandedTo(dialog->minimumSize());
        if (minimum.width() > kSmallScreen.width() || minimum.height() > kSmallScreen.height())
            m_findings << Finding{surface, QStringLiteral("larger than screen"), QLatin1String(dialog->metaObject()->className()),
                                  QStringLiteral("needs at least %1×%2, more than a %3×%4 screen")
                                      .arg(minimum.width()).arg(minimum.height()).arg(kSmallScreen.width()).arg(kSmallScreen.height())};
        dialog->resize(dialog->size().boundedTo(kSmallScreen));
        settle();
        audit(dialog, surface);
        shoot(dialog, surface);
    }

    void shoot(QWidget *w, const QString &surface)
    {
        if (m_shots.isEmpty())
            return;
        QString name = surface;
        name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9]+")), QStringLiteral("-"));
        w->grab().save(QDir(m_shots).filePath(name.toLower() + QStringLiteral(".png")));
    }

    // Lets pending layouts and the widgets' own short timers run (the prompt bar updates 30 ms after
    // a change, the selection's outline 90 ms after), so a screenshot shows the state it is named after.
    static void settle()
    {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        QTest::qWait(120);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    }

    void setShotDir(const QString &dir) { m_shots = dir; }
    const QList<Finding> &findings() const { return m_findings; }

    QString report() const
    {
        QString out = QStringLiteral("# Layout audit\n\n%1 findings.\n").arg(m_findings.size());
        QString surface;
        for (const Finding &f : m_findings) {
            if (f.surface != surface) {
                surface = f.surface;
                out += QStringLiteral("\n## %1\n\n").arg(surface);
            }
            out += QStringLiteral("- **%1**: %2 (%3)\n").arg(f.kind, f.widget, f.detail);
        }
        return out;
    }

private:
    QList<Finding> m_findings;
    QString m_shots;
};

QImage sampleImage()
{
    QImage im(1600, 1000, QImage::Format_ARGB32_Premultiplied);
    QPainter p(&im);
    QLinearGradient g(0, 0, 1600, 1000);
    g.setColorAt(0, QColor(40, 70, 120));
    g.setColorAt(1, QColor(220, 160, 90));
    p.fillRect(im.rect(), g);
    p.setBrush(QColor(240, 240, 240));
    p.drawEllipse(QPoint(800, 500), 220, 220);
    return im;
}

// A model file that is only a safetensors header with these tensor shapes (what the library reads
// to tell what a file is), padded past the library's 1 MB minimum.
QString writeModelFile(const QTemporaryDir &dir, const QString &relative, const QList<QPair<QString, QList<int>>> &tensors)
{
    QJsonObject header;
    for (const auto &[name, shape] : tensors) {
        QJsonArray dims;
        for (int d : shape)
            dims.append(d);
        header.insert(name, QJsonObject{{QStringLiteral("dtype"), QStringLiteral("BF16")}, {QStringLiteral("shape"), dims},
                                        {QStringLiteral("data_offsets"), QJsonArray{0, 0}}});
    }
    const QByteArray json = QJsonDocument(header).toJson(QJsonDocument::Compact);
    QByteArray data;
    for (int i = 0; i < 8; ++i)
        data.append(char((quint64(json.size()) >> (8 * i)) & 0xFF));
    data += json + QByteArray(2 * 1024 * 1024, '\0');
    const QString path = QDir::cleanPath(dir.filePath(relative));
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write(data);
    return path;
}

QAction *actionNamed(QWidget *window, const QString &name)
{
    return window->findChild<QAction *>(name);
}

// Every plugin on for the audit (they are off by default), as before the next test.
struct AllPlugins
{
    AllPlugins()
    {
        for (const ext::Extension *e : ext::Extensions::instance().all())
            ext::Extensions::instance().setEnabled(e->id(), true);
        ext::Extensions::instance().refresh();
    }
    ~AllPlugins()
    {
        for (const ext::Extension *e : ext::Extensions::instance().all())
            Settings::instance().remove(QStringLiteral("plugins/%1/enabled").arg(e->id()));
        ext::Extensions::instance().refresh();
    }
};

// An image engine for the editor's states (a stable-diffusion.cpp server that is never asked: a
// model without instruction edits, so the prompt bar offers a variation), removed again afterwards.
struct ImageEngine
{
    explicit ImageEngine(Studio *studio, const QString &name = QStringLiteral("Local image server"))
        : m_studio(studio)
    {
        m_id = studio->providers()->addProvider(QStringLiteral("sdcpp"), name, {{QStringLiteral("url"), QStringLiteral("http://127.0.0.1:9")}})->instanceId();
    }
    ~ImageEngine() { m_studio->providers()->removeProvider(m_id); }

private:
    Studio *m_studio;
    QString m_id;
};

// Four results to choose from, as a prompt run leaves them above the prompt bar.
void showResults(MainWindow &window, doc::Document *doc)
{
    QList<QImage> results;
    for (int i = 0; i < 4; ++i) {
        QImage im = sampleImage().scaled(doc->size());
        if (i % 2)
            im.invertPixels();
        // A selection's results are transparent around it (they show over the picture).
        if (doc->hasSelection())
            im = img::applyMaskToAlpha(img::normalized(im), doc->selection());
        results << im;
    }
    window.editor()->variantStrip()->addVariants(doc, results, QStringLiteral("a long description of what should change in the picture"));
}

// The editor's right panel (Layers, History, Filters and the AI panel).
QTabWidget *sidePanel(MainWindow &window)
{
    for (QTabWidget *t : window.editor()->findChildren<QTabWidget *>()) {
        if (t->indexOf(window.editor()->filtersPanel()) >= 0)
            return t;
    }
    return nullptr;
}

// The Filters tab with Old photo chosen (two sliders) and its preview on the canvas.
void showFilters(MainWindow &window)
{
    window.editor()->showFiltersPanel();
    QVERIFY(window.editor()->filtersPanel()->selectFilter(QStringLiteral(":/filters/builtin/old-photo.lumenfilter")));
    QTRY_VERIFY_WITH_TIMEOUT(!window.editor()->canvas()->preview(CanvasView::Preview::Filter).isNull(), 15000);
}

// Report and screenshot names of the plugin windows.
QString pageName(const QString &id)
{
    static const QHash<QString, QString> names = {
        {QStringLiteral("create"), QStringLiteral("Create")}, {QStringLiteral("poses"), QStringLiteral("Poses")}, {QStringLiteral("parts"), QStringLiteral("Parts")}};
    return names.value(id, id);
}

} // namespace

class TestLayout : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();
    void mainWindow();
    void mainWindowFitsSmallScreen();
    void settingsDialog();
    void dialogs();
    void filterTools();
    void fileQuestions();

private:
    Auditor m_auditor;
    qsizetype m_findingsBefore = 0; // the findings of the test functions before this one
};

void TestLayout::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    plugins::registerBuiltInPlugins();
    // The fonts and theme of the real app (main.cpp), so text widths match.
    QFont font = QApplication::font();
    for (const char *family : {"Segoe UI Variable Text", "Segoe UI", "Inter", "Noto Sans"}) {
        if (QFontDatabase::hasFamily(QString::fromLatin1(family))) {
            font.setFamily(QString::fromLatin1(family));
            break;
        }
    }
    QApplication::setFont(font);
    Theme::instance().apply(*qApp, QStringLiteral("dark"));
    qInfo("screen %dx%d, font %s", QGuiApplication::primaryScreen()->geometry().width(),
          QGuiApplication::primaryScreen()->geometry().height(), qPrintable(QApplication::font().family()));
    qInfo("scale factor %.2f: device pixel ratio %.2f, %dx%d device-independent pixels", layoutScale(),
          QGuiApplication::primaryScreen()->devicePixelRatio(), QGuiApplication::primaryScreen()->geometry().width(),
          QGuiApplication::primaryScreen()->geometry().height());
    // The screen tst_layout_150 asks for, or the audit at 150 % would audit 100 % again.
    QCOMPARE(QGuiApplication::primaryScreen()->devicePixelRatio(), layoutScale());
    const QString shots = qEnvironmentVariable("LUMEN_SCREENSHOT_DIR");
    if (!shots.isEmpty()) {
        const QString dir = QDir(shots).filePath(shotFolder());
        QDir().mkpath(dir);
        m_auditor.setShotDir(dir);
    }
}

void TestLayout::init()
{
    m_findingsBefore = m_auditor.findings().size();
}

// Strict: a test function fails on its own findings (LUMEN_LAYOUT_REPORT=1 only reports them).
void TestLayout::cleanup()
{
    if (qEnvironmentVariableIntValue("LUMEN_LAYOUT_REPORT"))
        return;
    const QList<Finding> found = m_auditor.findings().mid(m_findingsBefore);
    QString text;
    for (const Finding &f : found)
        text += QStringLiteral("\n%1: %2 %3 (%4)").arg(f.surface, f.kind, f.widget, f.detail);
    QVERIFY2(found.isEmpty(), qPrintable(QStringLiteral("%1 layout finding(s):%2").arg(found.size()).arg(text)));
}

void TestLayout::cleanupTestCase()
{
    const QString report = m_auditor.report();
    const QString shots = qEnvironmentVariable("LUMEN_SCREENSHOT_DIR");
    if (!shots.isEmpty()) {
        QFile f(QDir(QDir(shots).filePath(shotFolder())).filePath(QStringLiteral("layout-report.md")));
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(report.toUtf8());
    }
    QMap<QString, int> byKind;
    for (const Finding &f : m_auditor.findings())
        ++byKind[f.kind];
    qInfo("%d finding(s)", int(m_auditor.findings().size()));
    for (auto it = byKind.cbegin(); it != byKind.cend(); ++it)
        qInfo("%s: %d", qPrintable(it.key()), it.value());
}

// The main window (the start screen, the editor with a document, its results, the AI panel and the
// Filters tab, the queue and the assistant over it), the plugin pages' windows and Browse project
// images, at each window size.
void TestLayout::mainWindow()
{
    AllPlugins plugins;
    QTemporaryDir dir;
    Studio studio;
    // A project with images and a saved document, so the start screen shows its recent row.
    studio.openProject(dir.filePath(QStringLiteral("layout.lumen")));
    QTRY_VERIFY_WITH_TIMEOUT(studio.isProjectOpen(), 5000);
    for (int i = 0; i < 4; ++i) {
        QImage im = sampleImage().scaled(800, 500 + 150 * i);
        if (i % 2)
            im.invertPixels();
        auto added = studio.project()->addAsset(im, doc::AssetKind::Generation, QStringLiteral("A sample with a long name %1").arg(i + 1));
        QTRY_VERIFY_WITH_TIMEOUT(added.isFinished(), 5000);
    }
    {
        doc::Document *saved = studio.openImageAsDocument(sampleImage(), QStringLiteral("Saved document"));
        auto done = studio.project()->saveDocument(saved);
        QTRY_VERIFY_WITH_TIMEOUT(done.isFinished(), 5000);
        studio.closeDocument(saved);
    }
    MainWindow window(&studio);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    for (const QSize &size : windowSizes()) {
        window.resize(size);
        const QString res = QStringLiteral("%1x%2").arg(size.width()).arg(size.height());
        auto auditWindow = [&](const QString &what) {
            Auditor::settle();
            const QString surface = QStringLiteral("%1 %2").arg(what, res);
            m_auditor.audit(&window, surface);
            m_auditor.shoot(&window, surface);
        };
        // No document open: the start screen (with "Set up AI": no image engine in the test).
        const auto open = studio.documents();
        for (doc::Document *d : open)
            studio.closeDocument(d);
        QTRY_VERIFY(window.editor()->showsStartScreen());
        // The saved document, the four images, and the samples of the earlier sizes (saved on close).
        QTRY_VERIFY(window.editor()->startScreen()->recentTiles().size() >= 5);
        auditWindow(QStringLiteral("Start screen"));
        // The editor with a document (no image engine: the prompt bar offers to set one up), with an
        // engine that cannot change the whole image from words (it offers a variation), with a
        // selection and four results, the prompt bar's ⋯ menu, the AI panel, and the queue and the
        // assistant over it.
        doc::Document *doc = studio.openImageAsDocument(sampleImage(), QStringLiteral("Sample"));
        QVERIFY(!window.editor()->showsStartScreen());
        PromptBar *bar = window.editor()->promptBar();
        auto *generate = bar->findChild<QPushButton *>(QStringLiteral("promptGenerate"));
        QTRY_COMPARE(generate->text(), QStringLiteral("Set up AI…"));
        auditWindow(QStringLiteral("Editor"));
        {
            ImageEngine engine(&studio);
            QTRY_VERIFY(bar->plan().action == PromptAction::CannotEditImage);
            // What the screenshot is named after, on screen (not only the plan).
            QTRY_COMPARE(generate->text(), QStringLiteral("Make a variation"));
            QTRY_VERIFY(bar->findChild<QPushButton *>(QStringLiteral("promptSelectArea"))->isVisible());
            auditWindow(QStringLiteral("Editor with an engine without edits"));
            QImage mask = img::createMask(doc->size());
            {
                QPainter p(&mask);
                p.setPen(Qt::NoPen);
                p.setBrush(Qt::white);
                p.drawEllipse(QRect(600, 300, 400, 400));
            }
            doc->applySelection(mask, QStringLiteral("Select"));
            QTRY_VERIFY(bar->plan().region);
            QTRY_VERIFY(bar->findChild<QPushButton *>(QStringLiteral("promptRemove"))->isVisible());
            showResults(window, doc);
            QVERIFY(window.editor()->variantStrip()->isVisible());
            // The strip takes some of the canvas's height; the picture stays whole.
            QTRY_VERIFY(QRectF(window.editor()->canvas()->focusProxy()->rect())
                            .contains(window.editor()->canvas()->mapFromDoc(QRectF(doc->bounds()))));
            auditWindow(QStringLiteral("Editor with results"));
            bar->showOptions();
            QWidget *popup = bar->optionsPopup();
            QVERIFY(QTest::qWaitForWindowExposed(popup));
            Auditor::settle();
            m_auditor.audit(popup, QStringLiteral("Prompt bar options %1").arg(res));
            m_auditor.shoot(popup, QStringLiteral("Prompt bar options %1").arg(res));
            popup->hide();
            window.editor()->showAiPanel();
            // Not report-only: four tabs in the narrow panel show whole names or only icons.
            QTRY_VERIFY2(sidePanel(window)->tabBar()->sizeHint().width() <= sidePanel(window)->width(),
                         qPrintable(QStringLiteral("the side tabs need %1 px at %2, the panel has %3")
                                        .arg(sidePanel(window)->tabBar()->sizeHint().width())
                                        .arg(res)
                                        .arg(sidePanel(window)->width())));
            auditWindow(QStringLiteral("Editor with the AI panel"));
            // Its options further down, with the negative prompt ("Leave out") of an engine that
            // takes one.
            auto *aiScroll = window.editor()->aiPanel()->findChild<QScrollArea *>();
            QVERIFY(aiScroll);
            aiScroll->verticalScrollBar()->setValue(aiScroll->verticalScrollBar()->maximum());
            auditWindow(QStringLiteral("Editor with the AI panel's options"));
            // Its sliders (Area may change, Context, Blend edge) start at one place, under one another.
            QSet<int> sliderStarts;
            for (SliderField *field : window.editor()->aiPanel()->findChildren<SliderField *>()) {
                if (auto *slider = field->findChild<QSlider *>(); slider && field->isVisible())
                    sliderStarts << slider->mapTo(window.editor()->aiPanel(), QPoint()).x();
            }
            QVERIFY2(sliderStarts.size() == 1, qPrintable(QStringLiteral("the AI panel's sliders start at %1 places at %2").arg(sliderStarts.size()).arg(res)));
            aiScroll->verticalScrollBar()->setValue(0);
            window.editor()->hideAiPanel();
            window.editor()->variantStrip()->discard();
            doc->clearSelection();
            // The Filters tab next to Layers and History, with a filter's preview on the canvas, and
            // with results over it (they show instead; the panel says so).
            showFilters(window);
            auditWindow(QStringLiteral("Editor with the Filters tab"));
            showResults(window, doc);
            QTRY_VERIFY(window.editor()->canvas()->preview(CanvasView::Preview::Filter).isNull());
            auditWindow(QStringLiteral("Editor with the Filters tab and results"));
            window.editor()->variantStrip()->discard();
            sidePanel(window)->setCurrentIndex(0);
        }
        QTRY_COMPARE(generate->text(), QStringLiteral("Set up AI…"));
        QTRY_VERIFY(!bar->findChild<QPushButton *>(QStringLiteral("promptRemove"))->isVisible());
        // The queue and the assistant lie over the editor, above the prompt bar (whose buttons they
        // would hide).
        auto clearOfBar = [&](QWidget *overlay, const QString &what) {
            const int top = bar->mapTo(window.centralWidget(), QPoint(0, 0)).y();
            QVERIFY2(overlay->geometry().bottom() < top,
                     qPrintable(QStringLiteral("%1 at %2 reaches %3, the prompt bar starts at %4").arg(what, res).arg(overlay->geometry().bottom()).arg(top)));
        };
        window.setQueueVisible(true);
        auditWindow(QStringLiteral("Editor with the queue"));
        clearOfBar(window.queuePanel(), QStringLiteral("The queue"));
        window.setQueueVisible(false);
        QAction *assistant = actionNamed(&window, QStringLiteral("panel-assistant"));
        QVERIFY(assistant);
        assistant->setChecked(true);
        auditWindow(QStringLiteral("Editor with the assistant"));
        clearOfBar(window.findChild<AssistantPanel *>(), QStringLiteral("The assistant"));
        assistant->setChecked(false);

        // The plugin pages, each in its window, at the same size.
        for (const QString &page : window.pageIds()) {
            if (page == pages::Editor)
                continue;
            window.showPage(page);
            QWidget *w = window.pageWindow(page);
            QVERIFY2(w, qPrintable(page));
            QVERIFY(QTest::qWaitForWindowExposed(w));
            w->resize(size);
            Auditor::settle();
            const QString surface = QStringLiteral("%1 window %2").arg(pageName(page), res);
            m_auditor.audit(w, surface);
            m_auditor.shoot(w, surface);
        }
        // Browse project images.
        QDialog *images = window.browseProjectImages();
        QVERIFY(QTest::qWaitForWindowExposed(images));
        images->resize(size);
        Auditor::settle();
        m_auditor.audit(images, QStringLiteral("Browse project images %1").arg(res));
        m_auditor.shoot(images, QStringLiteral("Browse project images %1").arg(res));
        images->hide();
    }
}

// Not report-only: the main window fits a 1080p screen at 150 % (1280×720) with the start screen,
// with a document (its prompt bar, results, the AI panel and the Filters tab), and with the queue and
// the assistant open;
// so do the prompt bar's ⋯ menu, the plugin pages' windows and Browse project images.
void TestLayout::mainWindowFitsSmallScreen()
{
    AllPlugins plugins;
    Studio studio;
    MainWindow window(&studio);
    window.resize(1280, 720);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    const QSize screen(1280, 720);
    auto check = [&](QWidget *w, const QString &what) {
        Auditor::settle();
        const QSize need = w->minimumSizeHint().expandedTo(w->minimumSize());
        QVERIFY2(need.width() <= screen.width() && need.height() <= screen.height(),
                 qPrintable(QStringLiteral("%1 needs %2×%3").arg(what).arg(need.width()).arg(need.height())));
    };
    QVERIFY(window.editor()->showsStartScreen());
    check(&window, QStringLiteral("the start screen"));
    doc::Document *doc = studio.openImageAsDocument(sampleImage(), QStringLiteral("Sample"));
    check(&window, QStringLiteral("the editor"));
    {
        // The prompt bar with its offers, four results above it, its ⋯ menu and the AI panel.
        ImageEngine engine(&studio);
        QTRY_VERIFY(window.editor()->promptBar()->plan().action == PromptAction::CannotEditImage);
        check(&window, QStringLiteral("the prompt bar"));
        showResults(window, doc);
        check(&window, QStringLiteral("the results"));
        window.editor()->promptBar()->showOptions();
        check(window.editor()->promptBar()->optionsPopup(), QStringLiteral("the prompt bar's options"));
        window.editor()->promptBar()->optionsPopup()->hide();
        window.editor()->showAiPanel();
        check(&window, QStringLiteral("the AI panel"));
        window.editor()->hideAiPanel();
        window.editor()->variantStrip()->discard();
        showFilters(window);
        check(&window, QStringLiteral("the Filters tab"));
        showResults(window, doc);
        check(&window, QStringLiteral("the Filters tab with results"));
        window.editor()->variantStrip()->discard();
        sidePanel(window)->setCurrentIndex(0);
    }
    window.setQueueVisible(true);
    check(&window, QStringLiteral("the queue"));
    window.setQueueVisible(false);
    QAction *assistant = actionNamed(&window, QStringLiteral("panel-assistant"));
    QVERIFY(assistant);
    assistant->setChecked(true);
    check(&window, QStringLiteral("the assistant"));
    assistant->setChecked(false);
    QCOMPARE(window.pageIds(), (QStringList{QStringLiteral("editor"), QStringLiteral("create"), QStringLiteral("poses"), QStringLiteral("parts")}));
    const QSize available = QGuiApplication::primaryScreen()->availableGeometry().size();
    for (const QString &page : window.pageIds().mid(1)) {
        window.showPage(page);
        QWidget *w = window.pageWindow(page);
        QVERIFY(w);
        check(w, QStringLiteral("the %1 window").arg(page));
        // It opens within the screen.
        QVERIFY(w->width() <= available.width() && w->height() <= available.height());
    }
    QDialog *images = window.browseProjectImages();
    check(images, QStringLiteral("Browse project images"));
    QVERIFY(images->width() <= available.width() && images->height() <= available.height());
    // A new window takes at most the screen it opens on.
    MainWindow fresh(&studio);
    QVERIFY(fresh.width() <= available.width() && fresh.height() <= available.height());
}

void TestLayout::settingsDialog()
{
    // With the plugins on, as the comment below says: their pages (Roles, Cloud training) too.
    AllPlugins plugins;
    Studio studio;
    SettingsDialog dialog(&studio);
    m_auditor.auditDialog(&dialog, QStringLiteral("Settings"));
    auto *tabs = dialog.findChild<QTabWidget *>();
    QVERIFY(tabs);
    // The pages are a list on the left: every name whole and none behind a scroll bar on a small
    // screen, with the plugins' pages too.
    auto *pages = dialog.findChild<QListWidget *>(QStringLiteral("settingsPages"));
    QVERIFY(pages && pages->isVisible() && !tabs->tabBar()->isVisible());
    QCOMPARE(pages->count(), tabs->count());
    QVERIFY(!pages->verticalScrollBar()->isVisible());
    for (int i = 0; i < pages->count(); ++i) {
        const QRect item = pages->visualItemRect(pages->item(i));
        const int text = pages->fontMetrics().horizontalAdvance(pages->item(i)->text()) + pages->iconSize().width();
        QVERIFY2(pages->viewport()->rect().contains(item) && text < item.width(),
                 qPrintable(QStringLiteral("%1: %2 px of text in a %3 px item").arg(pages->item(i)->text()).arg(text).arg(item.width())));
    }
    for (int i = 0; i < tabs->count(); ++i) {
        pages->setCurrentRow(i);
        QCOMPARE(tabs->currentIndex(), i);
        Auditor::settle();
        const QString surface = QStringLiteral("Settings - %1").arg(tabs->tabText(i).remove(QLatin1Char('&')));
        m_auditor.audit(&dialog, surface);
        m_auditor.shoot(&dialog, surface);
    }
}

void TestLayout::dialogs()
{
    Studio studio;
    {
        OnboardingDialog d(&studio);
        m_auditor.auditDialog(&d, QStringLiteral("Onboarding dialog"));
    }
    {
        LocalSetupDialog d(&studio);
        m_auditor.auditDialog(&d, QStringLiteral("Local setup dialog"));
    }
    {
        ModelHubDialog d(&studio);
        m_auditor.auditDialog(&d, QStringLiteral("Model hub dialog"));
    }
    {
        HubAccountsDialog d;
        m_auditor.auditDialog(&d, QStringLiteral("Hub accounts dialog"));
    }
    {
        InstallRuntimeDialog d(&studio, std::nullopt);
        m_auditor.auditDialog(&d, QStringLiteral("Install runtime dialog"));
    }
    {
        // The server profile dialogs as a person meets them: a program of each kind (adopted, never
        // started), and for sd.cpp FLUX.2 [klein] 4B with its VAE in the library and its text encoder
        // missing, then each model type chosen by hand; then with Advanced unfolded.
        QTemporaryDir files;
        const QString klein = writeModelFile(files, QStringLiteral("diffusion_models/flux-2-klein-4b-fp8.safetensors"),
                                             {{QStringLiteral("img_in.weight"), {3072, 128}},
                                              {QStringLiteral("txt_in.weight"), {3072, 7680}},
                                              {QStringLiteral("double_stream_modulation_img.lin.weight"), {18432, 3072}},
                                              {QStringLiteral("double_blocks.0.img_attn.qkv.weight"), {9216, 3072}}});
        const QString vae = writeModelFile(files, QStringLiteral("vae/flux2-vae.safetensors"),
                                           {{QStringLiteral("encoder.down_blocks.0.resnets.0.conv1.weight"), {128, 128, 3, 3}},
                                            {QStringLiteral("decoder.conv_in.weight"), {512, 32, 3, 3}},
                                            {QStringLiteral("decoder.up_blocks.0.resnets.0.conv1.weight"), {512, 512, 3, 3}}});
        QVERIFY(studio.models()->addRoot(files.path()));
        auto removeRoot = qScopeGuard([&] { studio.models()->removeRoot(files.path()); });
        QTRY_VERIFY_WITH_TIMEOUT(studio.models()->entry(klein) && studio.models()->entry(vae) && !studio.models()->isScanning(), 20000);
        for (const QString &kind : {QStringLiteral("sd.cpp"), QStringLiteral("llama.cpp")}) {
            runtime::InstalledRuntime rt;
            rt.id = QStringLiteral("adopted/layout-%1").arg(kind);
            rt.kind = kind;
            rt.directory = files.path();
            rt.executable = files.filePath(kind == QLatin1String("sd.cpp") ? QStringLiteral("sd-server.exe") : QStringLiteral("llama-server.exe"));
            studio.runtimes()->addInstalled(rt);
            auto removeRuntime = qScopeGuard([&] { studio.runtimes()->remove(rt.id); });
            runtime::ServerProfile profile = studio.servers()->createProfile(rt.id, kind == QLatin1String("sd.cpp") ? QStringLiteral("Images")
                                                                                                                  : QStringLiteral("Language"));
            if (kind == QLatin1String("sd.cpp"))
                profile.options = runtime::modelServerOptions(studio.models(), klein, runtime::VramTier::Small);
            ServerProfileDialog d(&studio, profile, true);
            m_auditor.auditDialog(&d, QStringLiteral("Server profile dialog (%1)").arg(kind));
            // sd.cpp with each model type chosen by hand: the fields of that type (plan §17.5).
            if (auto *types = d.findChild<QComboBox *>(QStringLiteral("serverModelType"))) {
                for (int i = 0; i < types->count(); ++i) {
                    types->setCurrentIndex(i);
                    emit types->activated(i);
                    m_auditor.auditDialog(&d, QStringLiteral("Server profile dialog (%1, %2)").arg(kind, types->itemText(i)));
                }
                const int klein4b = types->findData(QStringLiteral("flux2-klein-4b"));
                types->setCurrentIndex(klein4b);
                emit types->activated(klein4b);
            }
            auto *advanced = d.findChild<QToolButton *>(QStringLiteral("serverAdvanced"));
            QVERIFY(advanced && !advanced->isChecked());
            advanced->click();
            m_auditor.auditDialog(&d, QStringLiteral("Server profile dialog (%1) with Advanced").arg(kind));
        }
    }
    {
        pose::Pose pose;
        PoseEditDialog d(&studio, pose, std::nullopt, QString());
        m_auditor.auditDialog(&d, QStringLiteral("Pose edit dialog"));
    }
    {
        NewFromPromptDialog d(&studio);
        m_auditor.auditDialog(&d, QStringLiteral("New from prompt dialog"));
    }
    {
        NewCanvasDialog d(&studio);
        m_auditor.auditDialog(&d, QStringLiteral("New blank canvas dialog"));
    }
    {
        // With everything it can show: a pose ("New image with this pose") and the more options, with
        // the negative prompt of an engine that takes one, whose name is long (the engine list shows
        // it whole).
        ImageEngine engine(&studio, QStringLiteral("stable-diffusion.cpp on the studio computer upstairs"));
        NewFromPromptDialog d(&studio);
        d.setPrompt(QStringLiteral("A lighthouse on a cliff at sunset"), QStringLiteral("text, watermark"));
        pose::Pose pose = pose::findBuiltInPreset(QStringLiteral("standing"))->toPose(QSize(832, 1216));
        d.setPose(pose);
        m_auditor.auditDialog(&d, QStringLiteral("New from prompt dialog with a pose"));
    }
    // The setup wizard's pages (§17.5), for an 8 GB GPU with nothing installed: no hardware probe,
    // no folders read, no ports or GitHub asked.
    {
        studio.providers()->serverProbes.clear();
        studio.runtimes()->feed()->setApiBase(QUrl(QStringLiteral("http://127.0.0.1:9")));
        SetupWizard d(&studio);
        runtime::HardwareInfo hw;
        runtime::GpuInfo gpu;
        gpu.name = QStringLiteral("NVIDIA GeForce RTX 4060 Laptop GPU");
        gpu.vendorId = 0x10DE;
        gpu.dedicatedVram = 8188ull * 1024 * 1024;
        hw.gpus = {gpu};
        d.hardwareOverride = hw;
        d.searchFolders = QStringList();
        QSignalSpy planned(&d, &SetupWizard::localPlanReady);
        const QList<std::pair<SetupWizard::Page, QString>> pages = {{SetupWizard::Page::Choose, QStringLiteral("choose")},
                                                                    {SetupWizard::Page::Local, QStringLiteral("run on this computer")},
                                                                    {SetupWizard::Page::Online, QStringLiteral("online service")},
                                                                    {SetupWizard::Page::Server, QStringLiteral("running server")}};
        for (const auto &[page, name] : pages) {
            d.showPage(page);
            if (page == SetupWizard::Page::Local)
                QTRY_VERIFY_WITH_TIMEOUT(planned.size() == 1, 30000);
            m_auditor.auditDialog(&d, QStringLiteral("Setup wizard - %1").arg(name));
        }
        {
            // With a plugin that needs a language model: the Local page's check box for one and the
            // Characters plugin's model set.
            AllPlugins plugins;
            SetupWizard withPlugins(&studio);
            withPlugins.hardwareOverride = hw;
            withPlugins.searchFolders = QStringList();
            QSignalSpy plannedWith(&withPlugins, &SetupWizard::localPlanReady);
            withPlugins.show(); // it looks at the computer once it shows
            withPlugins.showPage(SetupWizard::Page::Local);
            QTRY_VERIFY_WITH_TIMEOUT(plannedWith.size() == 1, 30000);
            m_auditor.auditDialog(&withPlugins, QStringLiteral("Setup wizard - run on this computer, with plugins"));
        }
        // The result page while the test image is made, and the local setup while it downloads,
        // against a server that takes every request and never answers.
        QTcpServer silent;
        QVERIFY(silent.listen(QHostAddress::LocalHost));
        const QString silentUrl = QStringLiteral("http://127.0.0.1:%1").arg(silent.serverPort());
        ai::Provider *engine = studio.providers()->addProvider(QStringLiteral("sdcpp"), QStringLiteral("stable-diffusion.cpp"),
                                                               {{QStringLiteral("url"), silentUrl}});
        auto removeEngine = qScopeGuard([&] { studio.providers()->removeProvider(engine->instanceId()); });
        d.runTestImage(engine->instanceId(), QStringLiteral("FLUX.2 [klein] 4B is set up"),
                       QStringLiteral("Lumen starts the image engine and makes a test image. The first start loads the model, which takes a "
                                      "minute or two."));
        m_auditor.auditDialog(&d, QStringLiteral("Setup wizard - test image"));

        runtime::InstalledRuntime rt;
        rt.id = QStringLiteral("adopted/layout-audit");
        rt.kind = QStringLiteral("sd.cpp");
        rt.directory = QDir::temp().filePath(QStringLiteral("lumen-layout-audit"));
        rt.executable = QDir(rt.directory).filePath(QStringLiteral("sd-server.exe"));
        studio.runtimes()->addInstalled(rt);
        auto removeRuntime = qScopeGuard([&] { studio.runtimes()->remove(rt.id); });
        studio.hub()->setBaseUrls(silentUrl, silentUrl);
        SetupWizard setting(&studio);
        setting.hardwareOverride = hw;
        setting.searchFolders = QStringList();
        // Small files, so the free disk space never stops the start.
        runtime::ModelPreset klein = *runtime::presetById(QStringLiteral("flux2-klein-4b"));
        klein.download.size = 4LL << 20;
        for (runtime::PresetSlot &slot : klein.components)
            slot.file.size = 1LL << 20;
        setting.presets = {klein, *runtime::presetById(QStringLiteral("sdxl"))};
        QSignalSpy plannedAgain(&setting, &SetupWizard::localPlanReady);
        setting.show();
        setting.showPage(SetupWizard::Page::Local);
        QTRY_VERIFY_WITH_TIMEOUT(plannedAgain.size() == 1, 30000);
        auto *start = setting.findChild<QPushButton *>(QStringLiteral("setupStart"));
        QVERIFY(start && start->isEnabled());
        QList<QString> parts;
        for (const runtime::PlannedFile &f : setting.localPlan().downloads())
            parts << runtime::FileDownload::partPath(studio.hub()->destinationFor(runtime::plannedDownload(setting.localPlan(), f, silentUrl)));
        start->click();
        QTRY_VERIFY_WITH_TIMEOUT(setting.localSetup()->jobs().size() == 3, 10000);
        m_auditor.auditDialog(&setting, QStringLiteral("Setup wizard - setting up"));
        QSignalSpy finished(setting.localSetup(), &LocalSetup::finished);
        studio.jobs()->cancelAll();
        QTRY_VERIFY_WITH_TIMEOUT(finished.size() == 1, 10000);
        for (const QString &part : std::as_const(parts))
            QFile::remove(part);
    }
    // Settings → AI with an engine in each row.
    {
        ai::Provider *engine = studio.providers()->addProvider(QStringLiteral("sdcpp"), QStringLiteral("stable-diffusion.cpp on the other computer"),
                                                               {{QStringLiteral("url"), QStringLiteral("http://127.0.0.1:9")}});
        auto remove = qScopeGuard([&] { studio.providers()->removeProvider(engine->instanceId()); });
        SettingsDialog d(&studio);
        d.showTab(QStringLiteral("ai"));
        d.show();
        QVERIFY(QTest::qWaitForWindowExposed(&d));
        // At the size it gets on a small screen (the dialog's own size is reported above).
        d.resize(d.size().boundedTo(kSmallScreen));
        Auditor::settle();
        m_auditor.audit(&d, QStringLiteral("Settings - AI with an engine"));
        m_auditor.shoot(&d, QStringLiteral("Settings - AI with an engine"));
    }
}

// The node editor (a window of its own) at the smallest window size, with a node selected, and the
// Filters panel at the editor's side-panel width with a filter's sliders shown (M17 §17.6).
void TestLayout::filterTools()
{
    Studio studio;
    studio.openImageAsDocument(sampleImage(), QStringLiteral("Sample"));
    FilterCatalog *catalog = FilterCatalog::of(&studio);
    const QString softFocus = QStringLiteral(":/filters/builtin/soft-focus.lumenfilter");
    // An AI filter too: its instruction (a field of several lines), the colours list, the engine
    // list, and the reason it cannot run (no image engine is set up here).
    const QString colorize = QStringLiteral(":/filters/builtin/colorize-with-ai.lumenfilter");
    struct Shown
    {
        QString path;
        QString node; // selected in the node editor
        QString name; // in the surface's name
    };
    const QList<Shown> shown{{softFocus, QStringLiteral("n3"), QString()}, // the blend: a list and a slider
                             {colorize, QStringLiteral("n2"), QStringLiteral(" AI")}};
    for (const Shown &s : shown) {
        NodeEditorWindow editor(catalog);
        editor.setAttribute(Qt::WA_DeleteOnClose, false);
        editor.setGraph(*catalog->load(s.path));
        editor.resize(windowSizes().first());
        editor.show();
        QVERIFY(QTest::qWaitForWindowExposed(&editor));
        editor.frameAll();
        editor.selectNode(s.node);
        QTRY_VERIFY(!editor.isPreviewPending());
        Auditor::settle();
        const QString surface = QStringLiteral("Node editor%1 1280x720").arg(s.name);
        m_auditor.audit(&editor, surface);
        m_auditor.shoot(&editor, surface);
    }
    for (const Shown &s : shown) {
        // The side panel's minimum width, and its height in a 1280×720 window (the tab's content
        // is about 340 × 540 there).
        EditorState state;
        CanvasView canvas(&state);
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *panel = new FiltersPanel(&studio, &canvas, &host, catalog);
        layout->addWidget(panel);
        panel->selectFilter(s.path);
        host.resize(300, 540);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        Auditor::settle();
        const QString surface = QStringLiteral("Filters panel%1 300x540").arg(s.name);
        m_auditor.audit(&host, surface);
        m_auditor.shoot(&host, surface);
    }
}

// The questions about a picture's file (M17): saving over it with everything it can say, and changes
// not saved to their files when a picture closes or Lumen quits; the start screen with recent files.
void TestLayout::fileQuestions()
{
    {
        const SaveOverQuestion question{QStringLiteral("Holiday at the sea with the whole family, summer 2026.jpg"),
                                        {QStringLiteral("The layers are merged in the file; your project keeps them."),
                                         QStringLiteral("JPEG has no transparency: the transparent parts become white."),
                                         QStringLiteral("JPEG loses a little quality each time it is saved; Lumen keeps the quality the "
                                                        "file had (85 of 100)."),
                                         QStringLiteral("The file has 16 bits per colour; Lumen saves 8, so the finest shades are lost.")}};
        std::unique_ptr<QMessageBox> box(saveOverBox(question, nullptr));
        m_auditor.auditDialog(box.get(), QStringLiteral("Save over question"));
    }
    {
        std::unique_ptr<QMessageBox> box(unsavedChangesBox({QStringLiteral("cat.png")}, false, nullptr));
        m_auditor.auditDialog(box.get(), QStringLiteral("Unsaved changes question (closing)"));
    }
    {
        std::unique_ptr<QMessageBox> box(unsavedChangesBox({QStringLiteral("cat.png"), QStringLiteral("dog.png"),
                                                            QStringLiteral("Holiday at the sea with the whole family.jpg")},
                                                           true, nullptr));
        m_auditor.auditDialog(box.get(), QStringLiteral("Unsaved changes question (quitting)"));
    }
    // The start screen with recent files (long names) next to the project's documents.
    QTemporaryDir dir;
    const QStringList before = Settings::instance().recentFiles();
    auto restore = qScopeGuard([&before] { Settings::instance().setValue(QStringLiteral("app/recentFiles"), before); });
    QStringList files;
    for (int i = 0; i < 4; ++i) {
        const QString path = dir.filePath(QStringLiteral("A picture from disk with a long name %1.png").arg(i + 1));
        QImage im = sampleImage().scaled(600 + 100 * i, 400);
        if (i % 2)
            im.invertPixels();
        QVERIFY(im.save(path));
        files.prepend(path);
    }
    Settings::instance().setValue(QStringLiteral("app/recentFiles"), files);
    Studio studio;
    studio.openProject(dir.filePath(QStringLiteral("files.lumen")));
    QTRY_VERIFY_WITH_TIMEOUT(studio.isProjectOpen(), 5000);
    {
        doc::Document *saved = studio.openImageAsDocument(sampleImage(), QStringLiteral("Saved document"));
        auto done = studio.project()->saveDocument(saved);
        QTRY_VERIFY_WITH_TIMEOUT(done.isFinished(), 5000);
        studio.closeDocument(saved);
    }
    MainWindow window(&studio);
    window.resize(windowSizes().first());
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTRY_VERIFY(window.editor()->showsStartScreen());
    QTRY_COMPARE(window.editor()->startScreen()->recentFileTiles().size(), 4);
    Auditor::settle();
    const QString surface = QStringLiteral("Start screen with recent files 1280x720");
    m_auditor.audit(&window, surface);
    m_auditor.shoot(&window, surface);
}

int main(int argc, char **argv)
{
    // A 1920×1080 screen at 96 dpi, so dialogs size themselves as on a real monitor (the offscreen
    // platform's default screen is 800×800). At 150 % it has 144 dpi: Qt divides its pixels by the
    // scale factor it takes from the dpi (as on Windows), so windows see a 1280×720 screen and paint
    // at a device pixel ratio of 1.5. The path is relative: platform arguments are split at ':',
    // which an absolute Windows path contains.
    const QString config = scaled() ? QStringLiteral("tst_layout_150_screens.json") : QStringLiteral("tst_layout_screens.json");
    QFile f(config);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QStringLiteral(R"({"screens": [{"name": "Audit", "x": 0, "y": 0, "width": 1920, "height": 1080,
                                   "logicalDpi": %1, "logicalBaseDpi": 96, "dpr": 1}]})")
                    .arg(qRound(96 * layoutScale()))
                    .toUtf8());
    f.close();
    qputenv("QT_QPA_PLATFORM", "offscreen:configfile=" + config.toLocal8Bit());
    // The offscreen platform renders with FreeType from a font folder; use the system fonts so text
    // widths are close to the real app's.
#ifdef Q_OS_WIN
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR"))
        qputenv("QT_QPA_FONTDIR", qgetenv("WINDIR") + "\\Fonts");
#endif
    QApplication app(argc, argv);
    QFile::remove(config);
    TestLayout test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}

#include "tst_layout.moc"
