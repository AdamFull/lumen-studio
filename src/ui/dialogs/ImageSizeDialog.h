#pragma once

#include <QDialog>
#include <QRect>

class QCheckBox;
class QComboBox;
class QSpinBox;

namespace lumen::ui {

// Image → Image size… (the picture scaled to a new size, its proportions kept unless unticked) and
// Image → Canvas size… (the canvas cut or extended around the picture, which stays where the user
// puts it: in the middle, at a side or in a corner).
class ImageSizeDialog : public QDialog
{
    Q_OBJECT
public:
    enum class Kind { Image, Canvas };
    ImageSizeDialog(Kind kind, const QSize &current, QWidget *parent = nullptr);

    QSize newSize() const;
    void setNewSize(const QSize &size);
    // Canvas: where the picture goes, as a column and a row from -1 (left, top) to 1 (right,
    // bottom); 0, 0 is the middle.
    void setAnchor(int column, int row);
    // Canvas: the new canvas in the picture's pixels, for Document::resizeCanvas.
    QRect canvasRect() const;

private:
    void widthChanged();
    void heightChanged();

    Kind m_kind;
    QSize m_current;
    QSpinBox *m_width;
    QSpinBox *m_height;
    QCheckBox *m_keep = nullptr;
    QComboBox *m_anchor = nullptr;
};

} // namespace lumen::ui
