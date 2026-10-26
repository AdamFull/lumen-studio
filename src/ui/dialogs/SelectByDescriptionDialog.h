#pragma once

#include "core/Finders.h"

#include <QDialog>
#include <QImage>
#include <QPointer>
#include <QTimer>

class QButtonGroup;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QWidget;

namespace lumen::doc {
class Document;
}

namespace lumen::ui {

class SliderField;
class SmartSelect;

// What a finder request will select, in plain words ("Lines darker than what is around them, up to
// 6 px wide"); for Kind::None, what can be found instead.
QString finderDescription(const img::FinderRequest &request);
// The words after "select" ("select: the black lines" → "the black lines"; also "auswählen:"), or
// an empty string when the words do not ask to select.
QString selectionWords(const QString &words);

// Select → By description… (plan M18 §18.1): the user says what to select ("the black lines",
// "dust", "the red"); Lumen says what it understood, shows the finder's settings and the found area
// on a preview, and Select makes it the selection (new, added, taken away or where both are) as one
// undo step. Other words are objects ("the cup"), found with the detection models or an image
// engine that can (setObjectFinder).
class SelectByDescriptionDialog : public QDialog
{
    Q_OBJECT
public:
    SelectByDescriptionDialog(doc::Document *doc, QWidget *parent = nullptr);

    void setWords(const QString &words);
    // Words that name no line, speck or colour are looked for as objects ("the cup") with the
    // detection models (Grounding DINO and SAM 2.1), downloaded once from here when missing.
    void setObjectFinder(SmartSelect *finder);
    // The words are looked for as objects.
    bool findsObjects() const { return m_objects; }
    const img::FinderRequest &request() const { return m_request; }
    // The found area (Grayscale8, the document's size); null before it is found or for Kind::None.
    QImage found() const { return m_found; }
    // Makes the found area the selection with the chosen mode; false when nothing was found.
    bool apply();

signals:
    // A finder run ended and the preview shows it (tests).
    void foundChanged();

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void parseWords();
    void showSettings();
    void readSettings();
    void find();
    void showPreview();

    QPointer<doc::Document> m_doc;
    QImage m_picture;
    img::FinderRequest m_request;
    QImage m_found;
    int m_run = 0;
    QTimer m_debounce;
    QLineEdit *m_words;
    QLabel *m_understood;
    QWidget *m_strokeBox;
    QComboBox *m_tone;
    QSpinBox *m_width;
    SliderField *m_contrast;
    QWidget *m_colorBox;
    SliderField *m_tolerance;
    QLabel *m_preview;
    QLabel *m_result;
    QButtonGroup *m_mode;
    QPushButton *m_select;
    QPushButton *m_download;
    QPointer<SmartSelect> m_finder;
    bool m_objects = false;
};

} // namespace lumen::ui
