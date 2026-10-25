#pragma once

#include "ai/CharacterContext.h"
#include "doc/Parts.h"
#include "ui/parts/PartListPanel.h"

#include <QHash>
#include <QPointer>
#include <QTimer>
#include <QUuid>
#include <QWidget>

#include <functional>

class QActionGroup;
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTabWidget;
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
class CharacterChip;
class CubismPanel;
class CubismView;
class ElidedLabel;
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
    // Opens step 4 with the Cubism model preview, loading `model3Path` when given.
    void previewCubismModel(const QString &model3Path);

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
    void openLayeredPsd(const QString &path = QString());
    void useEditorImage();
    void setSourceAsset(const QString &assetId, bool keepSession = false);
    void removeBackground();
    void refreshSourceInfo();

    // Masks.
    void autoSegment();
    void splitPart(int label, PartListPanel::Split mode);
    void detectSkeleton();
    void selectTool(const QString &id);

    // Generation.
    void generate(const QList<int> &labels);
    void generateExpression(const QString &kind);
    // Loads the page's character (if any) and continues with it and the checkpoint's family.
    void withCharacter(const QString &providerId, std::function<void(const ai::CharacterContext &, const QString &family)> then);
    // An image drawn for a library character makes that character the project's one.
    void adoptCharacterOf(const QString &assetId);
    void updateCharacterHint();
    void cancelGeneration();
    void rebuildCards();
    void updateCard(int label);
    void saveGenerationSettings();

    // Preview / export.
    void refreshPreview();
    void showPreviewWidget();
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
    ElidedLabel *m_stepHint;
    // Center.
    QStackedWidget *m_center;
    CanvasView *m_canvas;
    PuppetPreview *m_preview;
    CubismView *m_cubism;
    CubismPanel *m_cubismPanel = nullptr;
    QTabWidget *m_previewTabs = nullptr;
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
    CharacterChip *m_characterChip;
    QPushButton *m_generateAll;
    QPushButton *m_generateSelected;
    QPushButton *m_cancel;
    QComboBox *m_expression;
    QPushButton *m_expressionButton;
    QCheckBox *m_checkExpressions;
    QVBoxLayout *m_cards;
    QHash<int, QPointer<PartCard>> m_cardWidgets;
    QList<int> m_cardOrder;
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
    bool m_holdPreview = false; // an explicit Cubism preview request survives session restores
    struct Automation
    {
        bool active = false;
        bool segment = false;
        int step = 1;
    } m_automation;
    void runAutomation();
};

} // namespace lumen::ui
