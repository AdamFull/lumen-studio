#include "pose/PoseRenderer.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace lumen::pose {

namespace {

QColor scaledColor(const QColor &c, double k)
{
    return QColor(int(c.red() * k), int(c.green() * k), int(c.blue() * k));
}

} // namespace

void paintOpenPose(QImage &image, const Pose &pose, const QRectF &target, const RenderOptions &options)
{
    if (pose.canvas.isEmpty())
        return;
    const double sx = target.width() / pose.canvas.width();
    const double sy = target.height() / pose.canvas.height();
    auto map = [&](const QPointF &p) { return QPointF(target.left() + p.x() * sx, target.top() + p.y() * sy); };

    const double shortSide = std::min(target.width(), target.height());
    const double k = options.thickness > 0.0 ? options.thickness : std::max(1.0, shortSide / 512.0);
    const double stickWidth = 4.0 * k;
    const double jointRadius = 4.0 * k;
    const double handLine = 2.0 * k;
    const double handRadius = 4.0 * k;
    const double faceRadius = 3.0 * k;

    QPainter p(&image);
    // OpenCV rasterisation is not anti-aliased; ControlNet models were trained on such maps.
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(Qt::NoPen);

    for (const Person &person : pose.people) {
        if (options.body) {
            const auto &limbs = bodyLimbs();
            for (int i = 0; i < limbs.size(); ++i) {
                const Keypoint &a = person.body[static_cast<size_t>(limbs[i].first)];
                const Keypoint &b = person.body[static_cast<size_t>(limbs[i].second)];
                if (!a.visible || !b.visible)
                    continue;
                const QPointF pa = map(a.pos);
                const QPointF pb = map(b.pos);
                const QPointF mid = (pa + pb) / 2.0;
                const double length = std::hypot(pb.x() - pa.x(), pb.y() - pa.y());
                const double angle = std::atan2(pb.y() - pa.y(), pb.x() - pa.x()) * 180.0 / M_PI;
                p.save();
                p.translate(mid);
                p.rotate(angle);
                p.setBrush(scaledColor(bodyColor(i), 0.6));
                p.drawEllipse(QPointF(0, 0), length / 2.0, stickWidth);
                p.restore();
            }
            for (int i = 0; i < kBodyCount; ++i) {
                const Keypoint &kp = person.body[static_cast<size_t>(i)];
                if (!kp.visible)
                    continue;
                p.setBrush(bodyColor(i));
                p.drawEllipse(map(kp.pos), jointRadius, jointRadius);
            }
        }
        if (options.hands) {
            for (const QList<Keypoint> *hand : {&person.leftHand, &person.rightHand}) {
                if (hand->size() != kHandCount)
                    continue;
                const auto &edges = handEdges();
                for (int e = 0; e < edges.size(); ++e) {
                    const Keypoint &a = hand->at(edges[e].first);
                    const Keypoint &b = hand->at(edges[e].second);
                    if (!a.visible || !b.visible)
                        continue;
                    QPen pen(handEdgeColor(e), handLine, Qt::SolidLine, Qt::RoundCap);
                    p.setPen(pen);
                    p.drawLine(map(a.pos), map(b.pos));
                }
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(0, 0, 255));
                for (const Keypoint &kp : *hand) {
                    if (kp.visible)
                        p.drawEllipse(map(kp.pos), handRadius, handRadius);
                }
            }
        }
        if (options.face && person.face.size() == kFaceCount) {
            p.setBrush(Qt::white);
            for (const Keypoint &kp : person.face) {
                if (kp.visible)
                    p.drawEllipse(map(kp.pos), faceRadius, faceRadius);
            }
        }
    }
    p.end();
}

QImage renderOpenPose(const Pose &pose, const QSize &outputSize, const RenderOptions &options)
{
    const QSize size = outputSize.isEmpty() ? pose.canvas : outputSize;
    QImage image(size, QImage::Format_RGB32);
    image.fill(options.background);
    paintOpenPose(image, pose, QRectF(QPointF(0, 0), QSizeF(size)), options);
    return image;
}

} // namespace lumen::pose
