#include "ui/widgets/Toasts.h"

#include "ui/Icons.h"
#include "ui/Theme.h"

#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPropertyAnimation>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace lumen::ui {

Toasts::Toasts(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(8);
    parent->installEventFilter(this);
    setFixedWidth(380);
    reposition();
}

bool Toasts::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize)
        reposition();
    return QWidget::eventFilter(watched, event);
}

void Toasts::reposition()
{
    if (!parentWidget())
        return;
    // Only as tall as the visible toasts so the rest of the window stays clickable.
    const int h = std::min(m_layout->sizeHint().height(), parentWidget()->height() - 80);
    setGeometry(parentWidget()->width() - width() - 20, parentWidget()->height() - h - 40, width(), std::max(0, h));
    setVisible(m_layout->count() > 0);
    raise();
}

void Toasts::show(const QString &message, const QString &level)
{
    const Palette &pal = Theme::palette();
    QColor accent = pal.accent;
    QString icon = QStringLiteral("info");
    if (level == QLatin1String("error")) {
        accent = pal.danger;
        icon = QStringLiteral("warning");
    } else if (level == QLatin1String("warning")) {
        accent = pal.warning;
        icon = QStringLiteral("warning");
    } else if (level == QLatin1String("success")) {
        accent = pal.success;
        icon = QStringLiteral("check");
    }
    auto *toast = new QFrame(this);
    toast->setStyleSheet(QStringLiteral("QFrame#toast { background: %1; border: 1px solid %2; border-left: 3px solid %3; border-radius: 10px; }")
                             .arg(pal.raised.name(), pal.borderStrong.name(), accent.name()));
    toast->setObjectName(QStringLiteral("toast"));
    auto *h = new QHBoxLayout(toast);
    h->setContentsMargins(12, 10, 8, 10);
    h->setSpacing(10);
    auto *ic = new QLabel(toast);
    ic->setPixmap(Icons::pixmap(icon, 18, accent));
    auto *text = new QLabel(message, toast);
    text->setWordWrap(true);
    text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *close = new QToolButton(toast);
    close->setIcon(Icons::get(QStringLiteral("close")));
    close->setIconSize(QSize(12, 12));
    h->addWidget(ic, 0, Qt::AlignTop);
    h->addWidget(text, 1);
    h->addWidget(close, 0, Qt::AlignTop);
    m_layout->addWidget(toast);
    connect(toast, &QObject::destroyed, this, [this] { QTimer::singleShot(0, this, &Toasts::reposition); });
    while (m_layout->count() > 5) {
        QLayoutItem *item = m_layout->takeAt(0);
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
    QWidget::show();
    reposition();
    auto *effect = new QGraphicsOpacityEffect(toast);
    toast->setGraphicsEffect(effect);
    auto *fadeIn = new QPropertyAnimation(effect, "opacity", toast);
    fadeIn->setDuration(180);
    fadeIn->setStartValue(0.0);
    fadeIn->setEndValue(1.0);
    fadeIn->start(QAbstractAnimation::DeleteWhenStopped);
    auto dismiss = [toast, effect] {
        auto *fade = new QPropertyAnimation(effect, "opacity", toast);
        fade->setDuration(250);
        fade->setStartValue(effect->opacity());
        fade->setEndValue(0.0);
        QObject::connect(fade, &QPropertyAnimation::finished, toast, &QObject::deleteLater);
        fade->start(QAbstractAnimation::DeleteWhenStopped);
    };
    connect(close, &QToolButton::clicked, toast, dismiss);
    const int ms = level == QLatin1String("error") ? 12000 : (message.size() > 120 ? 8000 : 4500);
    QTimer::singleShot(ms, toast, dismiss);
}

} // namespace lumen::ui
