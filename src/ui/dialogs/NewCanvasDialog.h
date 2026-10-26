#pragma once

#include <QColor>
#include <QDialog>

class QComboBox;
class QLineEdit;
class QSpinBox;

namespace lumen::ui {

class Studio;

// File → New blank canvas: a name, a size (common sizes or any width × height) and a background.
// The canvas is a size of the user's choosing, so the sizes are in pixels (New from prompt picks a
// shape at the engine's pixels instead).
class NewCanvasDialog : public QDialog
{
    Q_OBJECT
public:
    explicit NewCanvasDialog(Studio *studio, QWidget *parent = nullptr);

    QString name() const;
    QSize canvasSize() const;
    QColor background() const;

private:
    QLineEdit *m_name;
    QComboBox *m_preset;
    QSpinBox *m_width;
    QSpinBox *m_height;
    QComboBox *m_background;
};

} // namespace lumen::ui
