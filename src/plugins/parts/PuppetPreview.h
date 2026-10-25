#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QTimer>
#include <QWidget>

namespace lumen::ui {

struct PuppetLayer
{
    QString name;
    QString tag;
    QString group;   // head, body, extra, custom
    QString side;
    QImage image;    // cropped RGBA
    QRect rect;      // canvas rect
    double depth = 0.5; // 0 back .. 1 front
    bool visible = true;
};

// Live2D-style 2.5D preview of a sliced character, without the Cubism SDK: per-part parallax for
// head and body angles, hair sway (strip deformation), breathing, blinking and mouth opening.
// Parameter names follow the Cubism standard parameters. Animates only while visible.
class PuppetPreview : public QWidget
{
    Q_OBJECT
public:
    struct Params
    {
        double angleX = 0;   // ParamAngleX  -30..30
        double angleY = 0;   // ParamAngleY  -30..30
        double angleZ = 0;   // ParamAngleZ  -30..30
        double bodyX = 0;    // ParamBodyAngleX -10..10
        double breath = 0;   // ParamBreath 0..1
        double eyeOpen = 1;  // ParamEyeLOpen / ParamEyeROpen 0..1
        double mouthOpen = 0; // ParamMouthOpenY 0..1
        double hair = 0;     // ParamHairFront / ParamHairBack -1..1
    };

    explicit PuppetPreview(QWidget *parent = nullptr);

    void setLayers(const QList<PuppetLayer> &layers, const QSize &canvas);
    bool isEmpty() const { return m_layers.isEmpty(); }
    const Params &params() const { return m_params; }
    void setParams(const Params &p);
    void setAutoIdle(bool on);
    void setFollowMouse(bool on);
    void setTalking(bool on);
    bool autoIdle() const { return m_idle; }
    QImage renderFrame(const QSize &size);

signals:
    void paramsChanged();

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *) override;

private:
    void tick();
    void updateTimer();
    void rescale();
    void render(QPainter &p, const QRectF &target);
    double viewScale(const QSizeF &area) const;

    QList<PuppetLayer> m_layers;
    QSize m_canvas;
    QList<QImage> m_scaled;   // layers pre-scaled to the current view scale
    double m_scaledFor = 0.0;
    quint64 m_generation = 0;
    QPointF m_headPivot, m_bodyPivot;
    double m_headSize = 100;
    Params m_params;
    bool m_idle = true;
    bool m_follow = false;
    bool m_talking = false;
    QPointF m_mouse{-1, -1};
    QTimer m_timer;
    QTimer m_rescaleTimer;
    QElapsedTimer m_clock;
    double m_nextBlink = 2.5;
    double m_blinkStart = -1;
    double m_lastT = 0;
};

} // namespace lumen::ui
