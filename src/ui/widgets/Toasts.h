#pragma once

#include <QWidget>

class QVBoxLayout;

namespace lumen::ui {

// Transient notifications stacked in the bottom-right corner of the parent widget.
class Toasts : public QWidget
{
    Q_OBJECT
public:
    explicit Toasts(QWidget *parent);
    void show(const QString &message, const QString &level = QStringLiteral("info"));

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void reposition();
    QVBoxLayout *m_layout;
};

} // namespace lumen::ui
