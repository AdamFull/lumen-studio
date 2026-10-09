#include "ui/Accessibility.h"

#include <QAbstractButton>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QAccessible>
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QFormLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QTextEdit>
#include <QToolButton>

namespace lumen::ui {

namespace {

bool isInteractive(const QWidget *w)
{
    return qobject_cast<const QAbstractButton *>(w) || qobject_cast<const QLineEdit *>(w) || qobject_cast<const QComboBox *>(w)
           || qobject_cast<const QAbstractSpinBox *>(w) || qobject_cast<const QAbstractSlider *>(w)
           || qobject_cast<const QPlainTextEdit *>(w) || qobject_cast<const QTextEdit *>(w) || qobject_cast<const QKeySequenceEdit *>(w);
}

// Parts of composite widgets (the line edit inside a combo/spin box, a line edit's clear button,
// scroll bars) are named by their owner.
bool isInternal(const QWidget *w)
{
    for (const QWidget *p = w->parentWidget(); p; p = p->parentWidget()) {
        if (qobject_cast<const QComboBox *>(p) || qobject_cast<const QAbstractSpinBox *>(p) || qobject_cast<const QLineEdit *>(p)
            || qobject_cast<const QKeySequenceEdit *>(p))
            return true;
    }
    return w->objectName().startsWith(QLatin1String("qt_"));
}

QString plain(QString text)
{
    static const QRegularExpression tags(QStringLiteral("<[^>]*>"));
    static const QRegularExpression shortcut(QStringLiteral("\\s*\\([^()]*\\)\\s*$"));
    text.remove(tags);
    text.remove(u'&');
    text.remove(shortcut);
    return text.simplified();
}

QString labelFor(QWidget *w)
{
    // A QFormLayout row label.
    if (QWidget *parent = w->parentWidget()) {
        if (auto *form = qobject_cast<QFormLayout *>(parent->layout())) {
            if (auto *label = qobject_cast<QLabel *>(form->labelForField(w)))
                return plain(label->text());
        }
        // A QLabel buddy.
        for (QLabel *label : parent->findChildren<QLabel *>(Qt::FindDirectChildrenOnly)) {
            if (label->buddy() == w)
                return plain(label->text());
        }
    }
    return {};
}

} // namespace

void Accessibility::install()
{
    static Accessibility *filter = nullptr;
    if (filter || !qApp)
        return;
    filter = new Accessibility(qApp);
    qApp->installEventFilter(filter);
}

bool Accessibility::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Polish && watched->isWidgetType())
        ensureName(static_cast<QWidget *>(watched));
    return QObject::eventFilter(watched, event);
}

QString Accessibility::ensureName(QWidget *w)
{
    if (!w->accessibleName().isEmpty())
        return w->accessibleName();
    if (!isInteractive(w) || isInternal(w))
        return {};
    QString name;
    if (auto *b = qobject_cast<QAbstractButton *>(w)) {
        name = plain(b->text());
        if (auto *tb = qobject_cast<QToolButton *>(w); name.isEmpty() && tb && tb->defaultAction())
            name = plain(tb->defaultAction()->text());
        if (!name.isEmpty())
            return name; // Qt announces the text itself
    }
    if (name.isEmpty())
        name = labelFor(w);
    if (name.isEmpty()) {
        if (auto *e = qobject_cast<QLineEdit *>(w))
            name = plain(e->placeholderText());
        else if (auto *p = qobject_cast<QPlainTextEdit *>(w))
            name = plain(p->placeholderText());
        else if (auto *t = qobject_cast<QTextEdit *>(w))
            name = plain(t->placeholderText());
    }
    if (name.isEmpty())
        name = plain(w->toolTip());
    if (!name.isEmpty())
        w->setAccessibleName(name);
    return name;
}

QList<QWidget *> Accessibility::unnamed(QWidget *root)
{
    QList<QWidget *> out;
    QList<QWidget *> widgets = root->findChildren<QWidget *>();
    widgets.prepend(root);
    for (QWidget *w : widgets) {
        if (!isInteractive(w) || isInternal(w))
            continue;
        QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(w);
        if (!iface || iface->text(QAccessible::Name).trimmed().isEmpty())
            out << w;
    }
    return out;
}

} // namespace lumen::ui
