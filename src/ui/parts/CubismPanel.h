#pragma once

#include <QPointer>
#include <QTimer>
#include <QWidget>

class QCheckBox;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace lumen::ui {

class CubismView;
class SliderField;
class Studio;

// Controls of the Cubism model preview: Core library location, model loading, animation toggles
// and one slider per model parameter.
class CubismPanel : public QWidget
{
    Q_OBJECT
public:
    CubismPanel(Studio *studio, CubismView *view, QWidget *parent = nullptr);

    // Loads the Core (when possible) and the last model the first time the panel is shown.
    void activate();
    bool openModel(const QString &path);

private:
    void locateCore();
    void chooseModel();
    void refreshStatus();
    void rebuildParameters();
    void syncSliders();

    Studio *m_studio;
    CubismView *m_view;
    QLabel *m_status;
    QPushButton *m_locate;
    QPushButton *m_open;
    QCheckBox *m_idle;
    QCheckBox *m_motion;
    QCheckBox *m_follow;
    QCheckBox *m_talk;
    QCheckBox *m_physics;
    SliderField *m_wind;
    QPushButton *m_reset;
    QWidget *m_paramHost;
    QVBoxLayout *m_params;
    QList<QPointer<SliderField>> m_sliders;
    QTimer m_sync;
    bool m_activated = false;
    bool m_syncing = false;
};

} // namespace lumen::ui
