#pragma once

#include <QDialog>

class QTextBrowser;

namespace lumen::ui {

// Help → User guide: docs/USER_GUIDE.md (and PLUGINS.md, which it links to), built into the program,
// so it opens without a Markdown viewer or a network. Links to its own sections, from the contents
// list, scroll there; links to web pages open in the browser.
class UserGuideDialog : public QDialog
{
    Q_OBJECT
public:
    explicit UserGuideDialog(QWidget *parent = nullptr);

    // Shows one of the built-in documents ("USER_GUIDE.md", "PLUGINS.md"), at a section when one is
    // given (the anchor of a heading, as GitHub makes it: "5-describing-a-change").
    void showDocument(const QString &name, const QString &section = QString());
    QString document() const { return m_document; }
    // Scrolls to a heading of the shown document; false when it has none of that name.
    bool showSection(const QString &section);
    QTextBrowser *browser() const { return m_browser; }

    // GitHub's anchor of a heading: lower case, punctuation dropped, spaces as hyphens.
    static QString anchorOf(const QString &heading);

private:
    void follow(const QUrl &url);

    QTextBrowser *m_browser;
    QString m_document;
};

} // namespace lumen::ui
