#pragma once

#include "ai/Onnx.h"

#include <QImage>
#include <QList>
#include <QMutex>
#include <QPointF>
#include <QRectF>
#include <QString>

#include <array>
#include <memory>
#include <vector>

namespace lumen::ai {

// Where the user points at an object (plan M18 §18.1, Smart selection): clicks on it (positive) or
// off it (negative), and/or a box around it, in picture pixels.
struct SegmentPrompt
{
    QList<QPointF> points;
    QList<bool> positive;       // one per point
    QRectF box;                 // null = none

    bool isEmpty() const { return points.isEmpty() && box.isNull(); }
};

// Cuts out the object a prompt points at. Blocking (a worker thread); one call at a time.
class Segmenter
{
public:
    virtual ~Segmenter() = default;
    // The object's mask (Grayscale8, 255 = the object) at the picture's size; null with `error` set
    // when it cannot be made.
    virtual QImage segment(const QImage &picture, const SegmentPrompt &prompt, QString *error = nullptr) = 0;
};

// SAM 2.1 through ONNX Runtime: the vision encoder turns the picture (squeezed to 1024 × 1024) into
// three feature maps once, kept for the next prompts on the same picture; the prompt decoder turns
// points and a box into three candidate masks at 256 × 256 with a quality score each, and the best
// one, scaled to the picture, is the object.
class Sam2Segmenter : public Segmenter
{
public:
    static std::shared_ptr<Sam2Segmenter> open(std::shared_ptr<OnnxRuntime> runtime, const QString &encoderPath,
                                               const QString &decoderPath, QString *error = nullptr);
    QImage segment(const QImage &picture, const SegmentPrompt &prompt, QString *error = nullptr) override;

    // How many times the picture was encoded (tests: the encoding is kept).
    int encodings() const { return m_encodings; }

private:
    std::unique_ptr<OnnxSession> m_encoder;
    std::unique_ptr<OnnxSession> m_decoder;
    QMutex m_mutex;
    qint64 m_cachedKey = 0;
    QSize m_cachedSize;
    std::array<OnnxTensor, 3> m_embeddings;
    int m_encodings = 0;
};

// The steps of SAM 2 around the models, as plain functions:
// The picture as the encoder takes it: 1 × 3 × 1024 × 1024, squeezed to the square, RGB scaled to
// 0–1 and normalised with ImageNet's mean and deviation.
std::vector<float> sam2Pixels(const QImage &picture);
// A point of the picture in the encoder's 1024 × 1024 space.
QPointF sam2Point(const QPointF &p, const QSize &picture);
// A 256 × 256 mask of logits as a mask of the picture's size (smooth scaling, then above 0).
QImage sam2Mask(const float *logits, int width, int height, const QSize &picture);
// The candidate with the highest quality score.
int bestCandidate(const std::vector<float> &scores, int offset, int count);

} // namespace lumen::ai
