// Layout audit: opens every page, panel and dialog at common window sizes and reports controls that
// are cut off, squeezed below their minimum size, overlap a sibling, or stick out of their parent.
//
// Report-only by default (the M17 checklist is built from it); LUMEN_LAYOUT_STRICT=1 fails on any
// finding. With LUMEN_SCREENSHOT_DIR, every state is saved as a PNG next to layout-report.md.

#include "core/Settings.h"
#include "pose/Skeleton.h"
#include "runtime/ServerProfile.h"
#include "ui/Accessibility.h"
#include "ui/MainWindow.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/dialogs/LocalServersPage.h"
#include "ui/dialogs/LocalSetupDialog.h"
#include "ui/dialogs/ModelHubDialog.h"
#include "ui/dialogs/PoseEditDialog.h"
#include "ui/dialogs/SettingsDialog.h"

#include <QAbstractButton>
#include <QAbstractScrollArea>
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
    return qobject_cast<const QScrollBar *>(w) || qobject_cast<const QSplitterHandle *>(w)
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

QAbstractButton *railButton(QWidget *window, const QString &name)
{
    for (QAbstractButton *b : window->findChildren<QAbstractButton *>()) {
        if (b->accessibleName() == name)
            return b;
    }
    return nullptr;
}

} // namespace

class TestLayout : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();
    void mainWindow();
    void settingsDialog();
    void dialogs();

private:
    Auditor m_auditor;
};

void TestLayout::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
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

void TestLayout::mainWindow()
{
    Studio studio;
    studio.openImageAsDocument(sampleImage(), QStringLiteral("Sample"));
    MainWindow window(&studio);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    const QList<std::pair<Page, QString>> pages = {{Page::Create, QStringLiteral("Create")},   {Page::Generate, QStringLiteral("Generate")},
                                                   {Page::Editor, QStringLiteral("Edit")},     {Page::Poses, QStringLiteral("Poses")},
                                                   {Page::Parts, QStringLiteral("Parts")},     {Page::Library, QStringLiteral("Library")}};
    for (const QSize &size : kWindowSizes) {
        window.resize(size);
        const QString res = QStringLiteral("%1x%2").arg(size.width()).arg(size.height());
        for (const auto &[page, name] : pages) {
            window.showPage(page);
            Auditor::settle();
            const QString surface = QStringLiteral("%1 page %2").arg(name, res);
            m_auditor.audit(&window, surface);
            m_auditor.shoot(&window, surface);
        }
        // The editor with the queue and the assistant open over it.
        window.showPage(Page::Editor);
        for (const QString &panel : {QStringLiteral("Queue"), QStringLiteral("Assistant")}) {
            QAbstractButton *b = railButton(&window, panel);
            QVERIFY2(b, qPrintable(panel));
            b->click();
            Auditor::settle();
            const QString surface = QStringLiteral("Edit page with %1 %2").arg(panel.toLower(), res);
            m_auditor.audit(&window, surface);
            m_auditor.shoot(&window, surface);
            b->click();
            Auditor::settle();
        }
    }
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
