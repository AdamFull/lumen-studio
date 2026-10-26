#include "ai/Segmenter.h"

#include "core/ImageOps.h"

#include <QCoreApplication>
#include <QMutexLocker>

#include <algorithm>
#include <cmath>

namespace lumen::ai {

namespace {

constexpr int kSide = 1024;

} // namespace

std::vector<float> sam2Pixels(const QImage &picture)
{
    const QImage im = img::flatten(picture, Qt::white)
                          .scaled(kSide, kSide, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                          .convertToFormat(QImage::Format_RGB32);
    static constexpr float mean[3] = {0.485f, 0.456f, 0.406f};
    static constexpr float deviation[3] = {0.229f, 0.224f, 0.225f};
    std::vector<float> out(size_t(3) * kSide * kSide);
    const size_t plane = size_t(kSide) * kSide;
    for (int y = 0; y < kSide; ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(im.constScanLine(y));
        for (int x = 0; x < kSide; ++x) {
            const size_t i = size_t(y) * kSide + x;
            const QRgb p = line[x];
            out[i] = (qRed(p) / 255.0f - mean[0]) / deviation[0];
            out[plane + i] = (qGreen(p) / 255.0f - mean[1]) / deviation[1];
            out[2 * plane + i] = (qBlue(p) / 255.0f - mean[2]) / deviation[2];
        }
    }
    return out;
}

QPointF sam2Point(const QPointF &p, const QSize &picture)
{
    return {p.x() * kSide / std::max(1, picture.width()), p.y() * kSide / std::max(1, picture.height())};
}

QImage sam2Mask(const float *logits, int width, int height, const QSize &picture)
{
    // The logits as 0–255 (a sigmoid), scaled smoothly to the picture, then halfway is the edge:
    // the same as thresholding the scaled logits at 0, without scaling floats.
    QImage low(width, height, QImage::Format_Grayscale8);
    for (int y = 0; y < height; ++y) {
        uchar *line = low.scanLine(y);
        for (int x = 0; x < width; ++x) {
            const float l = std::clamp(logits[size_t(y) * width + x], -20.0f, 20.0f);
            line[x] = uchar(std::lround(255.0 / (1.0 + std::exp(-l))));
        }
    }
    const QImage scaled = low.scaled(picture, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return img::thresholdMask(scaled, 128);
}

int bestCandidate(const std::vector<float> &scores, int offset, int count)
{
    int best = 0;
    for (int i = 1; i < count; ++i) {
        if (scores[size_t(offset + i)] > scores[size_t(offset + best)])
            best = i;
    }
    return best;
}

std::shared_ptr<Sam2Segmenter> Sam2Segmenter::open(std::shared_ptr<OnnxRuntime> runtime, const QString &encoderPath,
                                                   const QString &decoderPath, QString *error)
{
    auto s = std::make_shared<Sam2Segmenter>();
    s->m_encoder = OnnxSession::open(runtime, encoderPath, error);
    if (!s->m_encoder)
        return {};
    s->m_decoder = OnnxSession::open(runtime, decoderPath, error);
    if (!s->m_decoder)
        return {};
    return s;
}

QImage Sam2Segmenter::segment(const QImage &picture, const SegmentPrompt &prompt, QString *error)
{
    QMutexLocker lock(&m_mutex);
    if (picture.isNull() || prompt.isEmpty()) {
        if (error)
            *error = QCoreApplication::translate("lumen::ai::Segmenter", "Click on the object or draw a box around it.");
        return {};
    }
    // The picture's features, once per picture.
    if (picture.cacheKey() != m_cachedKey || picture.size() != m_cachedSize) {
        const auto out = m_encoder->run({{QStringLiteral("pixel_values"), OnnxTensor::floatTensor({1, 3, kSide, kSide}, sam2Pixels(picture))}},
                                        {QStringLiteral("image_embeddings.0"), QStringLiteral("image_embeddings.1"),
                                         QStringLiteral("image_embeddings.2")},
                                        error);
        if (!out)
            return {};
        for (int i = 0; i < 3; ++i)
            m_embeddings[size_t(i)] = out->value(QStringLiteral("image_embeddings.%1").arg(i));
        // The decoder takes the last as 256 × 64 × 64, however the encoder shapes it.
        m_embeddings[2].shape = {1, 256, 64, 64};
        m_cachedKey = picture.cacheKey();
        m_cachedSize = picture.size();
        ++m_encodings;
    }
    // Points (1 = on the object, 0 = off it); a box as its two corners (labels 2 and 3), as SAM
    // takes boxes among its points.
    std::vector<float> points;
    std::vector<int64_t> labels;
    for (int i = 0; i < prompt.points.size(); ++i) {
        const QPointF p = sam2Point(prompt.points.at(i), picture.size());
        points.push_back(float(p.x()));
        points.push_back(float(p.y()));
        labels.push_back(prompt.positive.value(i, true) ? 1 : 0);
    }
    if (!prompt.box.isNull()) {
        const QPointF a = sam2Point(prompt.box.topLeft(), picture.size());
        const QPointF b = sam2Point(prompt.box.bottomRight(), picture.size());
        points.insert(points.end(), {float(a.x()), float(a.y()), float(b.x()), float(b.y())});
        labels.insert(labels.end(), {2, 3});
    }
    const int64_t n = int64_t(labels.size());
    QHash<QString, OnnxTensor> inputs{
        {QStringLiteral("input_points"), OnnxTensor::floatTensor({1, 1, n, 2}, points)},
        {QStringLiteral("input_labels"), OnnxTensor::intTensor({1, 1, n}, labels)},
        {QStringLiteral("input_boxes"), OnnxTensor::floatTensor({1, 0, 4}, {})},
        {QStringLiteral("image_embeddings.0"), m_embeddings[0]},
        {QStringLiteral("image_embeddings.1"), m_embeddings[1]},
        {QStringLiteral("image_embeddings.2"), m_embeddings[2]},
    };
    const auto out = m_decoder->run(inputs, {QStringLiteral("iou_scores"), QStringLiteral("pred_masks")}, error);
    if (!out)
        return {};
    const OnnxTensor scores = out->value(QStringLiteral("iou_scores"));
    const OnnxTensor masks = out->value(QStringLiteral("pred_masks"));
    if (masks.shape.size() != 5 || scores.floats.empty()) {
        if (error)
            *error = QCoreApplication::translate("lumen::ai::Segmenter", "The segmentation model gave an answer Lumen does not understand.");
        return {};
    }
    const int candidates = int(masks.shape[2]);
    const int h = int(masks.shape[3]);
    const int w = int(masks.shape[4]);
    const int best = bestCandidate(scores.floats, 0, std::min<int>(candidates, int(scores.floats.size())));
    return sam2Mask(masks.floats.data() + size_t(best) * w * h, w, h, picture.size());
}

} // namespace lumen::ai
