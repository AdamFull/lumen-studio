#pragma once

#include "ai/Onnx.h"
#include "ai/Segmenter.h"

#include <QHash>
#include <QImage>
#include <QList>
#include <QMutex>
#include <QRectF>
#include <QString>

#include <memory>
#include <vector>

namespace lumen::ai {

// BERT's uncased WordPiece tokenizer, as Grounding DINO reads its words: lower case without accents,
// split at spaces and punctuation, each word into the longest pieces the vocabulary has ("##" for a
// piece inside a word), [CLS] … [SEP] around it.
class WordPiece
{
public:
    bool load(const QString &vocabPath, QString *error = nullptr);
    void setVocabulary(const QStringList &tokens);
    // The ids of `text` with [CLS] and [SEP], at most `maxLength` of them.
    std::vector<int64_t> encode(const QString &text, int maxLength = 256) const;
    int id(const QString &token) const { return m_ids.value(token, -1); }

private:
    QHash<QString, int> m_ids;
};

// An object found in a picture: its box (picture pixels) and how sure the model is (0–1).
struct Detection
{
    QRectF box;
    float score = 0;
};

// Grounding DINO tiny through ONNX Runtime (plan M18 §18.1): boxes of the objects a description
// names. The picture is squeezed to 800 × 800; each of the 900 candidate boxes scores against the
// description's words, and those above the threshold are kept (overlapping ones merged).
class GroundingDino
{
public:
    static std::shared_ptr<GroundingDino> open(std::shared_ptr<OnnxRuntime> runtime, const QString &modelPath, const QString &vocabPath,
                                               QString *error = nullptr);
    QList<Detection> detect(const QImage &picture, const QString &description, float threshold = 0.3f, QString *error = nullptr);

private:
    std::unique_ptr<OnnxSession> m_model;
    WordPiece m_words;
    QMutex m_mutex;
};

// The steps of Grounding DINO around the model, as plain functions:
// The description as the model reads it: lower case, ending with " ." (how it separates phrases).
QString dinoText(const QString &description);
// The picture as the model takes it: 1 × 3 × 800 × 800, normalised with ImageNet's mean and deviation.
std::vector<float> dinoPixels(const QImage &picture);
// The boxes whose best score over the description's word tokens (`wordTokens`: their positions)
// passes the threshold, in picture pixels, without boxes that overlap a better one by more than half.
QList<Detection> dinoDecode(const std::vector<float> &logits, const std::vector<float> &boxes, int queries, int textLength,
                            const std::vector<int> &wordTokens, const QSize &picture, float threshold);

// The area of the objects a description names: their boxes from the detector, each cut out by the
// segmenter, together. `found` gets how many. Null when none is found (with `error` saying so).
QImage findObjects(GroundingDino &detector, Segmenter &segmenter, const QImage &picture, const QString &description, int *found = nullptr,
                   QString *error = nullptr, float threshold = 0.3f);

// The area an image engine's FindArea gave (masks of any size, white on black or Grayscale8),
// together at `size`. `objects` gets how many masks were not empty. Null when all are empty.
QImage areaFromMasks(const QList<QImage> &masks, const QSize &size, int *objects = nullptr);

} // namespace lumen::ai
