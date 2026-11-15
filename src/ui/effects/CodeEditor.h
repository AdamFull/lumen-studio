#pragma once

#include <QHash>
#include <QPlainTextEdit>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>

class QCompleter;
class QStringListModel;

namespace lumen::ui {

// Colours for Slang source (plan M20 step 7, the effect editor): keywords, types, built-in and
// Lumen functions, Lumen's attributes, numbers, strings, comments and preprocessor lines, in the
// theme's light or dark colours. Block comments carry over from line to line.
class SlangHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT
public:
    explicit SlangHighlighter(QTextDocument *document);

    // What each kind of word is called in tests and the reference: "keyword", "type", "function",
    // "lumen", "attribute", "number", "string", "comment", "preprocessor"; empty for none.
    QString kindAt(int block, int column) const;

    static const QStringList &keywords();
    static const QStringList &types();
    static const QStringList &builtinFunctions();
    static const QStringList &lumenFunctions();
    static const QStringList &lumenAttributes();

    // Refreshes the colours from the theme.
    void updateColours();

protected:
    void highlightBlock(const QString &text) override;

private:
    QHash<QString, QTextCharFormat> m_formats; // by kind
};

// What a word means, shown on hover and in the reference: its form and a sentence. Empty for an
// unknown word.
struct CodeDoc
{
    QString signature;
    QString text;
};
CodeDoc codeDoc(const QString &word);
// Every documented word, by kind ("attribute", "lumen", "function").
QStringList documentedWords(const QString &kind);

// A problem in the source, from the compiler or Lumen.
struct CodeProblem
{
    int line = 0;   // 1-based
    int column = 0; // 1-based
    int endLine = 0;
    int endColumn = 0;
    bool error = true; // else a warning
    QString text;
};

// A code editor for Slang: line numbers with error and warning marks, the current line and matching
// brackets shown, problems underlined with their text on hover (and a word's documentation), indent
// kept on Enter and changed with Tab and Shift+Tab, Ctrl+/ for comments, brackets and quotes closed
// as they are typed, completion (Ctrl+Space, or after three letters) from Slang, Lumen and the
// words of the source, find and replace, go to line, and zoom (Ctrl+wheel).
class CodeEditor : public QPlainTextEdit
{
    Q_OBJECT
public:
    explicit CodeEditor(QWidget *parent = nullptr);

    void setProblems(const QList<CodeProblem> &problems);
    const QList<CodeProblem> &problems() const { return m_problems; }
    // The problem at a place in the text (for hover), or nullptr.
    const CodeProblem *problemAt(int line, int column) const;
    // Moves to the next (or previous) problem after the cursor, round to the first; false when
    // there are none.
    bool nextProblem(bool backwards = false);

    void goToLine(int line, int column = 1);
    int currentLine() const;
    int currentColumn() const;

    void toggleComment();
    void indentSelection(bool out);

    // Completion: the words for a prefix, and the popup.
    QStringList completions(const QString &prefix) const;
    void complete();
    bool isCompleting() const;

    // Find: the next match from the cursor (wrapping round); replace the selected match and find
    // the next; replace all, returning how many.
    bool find(const QString &text, bool backwards = false, bool caseSensitive = false, bool wholeWords = false);
    bool replaceNext(const QString &text, const QString &with, bool caseSensitive = false, bool wholeWords = false);
    int replaceAll(const QString &text, const QString &with, bool caseSensitive = false, bool wholeWords = false);

    void zoom(int steps); // 0 resets
    int gutterWidth() const;
    SlangHighlighter *highlighter() const { return m_highlighter; }

signals:
    void cursorMoved(int line, int column);

protected:
    bool event(QEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void changeEvent(QEvent *e) override;

private:
    class Gutter;
    friend class Gutter;
    void paintGutter(QPaintEvent *e);
    void updateMarks();
    QString wordBeforeCursor() const;
    QString wordAt(const QPoint &pos) const;
    void insertCompletion(const QString &word);
    bool handleIndent(QKeyEvent *e);
    bool handlePairs(QKeyEvent *e);

    Gutter *m_gutter;
    SlangHighlighter *m_highlighter;
    QCompleter *m_completer;
    QStringListModel *m_words;
    QList<CodeProblem> m_problems;
    double m_baseSize = 10.0;
    int m_zoom = 0;
};

} // namespace lumen::ui
