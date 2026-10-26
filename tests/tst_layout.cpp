// Layout audit: opens the main window (the start screen and the editor), every plugin page's window,
// the panels and the dialogs at common window sizes and reports controls that are cut off, squeezed
// below their minimum size, overlap a sibling, or stick out of their parent.
//
// Report-only by default (the M17 checklist is built from it); LUMEN_LAYOUT_STRICT=1 fails on any
// finding. With LUMEN_SCREENSHOT_DIR, every state is saved as a PNG next to layout-report.md.

#include "core/Settings.h"
#include "doc/Project.h"
#include "plugins/BuiltInPlugins.h"
#include "pose/PosePresets.h"
#include "pose/Skeleton.h"
#include "runtime/ServerProfile.h"
#include "ui/Accessibility.h"
#include "ui/MainWindow.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/dialogs/LocalServersPage.h"
#include "ui/dialogs/LocalSetupDialog.h"
#include "ui/dialogs/ModelHubDialog.h"
#include "ui/dialogs/NewFromPromptDialog.h"
#include "ui/dialogs/PoseEditDialog.h"
#include "ui/dialogs/SettingsDialog.h"
#include "ui/ext/Extension.h"
#include "core/ImageOps.h"
#include "ui/pages/EditorPage.h"
#include "ui/pages/PromptBar.h"
#include "ui/pages/StartScreen.h"
#include "ui/pages/VariantStrip.h"

#include <QAbstractButton>
#include <QAbstractScrollArea>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QLabel>
#include <QLayout>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollBar>
#include <QSplitter>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextDocumentFragment>
#include <QToolButton>
#include <QtTest>

using namespace lumen;
using namespace lumen::ui;

namespace {

// Logical sizes: 1920×1080 at 150 % and 125 %, and at 100 %.
const QList<QSize> kWindowSizes = {QSize(1280, 720), QSize(1536, 864), QSize(1920, 1080)};
// The smallest screen a dialog must fit on (1920×1080 at 150 %, minus the taskbar).
const QSize kSmallScreen(1280, 680);

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
                const int textWidth = b->fontMetrics().horizontalAdvance(b->text().remove(QLatin1Char('&')));
                if (textShown && (b->sizeHint().width() > b->width() + 2 || textWidth > b->width()))
                    add(QStringLiteral("text cut"), w, QStringLiteral("needs %1 px width, has %2").arg(b->sizeHint().width()).arg(b->width()));
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

    // A dialog: reports when its natural size does not fit a small screen, then audits it at the size
    // it would get there.
    void auditDialog(QWidget *dialog, const QString &surface)
    {
        dialog->show();
        QVERIFY(QTest::qWaitForWindowExposed(dialog));
        QTest::qWait(20);
        const QSize natural = dialog->size().expandedTo(dialog->minimumSizeHint());
        if (natural.width() > kSmallScreen.width() || natural.height() > kSmallScreen.height())
            m_findings << Finding{surface, QStringLiteral("larger than screen"), QLatin1String(dialog->metaObject()->className()),
                                  QStringLiteral("%1×%2 does not fit a %3×%4 screen")
                                      .arg(natural.width()).arg(natural.height()).arg(kSmallScreen.width()).arg(kSmallScreen.height())};
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

    static void settle()
    {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        QTest::qWait(30);
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
    explicit ImageEngine(Studio *studio)
        : m_studio(studio)
    {
        m_id = studio->providers()
                   ->addProvider(QStringLiteral("sdcpp"), QStringLiteral("Local image server"), {{QStringLiteral("url"), QStringLiteral("http://127.0.0.1:9")}})
                   ->instanceId();
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
        results << im;
    }
    window.editor()->variantStrip()->addVariants(doc, results, QStringLiteral("a long description of what should change in the picture"));
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
    void mainWindow();
    void mainWindowFitsSmallScreen();
    void settingsDialog();
    void dialogs();

private:
    Auditor m_auditor;
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
    const QString shots = qEnvironmentVariable("LUMEN_SCREENSHOT_DIR");
    if (!shots.isEmpty()) {
        const QString dir = QDir(shots).filePath(QStringLiteral("layout"));
        QDir().mkpath(dir);
        m_auditor.setShotDir(dir);
    }
}

void TestLayout::cleanupTestCase()
{
    const QString report = m_auditor.report();
    const QString shots = qEnvironmentVariable("LUMEN_SCREENSHOT_DIR");
    if (!shots.isEmpty()) {
        QFile f(QDir(shots).filePath(QStringLiteral("layout/layout-report.md")));
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(report.toUtf8());
    }
    QMap<QString, int> byKind;
    for (const Finding &f : m_auditor.findings())
        ++byKind[f.kind];
    for (auto it = byKind.cbegin(); it != byKind.cend(); ++it)
        qInfo("%s: %d", qPrintable(it.key()), it.value());
    if (qEnvironmentVariableIntValue("LUMEN_LAYOUT_STRICT"))
        QVERIFY2(m_auditor.findings().isEmpty(), qPrintable(report));
}

// The main window (the start screen, the editor with a document, the queue and the assistant over
// it), the plugin pages' windows and Browse project images, at each window size.
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
    for (const QSize &size : kWindowSizes) {
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
        auditWindow(QStringLiteral("Editor"));
        {
            ImageEngine engine(&studio);
            PromptBar *bar = window.editor()->promptBar();
            QTRY_VERIFY(bar->plan().action == PromptAction::CannotEditImage);
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
            showResults(window, doc);
            QVERIFY(window.editor()->variantStrip()->isVisible());
            auditWindow(QStringLiteral("Editor with results"));
            bar->showOptions();
            QWidget *popup = bar->optionsPopup();
            QVERIFY(QTest::qWaitForWindowExposed(popup));
            Auditor::settle();
            m_auditor.audit(popup, QStringLiteral("Prompt bar options %1").arg(res));
            m_auditor.shoot(popup, QStringLiteral("Prompt bar options %1").arg(res));
            popup->hide();
            window.editor()->showAiPanel();
            auditWindow(QStringLiteral("Editor with the AI panel"));
            window.editor()->hideAiPanel();
            window.editor()->variantStrip()->discard();
            doc->clearSelection();
        }
        window.setQueueVisible(true);
        auditWindow(QStringLiteral("Editor with the queue"));
        window.setQueueVisible(false);
        QAction *assistant = actionNamed(&window, QStringLiteral("panel-assistant"));
        QVERIFY(assistant);
        assistant->setChecked(true);
        auditWindow(QStringLiteral("Editor with the assistant"));
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
// with a document (its prompt bar, results and AI panel), and with the queue and the assistant open;
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
    Studio studio;
    SettingsDialog dialog(&studio);
    m_auditor.auditDialog(&dialog, QStringLiteral("Settings"));
    auto *tabs = dialog.findChild<QTabWidget *>();
    QVERIFY(tabs);
    for (int i = 0; i < tabs->count(); ++i) {
        tabs->setCurrentIndex(i);
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
    for (const auto &[runtime, name] : {std::pair{QStringLiteral("sd.cpp/test"), QStringLiteral("sd.cpp")},
                                        std::pair{QStringLiteral("llama.cpp/test"), QStringLiteral("llama.cpp")}}) {
        runtime::ServerProfile profile;
        profile.id = QStringLiteral("audit");
        profile.name = name;
        profile.runtimeId = runtime;
        profile.port = 7861;
        ServerProfileDialog d(&studio, profile, true);
        m_auditor.auditDialog(&d, QStringLiteral("Server profile dialog (%1)").arg(name));
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
        // With everything it can show: a pose ("New image with this pose") and the more options.
        NewFromPromptDialog d(&studio);
        d.setPrompt(QStringLiteral("A lighthouse on a cliff at sunset"), QStringLiteral("text, watermark"));
        pose::Pose pose = pose::findBuiltInPreset(QStringLiteral("standing"))->toPose(QSize(832, 1216));
        d.setPose(pose);
        m_auditor.auditDialog(&d, QStringLiteral("New from prompt dialog with a pose"));
    }
}

int main(int argc, char **argv)
{
    // A 1920×1080 screen at 96 dpi, so dialogs size themselves as on a real monitor (the offscreen
    // platform's default screen is 800×800). The path is relative: platform arguments are split at
    // ':', which an absolute Windows path contains.
    const QString config = QStringLiteral("tst_layout_screens.json");
    QFile f(config);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(R"({"screens": [{"name": "Audit", "x": 0, "y": 0, "width": 1920, "height": 1080,
                    "logicalDpi": 96, "logicalBaseDpi": 96, "dpr": 1}]})");
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
