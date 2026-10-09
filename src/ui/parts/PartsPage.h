#pragma once

#include "doc/Parts.h"
#include "ui/parts/PartListPanel.h"

#include <QHash>
#include <QPointer>
#include <QTimer>
#include <QUuid>
#include <QWidget>

class QActionGroup;
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QToolButton;
class QVBoxLayout;

namespace lumen {
class Job;
}

namespace lumen::doc {
class Document;
}

namespace lumen::ui {

class AutoTextEdit;
class CanvasView;
class EditorState;
class PartEditContext;
class PartCard;
class ProviderCombo;
class PuppetPreview;
class SliderField;
class Studio;
class Tool;

// Character part slicing workspace (Live2D): 1 Source → 2 Masks → 3 Generate → 4 Preview & export.
// Masks are edited before any generation; parts are generated all at once, selected, or one by one.
class PartsPage : public QWidget
{
    Q_OBJECT
public:
    explicit PartsPage(Studio *studio, QWidget *parent = nullptr);
    ~PartsPage() override;

    // Starts a new session from an image asset.
    void openAsset(const QString &assetId);
    // Scripted runs (smoke tests, screenshots): once the next source is loaded, optionally segment it
    // with the local skeleton segmenter, cut out every part and go to `step`.
    void automate(bool segment, int step);

protected:
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void showEvent(QShowEvent *e) override;

private:
    enum Step { SourceStep = 0, MasksStep, GenerateStep, ExportStep };

    // Sessions.
    void loadSessions(const QString &select = QString());
    void openSession(const QString &recordId);
    void newSession();
    void attach(doc::PartSet *set);
    void setStep(int step);
    void updateStepAvailability();

    // Source.
    void chooseSourceFile();
    void useEditorImage();
    void setSourceAsset(const QString &assetId, bool keepSession = false);
    void removeBackground();
    void refreshSourceInfo();

    // Masks.
    void autoSegment();
    void splitPart(int label, PartListPanel::Split mode);
    void selectTool(const QString &id);

    // Generation.
    void generate(const QList<int> &labels);
    void cancelGeneration();
    void rebuildCards();
    void updateCard(int label);
    void saveGenerationSettings();

    // Preview / export.
    void refreshPreview();
    void exportPsd();
    void exportPngs();
    void openInEditor();

    // Canvas display.
    void rebuildDisplay();
    void updateOverlay(const QRect &rect);
    void scheduleExtensionOverlay();
    void updateExtensionOverlay();
    void paintExtensionPreview(const QImage &mask, const QRect &rect);
    void scheduleAssembled();
    void updateAssembled();
    void applyOverlayMode();
    QVector<QRgb> overlayPalette() const;

    QWidget *buildSourcePanel();
    QWidget *buildMasksPanel();
    QWidget *buildGeneratePanel();
    QWidget *buildExportPanel();

    Studio *m_studio;
    EditorState *m_state;
    PartEditContext *m_context;
    QPointer<doc::PartSet> m_set;
    doc::Document *m_display = nullptr;
    QUuid m_sourceLayer, m_resultLayer, m_overlayLayer, m_extLayer;

    // Header.
    QComboBox *m_sessions;
    QButtonGroup *m_steps;
    QLabel *m_stepHint;
    // Center.
    QStackedWidget *m_center;
    CanvasView *m_canvas;
    PuppetPreview *m_preview;
    QWidget *m_palette;
    QHBoxLayout *m_optionsHost;
    QWidget *m_options = nullptr;
    QLabel *m_hintLabel;
    QLabel *m_zoomLabel;
    QWidget *m_zoomControls;
    QComboBox *m_overlayMode;
    SliderField *m_overlayOpacity;
    QList<Tool *> m_tools;
    QActionGroup *m_toolGroup;
    QHash<QString, QAction *> m_toolActions;
    // Side panels.
    PartListPanel *m_list;
    QStackedWidget *m_panels;
    // Source panel.
    QLabel *m_sourceThumb;
    QLabel *m_sourceInfo;
    QComboBox *m_taxonomy;
    QPushButton *m_removeBg;
    // Masks panel.
    QComboBox *m_segmenter;
    QComboBox *m_scope;
    QPushButton *m_segment;
    QLabel *m_segmentHint;
    // Generate panel.
    QComboBox *m_strategy;
    ProviderCombo *m_provider;
    QSpinBox *m_variants;
    AutoTextEdit *m_character;
    QPushButton *m_generateAll;
    QPushButton *m_generateSelected;
    QPushButton *m_cancel;
    QVBoxLayout *m_cards;
    QHash<int, QPointer<PartCard>> m_cardWidgets;
    // Export panel.
    QCheckBox *m_idle;
    QCheckBox *m_follow;
    QCheckBox *m_talk;
    QList<SliderField *> m_paramSliders;
    QCheckBox *m_includeCutOuts;
    QCheckBox *m_includeOriginal;

    QList<QPointer<Job>> m_running;
    QTimer m_extensionTimer;
    QTimer m_assembledTimer;
    quint64 m_extensionGeneration = 0;
    quint64 m_assembledGeneration = 0;
    int m_solo = 0;
    bool m_showUnassigned = false;
    bool m_loading = false;
    struct Automation
    {
        bool active = false;
        bool segment = false;
        int step = 1;
    } m_automation;
    void runAutomation();
};

} // namespace lumen::ui
