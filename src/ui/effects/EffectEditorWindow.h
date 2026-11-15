#pragma once

#include "filters/NodeType.h"
#include "gpu/Compiler.h"

#include <QImage>
#include <QMainWindow>
#include <QPointer>
#include <QTimer>

class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QVBoxLayout;

namespace lumen::ui {

class ParamField;
class Studio;
class UserEffects;

// The effect editor (plan M20 step 7): your own effects written in Slang, a window of its own. Your
// effects on the left; the source in the middle, compiled as you type, with the compiler's errors
// underlined where they are and listed below it; on the right the effect on the active layer (or a
// sample picture) with its sliders, which come from the source's [Label] and [Range]. Save puts it in
// the effects folder, where it is a node of the filter editor. Open… shows a file someone shared,
// with what it is said first, and adds nothing until it is saved; Export… writes a copy to share.
class EffectEditorWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit EffectEditorWindow(Studio *studio, QWidget *parent = nullptr);
    ~EffectEditorWindow() override;

    // The open window, raised, or a new one.
    static EffectEditorWindow *show(Studio *studio, QWidget *parent);

    QString source() const;
    void setSource(const QString &source); // as if typed: compiled and previewed shortly after
    QString effectName() const;
    void setEffectName(const QString &name);
    // Opens one of your effects, or a file from elsewhere (shown first, saved only on Save).
    bool openEffect(const QString &name);
    bool importFile(const QString &path, QString *error = nullptr);
    void newEffect();
    // Compiles now instead of after the pause in typing.
    void compileNow();
    bool save();

    bool isBusy() const { return m_compiling || m_previewing; }
    QList<gpu::CompileMessage> messages() const { return m_messages; }
    QString status() const;
    QImage preview() const { return m_previewImage; }
    ParamField *field(const QString &param) const;

    // The starting point of a new effect.
    static QString templateSource();

signals:
    // A compile ended (ok or not) and, when it compiled, the preview was rendered.
    void updated();

protected:
    void closeEvent(QCloseEvent *e) override;

private:
    void fillList();
    void compiled(const gpu::Compiled &result, int generation);
    void showMessages();
    void rebuildFields();
    void renderPreview();
    void showPicture();
    QImage previewSource() const;
    void updateBanner();
    bool confirmDiscard();

    Studio *m_studio;
    UserEffects *m_effects;
    QListWidget *m_list = nullptr;
    QLineEdit *m_name = nullptr;
    QPlainTextEdit *m_editor = nullptr;
    QListWidget *m_problems = nullptr;
    QLabel *m_banner = nullptr;
    QPushButton *m_download = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_previewLabel = nullptr;
    QWidget *m_fieldsHost = nullptr;
    QVBoxLayout *m_fields = nullptr;
    QList<ParamField *> m_paramFields;
    QPushButton *m_save = nullptr;
    QPushButton *m_delete = nullptr;

    QTimer m_compileTimer;
    int m_generation = 0;
    bool m_compiling = false;
    bool m_previewing = false;
    bool m_previewAgain = false;
    bool m_dirty = false;
    QString m_importedFrom; // the file shown for review; empty for your own
    QString m_savedSource;
    QList<gpu::CompileMessage> m_messages;
    QString m_error;
    filters::NodeTypePtr m_node;  // the compiled effect as it is now
    QVariantMap m_values;          // its sliders' values, kept while it is compiled again
    QImage m_previewImage;
    QString m_previewId;           // "preview:<n>": this window's compiled effect
};

} // namespace lumen::ui
