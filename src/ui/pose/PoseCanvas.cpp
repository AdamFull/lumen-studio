#include "ui/pose/PoseCanvas.h"

#include "ui/Theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <cmath>

namespace lumen::ui {

PoseCanvas::PoseCanvas(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumSize(320, 320);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void PoseCanvas::setPose(const pose::Pose &pose)
{
    m_pose = pose;
    if (m_person >= m_pose.people.size())
        m_person = std::max(0, int(m_pose.people.size()) - 1);
    m_hover = Hit();
    update();
}

void PoseCanvas::setSelectedPerson(int index)
{
    m_person = index;
    update();
    emit personSelected(index);
}

void PoseCanvas::setUnderlay(const QImage &image)
{
    m_underlay = image;
    update();
}

void PoseCanvas::setUnderlayOpacity(double opacity)
{
    m_underlayOpacity = opacity;
    update();
}

void PoseCanvas::setShowFace(bool on)
{
    m_showFace = on;
    update();
}

void PoseCanvas::setShowHands(bool on)
{
    m_showHands = on;
    update();
}

QRectF PoseCanvas::canvasRect() const
{
    const QSizeF avail = QSizeF(size()) - QSizeF(32, 32);
    const QSizeF s = QSizeF(m_pose.canvas).scaled(avail, Qt::KeepAspectRatio);
    return QRectF(QPointF((width() - s.width()) / 2.0, (height() - s.height()) / 2.0), s);
}

QPointF PoseCanvas::toCanvas(const QPointF &w) const
{
    const QRectF r = canvasRect();
    return QPointF((w.x() - r.left()) / r.width() * m_pose.canvas.width(), (w.y() - r.top()) / r.height() * m_pose.canvas.height());
}

QPointF PoseCanvas::toWidget(const QPointF &c) const
{
    const QRectF r = canvasRect();
    return QPointF(r.left() + c.x() / m_pose.canvas.width() * r.width(), r.top() + c.y() / m_pose.canvas.height() * r.height());
}

PoseCanvas::Hit PoseCanvas::hitTest(const QPointF &wpos) const
{
    Hit best;
    double bestDist = 12.0;
    for (int pi = 0; pi < m_pose.people.size(); ++pi) {
        const pose::Person &person = m_pose.people[pi];
        for (const pose::JointRef &ref : person.allJoints(true)) {
            if (ref.part == pose::Part::Face && !m_showFace)
                continue;
            if ((ref.part == pose::Part::LeftHand || ref.part == pose::Part::RightHand) && !m_showHands)
                continue;
            const double d = QLineF(toWidget(person.point(ref)->pos), wpos).length();
            // Body joints win ties over dense hand/face points.
            const double bias = ref.part == pose::Part::Body ? -2.0 : (ref.part == pose::Part::Face ? 2.0 : 0.0);
            if (d + bias < bestDist && d < 12.0) {
                bestDist = d + bias;
                best.person = pi;
                best.joint = ref;
            }
        }
    }
    return best;
}

void PoseCanvas::paintEvent(QPaintEvent *)
{
    const Palette &pal = Theme::palette();
    QPainter p(this);
    p.fillRect(rect(), pal.canvas);
    const QRectF r = canvasRect();
    p.fillRect(r, QColor(0, 0, 0));
    if (!m_underlay.isNull()) {
        p.setOpacity(m_underlayOpacity);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(r, m_underlay);
        p.setOpacity(1.0);
    }
    p.setPen(QPen(pal.borderStrong, 1));
    p.drawRect(r.adjusted(-0.5, -0.5, 0.5, 0.5));
    p.setRenderHint(QPainter::Antialiasing);
    const double scale = r.width() / std::max(1, m_pose.canvas.width());
    const double limbWidth = std::clamp(6.0 * scale * m_pose.canvas.width() / 512.0, 3.0, 9.0);

    for (int pi = 0; pi < m_pose.people.size(); ++pi) {
        const pose::Person &person = m_pose.people[pi];
        const bool selected = pi == m_person;
        p.setOpacity(selected ? 1.0 : 0.55);
        const auto &limbs = pose::bodyLimbs();
        for (int i = 0; i < limbs.size(); ++i) {
            const pose::Keypoint &a = person.body[static_cast<size_t>(limbs[i].first)];
            const pose::Keypoint &b = person.body[static_cast<size_t>(limbs[i].second)];
            if (!a.visible || !b.visible)
                continue;
            QColor c = pose::bodyColor(i);
            c.setAlphaF(0.85f);
            p.setPen(QPen(c, limbWidth, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(toWidget(a.pos), toWidget(b.pos));
        }
        if (m_showHands) {
            for (const QList<pose::Keypoint> *hand : {&person.leftHand, &person.rightHand}) {
                if (hand->size() != pose::kHandCount)
                    continue;
                const auto &edges = pose::handEdges();
                for (int e = 0; e < edges.size(); ++e) {
                    const pose::Keypoint &a = hand->at(edges[e].first);
                    const pose::Keypoint &b = hand->at(edges[e].second);
                    if (a.visible && b.visible) {
                        p.setPen(QPen(pose::handEdgeColor(e), 1.6, Qt::SolidLine, Qt::RoundCap));
                        p.drawLine(toWidget(a.pos), toWidget(b.pos));
                    }
                }
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(80, 120, 255));
                for (const pose::Keypoint &k : *hand) {
                    if (k.visible)
                        p.drawEllipse(toWidget(k.pos), 2.4, 2.4);
                }
            }
        }
        if (m_showFace && person.face.size() == pose::kFaceCount) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 220));
            for (const pose::Keypoint &k : person.face) {
                if (k.visible)
                    p.drawEllipse(toWidget(k.pos), 1.6, 1.6);
            }
        }
        for (int i = 0; i < pose::kBodyCount; ++i) {
            const pose::Keypoint &k = person.body[static_cast<size_t>(i)];
            if (!k.visible)
                continue;
            const bool hovered = m_hover.person == pi && m_hover.joint == pose::JointRef{pose::Part::Body, i};
            const bool dragged = m_drag.person == pi && m_drag.joint == pose::JointRef{pose::Part::Body, i};
            const double radius = dragged || hovered ? 7.5 : 5.5;
            p.setPen(QPen(Qt::white, hovered || dragged ? 2.5 : 1.5));
            p.setBrush(pose::bodyColor(i));
            p.drawEllipse(toWidget(k.pos), radius, radius);
        }
    }
    p.setOpacity(1.0);
    // Hovered non-body point and its name.
    if (m_hover.person >= 0) {
        const pose::Person &person = m_pose.people[m_hover.person];
        const QPointF w = toWidget(person.point(m_hover.joint)->pos);
        if (m_hover.joint.part != pose::Part::Body) {
            p.setPen(QPen(pal.accent, 2));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(w, 5, 5);
        }
        const QString name = pose::jointName(m_hover.joint);
        QFont f = font();
        f.setPointSizeF(f.pointSizeF() * 0.9);
        p.setFont(f);
        const QRectF label(w + QPointF(12, -24), QSizeF(QFontMetrics(f).horizontalAdvance(name) + 14, 22));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 190));
        p.drawRoundedRect(label, 6, 6);
        p.setPen(Qt::white);
        p.drawText(label, Qt::AlignCenter, name);
    }
}

void PoseCanvas::mousePressEvent(QMouseEvent *e)
{
    const Hit hit = hitTest(e->position());
    m_before = m_pose;
    m_lastCanvasPos = toCanvas(e->position());
    if (hit.person >= 0) {
        m_drag = hit;
        m_dragPerson = false;
        if (hit.person != m_person)
            setSelectedPerson(hit.person);
    } else {
        // Drag on empty space of the selected person's bounding box moves the whole person.
        m_drag = Hit();
        m_dragPerson = false;
        for (int pi = 0; pi < m_pose.people.size(); ++pi) {
            const QRectF b = m_pose.people[pi].bounds().adjusted(-20, -20, 20, 20);
            if (b.contains(m_lastCanvasPos)) {
                m_dragPerson = true;
                if (pi != m_person)
                    setSelectedPerson(pi);
                setCursor(Qt::ClosedHandCursor);
                break;
            }
        }
    }
}

void PoseCanvas::mouseMoveEvent(QMouseEvent *e)
{
    const QPointF c = toCanvas(e->position());
    if (m_drag.person >= 0 && (e->buttons() & Qt::LeftButton)) {
        pose::DragMode mode = m_mode;
        if (e->modifiers() & Qt::ShiftModifier)
            mode = pose::DragMode::Translate;
        else if (e->modifiers() & Qt::AltModifier)
            mode = pose::DragMode::Free;
        else if (e->modifiers() & Qt::ControlModifier)
            mode = pose::DragMode::Chain;
        pose::dragJoint(m_pose.people[m_drag.person], m_drag.joint, c, mode);
        update();
        emit poseEdited(m_pose);
        return;
    }
    if (m_dragPerson && (e->buttons() & Qt::LeftButton) && m_person < m_pose.people.size()) {
        m_pose.people[m_person].translate(c - m_lastCanvasPos);
        m_lastCanvasPos = c;
        update();
        emit poseEdited(m_pose);
        return;
    }
    const Hit hover = hitTest(e->position());
    if (hover.person != m_hover.person || hover.joint != m_hover.joint) {
        m_hover = hover;
        setCursor(hover.person >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void PoseCanvas::mouseReleaseEvent(QMouseEvent *)
{
    if ((m_drag.person >= 0 || m_dragPerson) && !(m_before == m_pose))
        emit editFinished(m_before, m_pose);
    m_drag = Hit();
    m_dragPerson = false;
    setCursor(Qt::ArrowCursor);
    update();
}

void PoseCanvas::leaveEvent(QEvent *)
{
    m_hover = Hit();
    update();
}

} // namespace lumen::ui
