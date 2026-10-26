#include "ui/dialogs/UserGuideDialog.h"

#include "ui/widgets/Controls.h"

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFile>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextDocument>
#include <QUrl>
#include <QVBoxLayout>

namespace lumen::ui {

UserGuideDialog::UserGuideDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("User guide"));
    setObjectName(QStringLiteral("userGuide"));
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 12);
    m_browser = new QTextBrowser(this);
    m_browser->setOpenLinks(false);
    m_browser->setFrameShape(QFrame::NoFrame);
    m_browser->document()->setDocumentMargin(20);
    connect(m_browser, &QTextBrowser::anchorClicked, this, &UserGuideDialog::follow);
    v->addWidget(m_browser, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->setContentsMargins(12, 0, 12, 0);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(buttons);
    showDocument(QStringLiteral("USER_GUIDE.md"));
    resizeWithinScreen(this, QSize(900, 760));
}

void UserGuideDialog::showDocument(const QString &name, const QString &section)
{
    if (name != m_document) {
        QFile f(QStringLiteral(":/docs/") + name);
        if (!f.open(QIODevice::ReadOnly))
            return;
        m_document = name;
        m_browser->setMarkdown(QString::fromUtf8(f.readAll()));
    }
    if (section.isEmpty() || !showSection(section))
        m_browser->verticalScrollBar()->setValue(0);
}

bool UserGuideDialog::showSection(const QString &section)
{
    for (QTextBlock block = m_browser->document()->begin(); block.isValid(); block = block.next()) {
        if (block.blockFormat().headingLevel() > 0 && anchorOf(block.text()) == section) {
            // The heading at the top of the view: to the end first, then back to it.
            m_browser->verticalScrollBar()->setValue(m_browser->verticalScrollBar()->maximum());
            QTextCursor cursor(block);
            m_browser->setTextCursor(cursor);
            m_browser->ensureCursorVisible();
            return true;
        }
    }
    return false;
}

QString UserGuideDialog::anchorOf(const QString &heading)
{
    QString out;
    for (const QChar c : heading.trimmed().toLower()) {
        if (c.isLetterOrNumber() || c == u'-' || c == u'_')
            out += c;
        else if (c == u' ')
            out += u'-';
    }
    return out;
}

void UserGuideDialog::follow(const QUrl &url)
{
    if (url.isRelative()) {
        // "#5-describing-a-change", "PLUGINS.md", "PLUGINS.md#characters", "USER_GUIDE.md".
        const QString path = url.path();
        if (path.isEmpty())
            showSection(url.fragment());
        else if (path == QLatin1String("USER_GUIDE.md") || path == QLatin1String("PLUGINS.md"))
            showDocument(path, url.fragment());
        return;
    }
    QDesktopServices::openUrl(url);
}

} // namespace lumen::ui
