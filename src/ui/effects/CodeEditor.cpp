#include "ui/effects/CodeEditor.h"

#include "ui/Theme.h"

#include <QAbstractItemView>
#include <QCompleter>
#include <QFontDatabase>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QStringListModel>
#include <QTextBlock>
#include <QToolTip>

#include <algorithm>

namespace lumen::ui {

namespace {

struct Text
{
    Q_DECLARE_TR_FUNCTIONS(lumen::ui::CodeEditor)
};

// Block states: inside a /* */ comment or not.
enum State { Normal = 0, InComment = 1 };

const QRegularExpression &wordPattern()
{
    static const QRegularExpression re(QStringLiteral("[A-Za-z_][A-Za-z0-9_]*"));
    return re;
}

bool isWordChar(QChar c)
{
    return c.isLetterOrNumber() || c == u'_';
}

} // namespace

// ---- Words --------------------------------------------------------------------------------------

const QStringList &SlangHighlighter::keywords()
{
    static const QStringList words{
        QStringLiteral("import"),   QStringLiteral("module"),    QStringLiteral("implementing"), QStringLiteral("struct"),
        QStringLiteral("return"),   QStringLiteral("if"),        QStringLiteral("else"),         QStringLiteral("for"),
        QStringLiteral("while"),    QStringLiteral("do"),        QStringLiteral("break"),        QStringLiteral("continue"),
        QStringLiteral("switch"),   QStringLiteral("case"),      QStringLiteral("default"),      QStringLiteral("const"),
        QStringLiteral("static"),   QStringLiteral("uniform"),   QStringLiteral("in"),           QStringLiteral("out"),
        QStringLiteral("inout"),    QStringLiteral("void"),      QStringLiteral("true"),         QStringLiteral("false"),
        QStringLiteral("public"),   QStringLiteral("private"),   QStringLiteral("internal"),     QStringLiteral("let"),
        QStringLiteral("var"),      QStringLiteral("func"),      QStringLiteral("typedef"),      QStringLiteral("interface"),
        QStringLiteral("extension"), QStringLiteral("namespace"), QStringLiteral("export"),      QStringLiteral("discard"),
        QStringLiteral("groupshared"), QStringLiteral("inline"), QStringLiteral("typealias"),    QStringLiteral("enum"),
    };
    return words;
}

const QStringList &SlangHighlighter::types()
{
    static const QStringList words = [] {
        QStringList out{QStringLiteral("RWTexture2D"),  QStringLiteral("Texture2D"),       QStringLiteral("SamplerState"),
                        QStringLiteral("ConstantBuffer"), QStringLiteral("StructuredBuffer"), QStringLiteral("RWStructuredBuffer"),
                        QStringLiteral("Params")};
        for (const char *base : {"float", "half", "int", "uint", "bool", "double"}) {
            const QString b = QLatin1String(base);
            out << b;
            for (int n = 2; n <= 4; ++n) {
                out << b + QString::number(n);
                for (int m = 2; m <= 4; ++m)
                    out << b + QStringLiteral("%1x%2").arg(n).arg(m);
            }
        }
        return out;
    }();
    return words;
}

const QStringList &SlangHighlighter::builtinFunctions()
{
    static const QStringList words{
        QStringLiteral("abs"),   QStringLiteral("acos"),  QStringLiteral("all"),       QStringLiteral("any"),       QStringLiteral("asin"),
        QStringLiteral("atan"),  QStringLiteral("atan2"), QStringLiteral("ceil"),      QStringLiteral("clamp"),     QStringLiteral("cos"),
        QStringLiteral("cross"), QStringLiteral("degrees"), QStringLiteral("distance"), QStringLiteral("dot"),      QStringLiteral("exp"),
        QStringLiteral("exp2"),  QStringLiteral("floor"), QStringLiteral("fmod"),      QStringLiteral("frac"),      QStringLiteral("length"),
        QStringLiteral("lerp"),  QStringLiteral("log"),   QStringLiteral("log2"),      QStringLiteral("max"),       QStringLiteral("min"),
        QStringLiteral("normalize"), QStringLiteral("pow"), QStringLiteral("radians"), QStringLiteral("round"),    QStringLiteral("rsqrt"),
        QStringLiteral("saturate"), QStringLiteral("sign"), QStringLiteral("sin"),     QStringLiteral("smoothstep"), QStringLiteral("sqrt"),
        QStringLiteral("step"),  QStringLiteral("tan"),   QStringLiteral("trunc"),     QStringLiteral("GetDimensions"),
    };
    return words;
}

const QStringList &SlangHighlighter::lumenFunctions()
{
    static const QStringList words{
        QStringLiteral("toSrgb"),   QStringLiteral("toLinear"),   QStringLiteral("luma"),    QStringLiteral("toOklab"),
        QStringLiteral("fromOklab"), QStringLiteral("wheelDirection"), QStringLiteral("bilinear"), QStringLiteral("bilinearAt"),
        QStringLiteral("blockOf"),  QStringLiteral("boxDown"),    QStringLiteral("gaussian"),
    };
    return words;
}

const QStringList &SlangHighlighter::lumenAttributes()
{
    static const QStringList words{
        QStringLiteral("Effect"), QStringLiteral("Label"), QStringLiteral("Range"), QStringLiteral("Default"),
        QStringLiteral("Unit"),   QStringLiteral("Section"), QStringLiteral("Control"), QStringLiteral("Input"),
        QStringLiteral("Scale"),  QStringLiteral("Fit"),   QStringLiteral("Size"),  QStringLiteral("Over"),
        QStringLiteral("shader"), QStringLiteral("numthreads"), QStringLiteral("format"), QStringLiteral("vk"),
        QStringLiteral("binding"), QStringLiteral("push_constant"),
    };
    return words;
}

CodeDoc codeDoc(const QString &word)
{
    static const QHash<QString, CodeDoc> docs = [] {
        QHash<QString, CodeDoc> d;
        // Lumen's attributes (src/gpu/shaders/effect.slang).
        d.insert(QStringLiteral("Effect"), {QStringLiteral("[Effect(\"title\", \"group\")]"),
                                            Text::tr("On the first pass: the effect's name, as the filter editor shows it.")});
        d.insert(QStringLiteral("Label"), {QStringLiteral("[Label(\"text\")]"),
                                           Text::tr("A setting's name on its slider. Settings without one are not shown.")});
        d.insert(QStringLiteral("Range"), {QStringLiteral("[Range(minimum, maximum)]"), Text::tr("The slider's ends.")});
        d.insert(QStringLiteral("Default"), {QStringLiteral("[Default(value)]"),
                                             Text::tr("Where the slider starts; 0, or the nearest end of its range, without one.")});
        d.insert(QStringLiteral("Unit"), {QStringLiteral("[Unit(\"text\")]"), Text::tr("Shown after the slider's value.")});
        d.insert(QStringLiteral("Section"), {QStringLiteral("[Section(\"text\")]"),
                                             Text::tr("A heading for this setting and the ones after it.")});
        d.insert(QStringLiteral("Control"), {QStringLiteral("[Control(\"wheel\")]"),
                                             Text::tr("A float2 set on a colour wheel: hue in degrees, saturation in %.")});
        d.insert(QStringLiteral("Input"), {QStringLiteral("[Input(\"curve\" | \"lut\")]"),
                                           Text::tr("A texture the effect reads besides the picture: tone curves or a 3D LUT.")});
        d.insert(QStringLiteral("Scale"), {QStringLiteral("[Scale(n)]"),
                                           Text::tr("A texture of the effect's own, n times smaller than the picture.")});
        d.insert(QStringLiteral("Fit"), {QStringLiteral("[Fit(pixels)]"),
                                         Text::tr("A texture of the effect's own whose long side is so many pixels: a blur on it covers "
                                                  "the same part of any picture, so the preview looks like the full size.")});
        d.insert(QStringLiteral("Size"), {QStringLiteral("[Size(width, height)]"),
                                          Text::tr("A texture of the effect's own of a fixed size (1, 1 for one value).")});
        d.insert(QStringLiteral("Over"), {QStringLiteral("[Over(\"texture\")]"),
                                          Text::tr("A pass that runs over one of the effect's textures instead of the picture.")});
        d.insert(QStringLiteral("numthreads"), {QStringLiteral("[numthreads(x, y, z)]"),
                                                Text::tr("Threads per group: 16, 16, 1 for a pass over pixels.")});
        d.insert(QStringLiteral("shader"), {QStringLiteral("[shader(\"compute\")]"),
                                            Text::tr("Makes a function a pass; the passes run in the file's order.")});
        // Lumen's modules (colorspace, sampling).
        d.insert(QStringLiteral("toSrgb"), {QStringLiteral("float3 toSrgb(float3 linear)"),
                                            Text::tr("Linear light to display values (the sRGB curve); above 1 it goes on in a line. "
                                                     "import colorspace;")});
        d.insert(QStringLiteral("toLinear"), {QStringLiteral("float3 toLinear(float3 display)"),
                                              Text::tr("Display values to linear light. import colorspace;")});
        d.insert(QStringLiteral("luma"), {QStringLiteral("float luma(float3 linear)"),
                                          Text::tr("The luminance of linear light (Rec. 709). import colorspace;")});
        d.insert(QStringLiteral("toOklab"), {QStringLiteral("float3 toOklab(float3 linear)"),
                                             Text::tr("OKLab: lightness and two colour axes spaced as people see them. import colorspace;")});
        d.insert(QStringLiteral("fromOklab"), {QStringLiteral("float3 fromOklab(float3 lab)"),
                                               Text::tr("OKLab back to linear light. import colorspace;")});
        d.insert(QStringLiteral("wheelDirection"), {QStringLiteral("float2 wheelDirection(float degrees)"),
                                                    Text::tr("The OKLab direction of a colour wheel's hue. import colorspace;")});
        d.insert(QStringLiteral("bilinear"), {QStringLiteral("float4 bilinear(RWTexture2D<float4> t, float2 at)"),
                                              Text::tr("A texture between its pixels (centres at .5), the edges held. import sampling;")});
        d.insert(QStringLiteral("bilinearAt"), {QStringLiteral("float4 bilinearAt(RWTexture2D<float4> t, uint2 pixel, uint2 size)"),
                                                Text::tr("The same place in a texture of another size: a smaller copy read back up. "
                                                         "import sampling;")});
        d.insert(QStringLiteral("boxDown"), {QStringLiteral("float4 boxDown(RWTexture2D<float4> from, uint2 at, uint2 size)"),
                                             Text::tr("The average of the pixels of `from` that fall into one pixel of a smaller "
                                                      "texture. import sampling;")});
        d.insert(QStringLiteral("blockOf"), {QStringLiteral("void blockOf(from, at, size, out uint2 first, out uint2 count)"),
                                             Text::tr("Which pixels of `from` fall into one pixel of a smaller texture. import sampling;")});
        d.insert(QStringLiteral("gaussian"), {QStringLiteral("float4 gaussian(RWTexture2D<float4> t, uint2 at, int2 direction, float sigma)"),
                                              Text::tr("A Gaussian blur along one direction: across into one texture, then down into "
                                                       "another. import sampling;")});
        // Slang's own, the ones effects use most.
        d.insert(QStringLiteral("lerp"), {QStringLiteral("lerp(a, b, t)"), Text::tr("a + (b − a) × t: from a at 0 to b at 1.")});
        d.insert(QStringLiteral("saturate"), {QStringLiteral("saturate(x)"), Text::tr("x kept between 0 and 1.")});
        d.insert(QStringLiteral("clamp"), {QStringLiteral("clamp(x, low, high)"), Text::tr("x kept between low and high.")});
        d.insert(QStringLiteral("smoothstep"), {QStringLiteral("smoothstep(edge0, edge1, x)"),
                                                Text::tr("0 below edge0, 1 above edge1, a smooth S between.")});
        d.insert(QStringLiteral("step"), {QStringLiteral("step(edge, x)"), Text::tr("0 where x < edge, else 1.")});
        d.insert(QStringLiteral("pow"), {QStringLiteral("pow(x, y)"), Text::tr("x to the power y (x not below 0).")});
        d.insert(QStringLiteral("exp2"), {QStringLiteral("exp2(x)"), Text::tr("2 to the power x: exposure in stops.")});
        d.insert(QStringLiteral("dot"), {QStringLiteral("dot(a, b)"), Text::tr("The dot product.")});
        d.insert(QStringLiteral("length"), {QStringLiteral("length(v)"), Text::tr("A vector's length.")});
        d.insert(QStringLiteral("frac"), {QStringLiteral("frac(x)"), Text::tr("x minus its floor: the part after the point.")});
        d.insert(QStringLiteral("any"), {QStringLiteral("any(v)"), Text::tr("True when any component is true.")});
        d.insert(QStringLiteral("GetDimensions"), {QStringLiteral("t.GetDimensions(out uint width, out uint height)"),
                                                   Text::tr("A texture's size.")});
        return d;
    }();
    return docs.value(word);
}

QStringList documentedWords(const QString &kind)
{
    QStringList out;
    const QStringList &from = kind == QLatin1String("attribute") ? SlangHighlighter::lumenAttributes()
                              : kind == QLatin1String("lumen")   ? SlangHighlighter::lumenFunctions()
                                                                 : SlangHighlighter::builtinFunctions();
    for (const QString &w : from) {
        if (!codeDoc(w).signature.isEmpty())
            out << w;
    }
    return out;
}

// ---- Highlighter --------------------------------------------------------------------------------

SlangHighlighter::SlangHighlighter(QTextDocument *document)
    : QSyntaxHighlighter(document)
{
    updateColours();
}

void SlangHighlighter::updateColours()
{
    const bool dark = Theme::palette().dark;
    auto format = [](const QColor &c, bool bold = false, bool italic = false) {
        QTextCharFormat f;
        f.setForeground(c);
        if (bold)
            f.setFontWeight(QFont::DemiBold);
        f.setFontItalic(italic);
        return f;
    };
    m_formats.insert(QStringLiteral("keyword"), format(dark ? QColor(0xc5, 0x86, 0xc0) : QColor(0xaf, 0x00, 0xdb), true));
    m_formats.insert(QStringLiteral("type"), format(dark ? QColor(0x4e, 0xc9, 0xb0) : QColor(0x26, 0x7f, 0x99)));
    m_formats.insert(QStringLiteral("function"), format(dark ? QColor(0xdc, 0xdc, 0xaa) : QColor(0x79, 0x5e, 0x26)));
    m_formats.insert(QStringLiteral("lumen"), format(dark ? QColor(0x9c, 0xdc, 0xfe) : QColor(0x00, 0x70, 0xc1), true));
    m_formats.insert(QStringLiteral("attribute"), format(dark ? QColor(0xd7, 0xba, 0x7d) : QColor(0x8a, 0x5d, 0x00)));
    m_formats.insert(QStringLiteral("number"), format(dark ? QColor(0xb5, 0xce, 0xa8) : QColor(0x09, 0x86, 0x58)));
    m_formats.insert(QStringLiteral("string"), format(dark ? QColor(0xce, 0x91, 0x78) : QColor(0xa3, 0x15, 0x15)));
    m_formats.insert(QStringLiteral("comment"), format(dark ? QColor(0x7c, 0xa6, 0x68) : QColor(0x00, 0x80, 0x00), false, true));
    m_formats.insert(QStringLiteral("preprocessor"), format(dark ? QColor(0xc5, 0x86, 0xc0) : QColor(0xaf, 0x00, 0xdb)));
    // Each format says its kind (kindAt, tests).
    for (auto it = m_formats.begin(); it != m_formats.end(); ++it)
        it.value().setProperty(QTextFormat::UserProperty, it.key());
    rehighlight();
}

void SlangHighlighter::highlightBlock(const QString &text)
{
    static const QSet<QString> keywordSet(keywords().cbegin(), keywords().cend());
    static const QSet<QString> typeSet(types().cbegin(), types().cend());
    static const QSet<QString> builtinSet(builtinFunctions().cbegin(), builtinFunctions().cend());
    static const QSet<QString> lumenSet(lumenFunctions().cbegin(), lumenFunctions().cend());
    static const QSet<QString> attributeSet(lumenAttributes().cbegin(), lumenAttributes().cend());

    const int n = int(text.size());
    int i = 0;
    setCurrentBlockState(Normal);
    if (previousBlockState() == InComment) {
        const int end = int(text.indexOf(QStringLiteral("*/")));
        if (end < 0) {
            setFormat(0, n, m_formats.value(QStringLiteral("comment")));
            setCurrentBlockState(InComment);
            return;
        }
        setFormat(0, end + 2, m_formats.value(QStringLiteral("comment")));
        i = end + 2;
    }
    // A preprocessor line.
    if (text.trimmed().startsWith(u'#')) {
        setFormat(0, n, m_formats.value(QStringLiteral("preprocessor")));
        return;
    }
    int bracketDepth = 0; // inside [ ]: attribute names
    while (i < n) {
        const QChar c = text.at(i);
        if (c == u'/' && i + 1 < n && text.at(i + 1) == u'/') {
            setFormat(i, n - i, m_formats.value(QStringLiteral("comment")));
            return;
        }
        if (c == u'/' && i + 1 < n && text.at(i + 1) == u'*') {
            const int end = int(text.indexOf(QStringLiteral("*/"), i + 2));
            if (end < 0) {
                setFormat(i, n - i, m_formats.value(QStringLiteral("comment")));
                setCurrentBlockState(InComment);
                return;
            }
            setFormat(i, end + 2 - i, m_formats.value(QStringLiteral("comment")));
            i = end + 2;
            continue;
        }
        if (c == u'"') {
            int j = i + 1;
            while (j < n && text.at(j) != u'"') {
                if (text.at(j) == u'\\')
                    ++j;
                ++j;
            }
            setFormat(i, std::min(n, j + 1) - i, m_formats.value(QStringLiteral("string")));
            i = j + 1;
            continue;
        }
        if (c.isDigit() || (c == u'.' && i + 1 < n && text.at(i + 1).isDigit())) {
            int j = i;
            while (j < n && (text.at(j).isLetterOrNumber() || text.at(j) == u'.'
                             || ((text.at(j) == u'+' || text.at(j) == u'-') && j > i && text.at(j - 1).toLower() == u'e')))
                ++j;
            setFormat(i, j - i, m_formats.value(QStringLiteral("number")));
            i = j;
            continue;
        }
        if (c == u'[') {
            ++bracketDepth;
            ++i;
            continue;
        }
        if (c == u']') {
            bracketDepth = std::max(0, bracketDepth - 1);
            ++i;
            continue;
        }
        if (c.isLetter() || c == u'_') {
            int j = i;
            while (j < n && isWordChar(text.at(j)))
                ++j;
            const QString word = text.mid(i, j - i);
            QString kind;
            if (bracketDepth > 0 && attributeSet.contains(word))
                kind = QStringLiteral("attribute");
            else if (keywordSet.contains(word))
                kind = QStringLiteral("keyword");
            else if (typeSet.contains(word))
                kind = QStringLiteral("type");
            else if (lumenSet.contains(word))
                kind = QStringLiteral("lumen");
            else if (builtinSet.contains(word))
                kind = QStringLiteral("function");
            if (!kind.isEmpty())
                setFormat(i, j - i, m_formats.value(kind));
            i = j;
            continue;
        }
        ++i;
    }
}

QString SlangHighlighter::kindAt(int blockNumber, int column) const
{
    const QTextBlock block = document()->findBlockByNumber(blockNumber);
    if (!block.isValid() || !block.layout())
        return {};
    for (const QTextLayout::FormatRange &r : block.layout()->formats()) {
        if (column >= r.start && column < r.start + r.length)
            return r.format.property(QTextFormat::UserProperty).toString();
    }
    return {};
}

// ---- Editor ---------------------------------------------------------------------------------------

class CodeEditor::Gutter : public QWidget
{
public:
    explicit Gutter(CodeEditor *editor)
        : QWidget(editor)
        , m_editor(editor)
    {
        setMouseTracking(true);
    }

protected:
    void paintEvent(QPaintEvent *e) override { m_editor->paintGutter(e); }
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::ToolTip) {
            auto *help = static_cast<QHelpEvent *>(e);
            const QTextCursor c = m_editor->cursorForPosition(QPoint(0, help->pos().y()));
            const int line = c.blockNumber() + 1;
            QStringList texts;
            for (const CodeProblem &p : m_editor->m_problems) {
                if (p.line == line)
                    texts << p.text;
            }
            if (!texts.isEmpty())
                QToolTip::showText(help->globalPos(), texts.join(u'\n'), this);
            else
                QToolTip::hideText();
            return true;
        }
        return QWidget::event(e);
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        // A click on a line number selects the line.
        QTextCursor c = m_editor->cursorForPosition(QPoint(0, int(e->position().y())));
        c.movePosition(QTextCursor::StartOfBlock);
        c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        m_editor->setTextCursor(c);
    }

private:
    CodeEditor *m_editor;
};

CodeEditor::CodeEditor(QWidget *parent)
    : QPlainTextEdit(parent)
    , m_gutter(new Gutter(this))
    , m_highlighter(new SlangHighlighter(document()))
    , m_completer(new QCompleter(this))
    , m_words(new QStringListModel(this))
{
    setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_baseSize = std::max(10.0, font().pointSizeF());
    mono.setPointSizeF(m_baseSize);
    setFont(mono);
    setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);
    setCursorWidth(2);

    m_completer->setModel(m_words);
    m_completer->setWidget(this);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setModelSorting(QCompleter::UnsortedModel);
    m_completer->setMaxVisibleItems(10);
    m_completer->popup()->setObjectName(QStringLiteral("codeCompletions"));
    connect(m_completer, qOverload<const QString &>(&QCompleter::activated), this, &CodeEditor::insertCompletion);

    connect(this, &QPlainTextEdit::blockCountChanged, this, [this] { setViewportMargins(gutterWidth(), 0, 0, 0); });
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect &rect, int dy) {
        if (dy)
            m_gutter->scroll(0, dy);
        else
            m_gutter->update(0, rect.y(), m_gutter->width(), rect.height());
    });
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        updateMarks();
        emit cursorMoved(currentLine(), currentColumn());
    });
    setViewportMargins(gutterWidth(), 0, 0, 0);
    updateMarks();
}

int CodeEditor::gutterWidth() const
{
    const int digits = std::max(3, int(QString::number(std::max(1, blockCount())).size()));
    // The numbers, and room for a problem's mark on their left.
    return 22 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

int CodeEditor::currentLine() const
{
    return textCursor().blockNumber() + 1;
}

int CodeEditor::currentColumn() const
{
    return textCursor().positionInBlock() + 1;
}

void CodeEditor::resizeEvent(QResizeEvent *e)
{
    QPlainTextEdit::resizeEvent(e);
    const QRect r = contentsRect();
    m_gutter->setGeometry(r.left(), r.top(), gutterWidth(), r.height());
}

void CodeEditor::changeEvent(QEvent *e)
{
    QPlainTextEdit::changeEvent(e);
    if (e->type() == QEvent::PaletteChange || e->type() == QEvent::StyleChange) {
        m_highlighter->updateColours();
        updateMarks();
    }
}

void CodeEditor::paintGutter(QPaintEvent *e)
{
    QPainter p(m_gutter);
    const Palette &pal = Theme::palette();
    p.fillRect(e->rect(), pal.panel);
    QHash<int, bool> marks; // line → an error (else a warning)
    for (const CodeProblem &problem : std::as_const(m_problems)) {
        if (problem.line > 0)
            marks[problem.line] = marks.value(problem.line) || problem.error;
    }
    const int current = currentLine();
    QTextBlock block = firstVisibleBlock();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    p.setRenderHint(QPainter::Antialiasing);
    while (block.isValid() && top <= e->rect().bottom()) {
        const int height = qRound(blockBoundingRect(block).height());
        if (block.isVisible()) {
            const int line = block.blockNumber() + 1;
            const auto mark = marks.constFind(line);
            QFont f = font();
            f.setBold(line == current);
            p.setFont(f);
            p.setPen(mark != marks.cend() ? (*mark ? pal.danger : pal.warning) : line == current ? pal.text : pal.textMuted);
            p.drawText(0, top, m_gutter->width() - 6, height, Qt::AlignRight | Qt::AlignVCenter, QString::number(line));
            if (mark != marks.cend()) {
                // A dot for the problem: red for an error, amber for a warning.
                const double r = std::min(4.0, height / 4.0);
                p.setPen(Qt::NoPen);
                p.setBrush(*mark ? pal.danger : pal.warning);
                p.drawEllipse(QPointF(7, top + height / 2.0), r, r);
            }
        }
        block = block.next();
        top += height;
    }
}

void CodeEditor::setProblems(const QList<CodeProblem> &problems)
{
    m_problems = problems;
    updateMarks();
    m_gutter->update();
}

const CodeProblem *CodeEditor::problemAt(int line, int column) const
{
    for (const CodeProblem &p : m_problems) {
        const int endLine = p.endLine > 0 ? p.endLine : p.line;
        const int endColumn = p.endColumn > p.column || endLine > p.line ? p.endColumn : p.column + 1;
        const bool after = line > p.line || (line == p.line && column >= p.column);
        const bool before = line < endLine || (line == endLine && column < std::max(endColumn, p.column + 1));
        if (after && before)
            return &p;
    }
    return nullptr;
}

bool CodeEditor::nextProblem(bool backwards)
{
    QList<CodeProblem> sorted;
    for (const CodeProblem &p : m_problems) {
        if (p.line > 0)
            sorted << p;
    }
    if (sorted.isEmpty())
        return false;
    std::sort(sorted.begin(), sorted.end(), [](const CodeProblem &a, const CodeProblem &b) {
        return a.line != b.line ? a.line < b.line : a.column < b.column;
    });
    const int line = currentLine();
    const int column = currentColumn();
    auto isAfter = [&](const CodeProblem &p) { return p.line > line || (p.line == line && p.column > column); };
    auto isBefore = [&](const CodeProblem &p) { return p.line < line || (p.line == line && p.column < column); };
    const CodeProblem *to = nullptr;
    if (!backwards) {
        for (const CodeProblem &p : sorted) {
            if (isAfter(p)) {
                to = &p;
                break;
            }
        }
        if (!to)
            to = &sorted.first();
    } else {
        for (auto it = sorted.crbegin(); it != sorted.crend(); ++it) {
            if (isBefore(*it)) {
                to = &*it;
                break;
            }
        }
        if (!to)
            to = &sorted.last();
    }
    goToLine(to->line, to->column);
    return true;
}

void CodeEditor::goToLine(int line, int column)
{
    const QTextBlock block = document()->findBlockByNumber(std::clamp(line, 1, blockCount()) - 1);
    QTextCursor c(block);
    c.setPosition(block.position() + std::clamp(column - 1, 0, std::max(0, block.length() - 1)));
    setTextCursor(c);
    centerCursor();
    setFocus();
}

void CodeEditor::updateMarks()
{
    const Palette &pal = Theme::palette();
    QList<QTextEdit::ExtraSelection> marks;
    // The current line.
    {
        QTextEdit::ExtraSelection line;
        line.format.setBackground(Theme::mix(pal.surface, pal.text, pal.dark ? 0.05 : 0.04));
        line.format.setProperty(QTextFormat::FullWidthSelection, true);
        line.cursor = textCursor();
        line.cursor.clearSelection();
        marks << line;
    }
    // The bracket at the cursor and its partner.
    {
        const QString text = document()->toPlainText();
        const int pos = textCursor().position();
        static const QString open = QStringLiteral("([{");
        static const QString close = QStringLiteral(")]}");
        int at = -1;
        if (pos < text.size() && (open.contains(text.at(pos)) || close.contains(text.at(pos))))
            at = pos;
        else if (pos > 0 && (open.contains(text.at(pos - 1)) || close.contains(text.at(pos - 1))))
            at = pos - 1;
        if (at >= 0) {
            const QChar c = text.at(at);
            const bool forward = open.contains(c);
            const QChar partner = forward ? close.at(open.indexOf(c)) : open.at(close.indexOf(c));
            int depth = 0;
            int match = -1;
            for (int i = at; forward ? i < text.size() : i >= 0; i += forward ? 1 : -1) {
                if (text.at(i) == c)
                    ++depth;
                else if (text.at(i) == partner && --depth == 0) {
                    match = i;
                    break;
                }
            }
            for (int p : {at, match}) {
                if (p < 0)
                    continue;
                QTextEdit::ExtraSelection b;
                b.cursor = QTextCursor(document());
                b.cursor.setPosition(p);
                b.cursor.setPosition(p + 1, QTextCursor::KeepAnchor);
                b.format.setBackground(match >= 0 ? Theme::mix(pal.surface, pal.accent, 0.35) : Theme::mix(pal.surface, pal.danger, 0.4));
                marks << b;
            }
        }
    }
    // Problems, underlined where they are.
    for (const CodeProblem &p : std::as_const(m_problems)) {
        if (p.line <= 0)
            continue;
        const QTextBlock block = document()->findBlockByNumber(p.line - 1);
        if (!block.isValid())
            continue;
        QTextEdit::ExtraSelection mark;
        mark.cursor = QTextCursor(block);
        const int last = std::max(0, block.length() - 1);
        int start = std::clamp(p.column - 1, 0, last);
        int end = p.endLine == p.line && p.endColumn > p.column ? std::min(p.endColumn - 1, last) : last;
        if (end <= start) {
            // A point: the word there, or the line's end.
            start = std::min(start, std::max(0, last - 1));
            end = std::max(start + 1, std::min(last, start + 1));
        }
        mark.cursor.setPosition(block.position() + start);
        mark.cursor.setPosition(block.position() + end, QTextCursor::KeepAnchor);
        mark.format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
        mark.format.setUnderlineColor(p.error ? pal.danger : pal.warning);
        marks << mark;
    }
    setExtraSelections(marks);
}

QString CodeEditor::wordAt(const QPoint &pos) const
{
    QTextCursor c = cursorForPosition(pos);
    c.select(QTextCursor::WordUnderCursor);
    return c.selectedText();
}

bool CodeEditor::event(QEvent *e)
{
    if (e->type() == QEvent::ToolTip) {
        auto *help = static_cast<QHelpEvent *>(e);
        const QPoint at = viewport()->mapFrom(this, help->pos());
        const QTextCursor c = cursorForPosition(at);
        // Only over the text itself, not the empty space after a line.
        const QRect r = cursorRect(c);
        if (std::abs(r.center().x() - at.x()) > fontMetrics().averageCharWidth() * 2) {
            QToolTip::hideText();
            return true;
        }
        if (const CodeProblem *p = problemAt(c.blockNumber() + 1, c.positionInBlock() + 1)) {
            QToolTip::showText(help->globalPos(), (p->error ? Text::tr("Error: %1") : Text::tr("Warning: %1")).arg(p->text), this);
            return true;
        }
        const CodeDoc doc = codeDoc(wordAt(at));
        if (!doc.signature.isEmpty()) {
            QToolTip::showText(help->globalPos(),
                               QStringLiteral("<b><code>%1</code></b><br>%2").arg(doc.signature.toHtmlEscaped(), doc.text.toHtmlEscaped()), this);
            return true;
        }
        QToolTip::hideText();
        return true;
    }
    return QPlainTextEdit::event(e);
}

void CodeEditor::wheelEvent(QWheelEvent *e)
{
    if (e->modifiers() & Qt::ControlModifier) {
        zoom(e->angleDelta().y() > 0 ? 1 : -1);
        e->accept();
        return;
    }
    QPlainTextEdit::wheelEvent(e);
}

void CodeEditor::zoom(int steps)
{
    m_zoom = steps == 0 ? 0 : std::clamp(m_zoom + steps, -4, 12);
    QFont f = font();
    f.setPointSizeF(m_baseSize + m_zoom);
    setFont(f);
    setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);
    setViewportMargins(gutterWidth(), 0, 0, 0);
    const QRect r = contentsRect();
    m_gutter->setGeometry(r.left(), r.top(), gutterWidth(), r.height());
}

void CodeEditor::toggleComment()
{
    QTextCursor c = textCursor();
    const int startBlock = document()->findBlock(c.selectionStart()).blockNumber();
    int endBlock = document()->findBlock(c.selectionEnd()).blockNumber();
    // A selection ending at a line's start leaves that line alone.
    if (c.hasSelection() && endBlock > startBlock && document()->findBlock(c.selectionEnd()).position() == c.selectionEnd())
        --endBlock;
    bool allCommented = true;
    int indent = INT_MAX;
    for (int b = startBlock; b <= endBlock; ++b) {
        const QString t = document()->findBlockByNumber(b).text();
        if (t.trimmed().isEmpty())
            continue;
        allCommented = allCommented && t.trimmed().startsWith(QStringLiteral("//"));
        indent = std::min(indent, int(t.size() - QString(t).remove(QRegularExpression(QStringLiteral("^\\s+"))).size()));
    }
    if (indent == INT_MAX)
        return;
    c.beginEditBlock();
    for (int b = startBlock; b <= endBlock; ++b) {
        const QTextBlock block = document()->findBlockByNumber(b);
        const QString t = block.text();
        if (t.trimmed().isEmpty())
            continue;
        QTextCursor line(block);
        if (allCommented) {
            const int at = int(t.indexOf(QStringLiteral("//")));
            line.setPosition(block.position() + at);
            const int length = t.mid(at, 3) == QStringLiteral("// ") ? 3 : 2;
            line.setPosition(block.position() + at + length, QTextCursor::KeepAnchor);
            line.removeSelectedText();
        } else {
            line.setPosition(block.position() + indent);
            line.insertText(QStringLiteral("// "));
        }
    }
    c.endEditBlock();
}

void CodeEditor::indentSelection(bool out)
{
    QTextCursor c = textCursor();
    const int startBlock = document()->findBlock(c.selectionStart()).blockNumber();
    int endBlock = document()->findBlock(c.selectionEnd()).blockNumber();
    if (c.hasSelection() && endBlock > startBlock && document()->findBlock(c.selectionEnd()).position() == c.selectionEnd())
        --endBlock;
    c.beginEditBlock();
    for (int b = startBlock; b <= endBlock; ++b) {
        const QTextBlock block = document()->findBlockByNumber(b);
        QTextCursor line(block);
        if (out) {
            const QString t = block.text();
            int spaces = 0;
            while (spaces < 4 && spaces < t.size() && t.at(spaces) == u' ')
                ++spaces;
            if (spaces == 0 && t.startsWith(u'\t'))
                spaces = 1;
            line.setPosition(block.position() + spaces, QTextCursor::KeepAnchor);
            line.removeSelectedText();
        } else {
            line.insertText(QStringLiteral("    "));
        }
    }
    c.endEditBlock();
}

QString CodeEditor::wordBeforeCursor() const
{
    QTextCursor c = textCursor();
    const QString line = c.block().text().left(c.positionInBlock());
    int i = int(line.size());
    while (i > 0 && isWordChar(line.at(i - 1)))
        --i;
    return line.mid(i);
}

QStringList CodeEditor::completions(const QString &prefix) const
{
    // Slang's words and Lumen's first, then the source's own.
    QStringList pool;
    pool << SlangHighlighter::lumenFunctions() << SlangHighlighter::lumenAttributes() << SlangHighlighter::keywords()
         << SlangHighlighter::types() << SlangHighlighter::builtinFunctions();
    QSet<QString> seen(pool.cbegin(), pool.cend());
    const QString text = toPlainText();
    QRegularExpressionMatchIterator it = wordPattern().globalMatch(text);
    while (it.hasNext()) {
        const QString w = it.next().captured();
        if (w.size() >= 3 && !seen.contains(w)) {
            seen.insert(w);
            pool << w;
        }
    }
    QStringList out;
    for (const QString &w : std::as_const(pool)) {
        if (w.startsWith(prefix, Qt::CaseInsensitive) && w != prefix)
            out << w;
    }
    return out;
}

bool CodeEditor::isCompleting() const
{
    return m_completer->popup()->isVisible();
}

void CodeEditor::complete()
{
    const QString prefix = wordBeforeCursor();
    const QStringList words = completions(prefix);
    if (words.isEmpty()) {
        m_completer->popup()->hide();
        return;
    }
    m_words->setStringList(words);
    m_completer->setCompletionPrefix(prefix);
    m_completer->popup()->setCurrentIndex(m_completer->completionModel()->index(0, 0));
    QRect r = cursorRect();
    r.setWidth(m_completer->popup()->sizeHintForColumn(0) + m_completer->popup()->verticalScrollBar()->sizeHint().width() + 16);
    m_completer->complete(r);
}

void CodeEditor::insertCompletion(const QString &word)
{
    QTextCursor c = textCursor();
    const int typed = int(wordBeforeCursor().size());
    c.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor, typed);
    c.insertText(word);
    setTextCursor(c);
}

bool CodeEditor::handleIndent(QKeyEvent *e)
{
    QTextCursor c = textCursor();
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        // The line's indent, one more after an opening brace; a closing brace right after the
        // cursor goes to its own line, back one.
        const QString line = c.block().text();
        const QString before = line.left(c.positionInBlock());
        const QString after = line.mid(c.positionInBlock());
        QString indent;
        for (const QChar ch : line) {
            if (ch != u' ' && ch != u'\t')
                break;
            indent += ch;
        }
        const bool opens = before.trimmed().endsWith(u'{');
        c.beginEditBlock();
        c.insertText(QStringLiteral("\n") + indent + (opens ? QStringLiteral("    ") : QString()));
        if (opens && after.trimmed().startsWith(u'}')) {
            const int keep = c.position();
            c.insertText(QStringLiteral("\n") + indent);
            c.setPosition(keep);
        }
        c.endEditBlock();
        setTextCursor(c);
        return true;
    }
    if (e->key() == Qt::Key_Tab && !(e->modifiers() & Qt::ControlModifier)) {
        if (c.hasSelection() && c.selectedText().contains(QChar::ParagraphSeparator))
            indentSelection(false);
        else
            c.insertText(QString(4 - c.positionInBlock() % 4, u' '));
        return true;
    }
    if (e->key() == Qt::Key_Backtab) {
        indentSelection(true);
        return true;
    }
    if (e->text() == QStringLiteral("}")) {
        // Back to the brace's own indent on an empty line.
        const QString line = c.block().text();
        if (line.trimmed().isEmpty() && c.positionInBlock() == line.size() && line.size() >= 4 && line.endsWith(QStringLiteral("    "))) {
            c.beginEditBlock();
            c.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor, 4);
            c.insertText(QStringLiteral("}"));
            c.endEditBlock();
            setTextCursor(c);
            return true;
        }
    }
    return false;
}

bool CodeEditor::handlePairs(QKeyEvent *e)
{
    static const QHash<QString, QString> pairs{{QStringLiteral("("), QStringLiteral(")")},
                                               {QStringLiteral("["), QStringLiteral("]")},
                                               {QStringLiteral("{"), QStringLiteral("}")},
                                               {QStringLiteral("\""), QStringLiteral("\"")}};
    const QString typed = e->text();
    QTextCursor c = textCursor();
    const QString line = c.block().text();
    const int at = c.positionInBlock();
    const QChar next = at < line.size() ? line.at(at) : QChar();
    // Typing a closer that is already there steps over it.
    if ((typed == QStringLiteral(")") || typed == QStringLiteral("]") || typed == QStringLiteral("}") || typed == QStringLiteral("\""))
        && !c.hasSelection() && next == typed.at(0)) {
        c.movePosition(QTextCursor::Right);
        setTextCursor(c);
        return true;
    }
    if (!pairs.contains(typed))
        return false;
    // Only before a space, a closer or the line's end; quotes not inside a word.
    if (!next.isNull() && !next.isSpace() && !QStringLiteral(")]};,").contains(next))
        return false;
    if (typed == QStringLiteral("\"") && at > 0 && isWordChar(line.at(at - 1)))
        return false;
    c.beginEditBlock();
    const QString selected = c.selectedText();
    c.insertText(typed + selected + pairs.value(typed));
    c.movePosition(QTextCursor::Left, QTextCursor::MoveAnchor, 1);
    c.endEditBlock();
    setTextCursor(c);
    return true;
}

void CodeEditor::keyPressEvent(QKeyEvent *e)
{
    if (isCompleting()) {
        switch (e->key()) {
        case Qt::Key_Enter:
        case Qt::Key_Return:
        case Qt::Key_Tab:
            insertCompletion(m_completer->popup()->currentIndex().data().toString());
            m_completer->popup()->hide();
            return;
        case Qt::Key_Escape:
            m_completer->popup()->hide();
            return;
        default:
            break;
        }
    }
    const bool ctrl = e->modifiers() & Qt::ControlModifier;
    if (ctrl && e->key() == Qt::Key_Space) {
        complete();
        return;
    }
    if (ctrl && (e->key() == Qt::Key_Slash || e->key() == Qt::Key_Question)) {
        toggleComment();
        return;
    }
    if (!ctrl && (handleIndent(e) || handlePairs(e)))
        return;
    QPlainTextEdit::keyPressEvent(e);
    // Completion while a word is typed (three letters), or follows it once open.
    const QString word = wordBeforeCursor();
    const bool typing = !e->text().isEmpty() && isWordChar(e->text().back());
    if ((typing && word.size() >= 3) || (isCompleting() && !word.isEmpty()))
        complete();
    else if (isCompleting())
        m_completer->popup()->hide();
}

bool CodeEditor::find(const QString &text, bool backwards, bool caseSensitive, bool wholeWords)
{
    if (text.isEmpty())
        return false;
    QTextDocument::FindFlags flags;
    if (backwards)
        flags |= QTextDocument::FindBackward;
    if (caseSensitive)
        flags |= QTextDocument::FindCaseSensitively;
    if (wholeWords)
        flags |= QTextDocument::FindWholeWords;
    QTextCursor found = document()->find(text, textCursor(), flags);
    if (found.isNull()) {
        // Round from the other end.
        QTextCursor from(document());
        if (backwards)
            from.movePosition(QTextCursor::End);
        found = document()->find(text, from, flags);
    }
    if (found.isNull())
        return false;
    setTextCursor(found);
    centerCursor();
    return true;
}

bool CodeEditor::replaceNext(const QString &text, const QString &with, bool caseSensitive, bool wholeWords)
{
    QTextCursor c = textCursor();
    if (c.hasSelection() && QString::compare(c.selectedText(), text, caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive) == 0) {
        c.insertText(with);
        setTextCursor(c);
    }
    return find(text, false, caseSensitive, wholeWords);
}

int CodeEditor::replaceAll(const QString &text, const QString &with, bool caseSensitive, bool wholeWords)
{
    if (text.isEmpty())
        return 0;
    QTextDocument::FindFlags flags;
    if (caseSensitive)
        flags |= QTextDocument::FindCaseSensitively;
    if (wholeWords)
        flags |= QTextDocument::FindWholeWords;
    int count = 0;
    QTextCursor edit(document());
    edit.beginEditBlock();
    QTextCursor at(document());
    while (true) {
        at = document()->find(text, at, flags);
        if (at.isNull())
            break;
        at.insertText(with);
        ++count;
    }
    edit.endEditBlock();
    return count;
}

} // namespace lumen::ui
