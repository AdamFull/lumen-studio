#include "ai/Detector.h"

#include "core/ImageOps.h"

#include <QCoreApplication>
#include <QFile>
#include <QMutexLocker>

#include <algorithm>
#include <cmath>

namespace lumen::ai {

namespace {

constexpr int kSide = 800;

bool isPunctuation(QChar c)
{
    const ushort u = c.unicode();
    // BERT counts ASCII symbols as punctuation too ("$", "^", "`"…).
    if ((u >= 33 && u <= 47) || (u >= 58 && u <= 64) || (u >= 91 && u <= 96) || (u >= 123 && u <= 126))
        return true;
    return c.isPunct();
}

float sigmoid(float x)
{
    return 1.0f / (1.0f + std::exp(-x));
}

double iou(const QRectF &a, const QRectF &b)
{
    const QRectF i = a.intersected(b);
    const double inter = i.isValid() ? i.width() * i.height() : 0;
    const double uni = a.width() * a.height() + b.width() * b.height() - inter;
    return uni > 0 ? inter / uni : 0;
}

} // namespace

bool WordPiece::load(const QString &vocabPath, QString *error)
{
    QFile f(vocabPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QCoreApplication::translate("lumen::ai::Detector", "The vocabulary %1 could not be read.").arg(vocabPath);
        return false;
    }
    QStringList tokens;
    while (!f.atEnd())
        tokens << QString::fromUtf8(f.readLine()).trimmed();
    setVocabulary(tokens);
    return true;
}

void WordPiece::setVocabulary(const QStringList &tokens)
{
    m_ids.clear();
    for (int i = 0; i < tokens.size(); ++i) {
        if (!tokens.at(i).isEmpty())
            m_ids.insert(tokens.at(i), i);
    }
}

std::vector<int64_t> WordPiece::encode(const QString &text, int maxLength) const
{
    // Lower case without accents (a letter and its marks apart, the marks dropped).
    QString clean;
    for (QChar c : text.normalized(QString::NormalizationForm_D)) {
        if (c.category() == QChar::Mark_NonSpacing || (c.category() == QChar::Other_Control && !c.isSpace()))
            continue;
        clean += c.toLower();
    }
    // Words: split at spaces; each punctuation mark a word of its own.
    QStringList words;
    QString word;
    for (QChar c : clean) {
        if (c.isSpace() || isPunctuation(c)) {
            if (!word.isEmpty())
                words << std::exchange(word, QString());
            if (!c.isSpace())
                words << QString(c);
        } else {
            word += c;
        }
    }
    if (!word.isEmpty())
        words << word;
    const int unk = m_ids.value(QStringLiteral("[UNK]"), 100);
    std::vector<int64_t> ids{m_ids.value(QStringLiteral("[CLS]"), 101)};
    for (const QString &w : words) {
        if (w.size() > 100) {
            ids.push_back(unk);
            continue;
        }
        // The longest pieces in the vocabulary, left to right.
        std::vector<int64_t> pieces;
        int start = 0;
        bool known = true;
        while (start < w.size()) {
            int end = int(w.size());
            int found = -1;
            for (; end > start; --end) {
                const QString piece = (start > 0 ? QStringLiteral("##") : QString()) + w.mid(start, end - start);
                if (const auto it = m_ids.constFind(piece); it != m_ids.cend()) {
                    found = it.value();
                    break;
                }
            }
            if (found < 0) {
                known = false;
                break;
            }
            pieces.push_back(found);
            start = end;
        }
        if (known)
            ids.insert(ids.end(), pieces.begin(), pieces.end());
        else
            ids.push_back(unk);
    }
    if (int(ids.size()) > maxLength - 1)
        ids.resize(size_t(maxLength - 1));
    ids.push_back(m_ids.value(QStringLiteral("[SEP]"), 102));
    return ids;
}

QString dinoText(const QString &description)
{
    QString t = description.simplified().toLower();
    while (t.endsWith(QLatin1Char('.')) || t.endsWith(QLatin1Char(',')))
        t.chop(1);
    return t.trimmed() + QStringLiteral(" .");
}

std::vector<float> dinoPixels(const QImage &picture)
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
            out[i] = (qRed(line[x]) / 255.0f - mean[0]) / deviation[0];
            out[plane + i] = (qGreen(line[x]) / 255.0f - mean[1]) / deviation[1];
            out[2 * plane + i] = (qBlue(line[x]) / 255.0f - mean[2]) / deviation[2];
        }
    }
    return out;
}

QList<Detection> dinoDecode(const std::vector<float> &logits, const std::vector<float> &boxes, int queries, int textLength,
                            const std::vector<int> &wordTokens, const QSize &picture, float threshold)
{
    QList<Detection> found;
    for (int q = 0; q < queries; ++q) {
        float best = 0;
        for (int t : wordTokens) {
            if (t < textLength)
                best = std::max(best, sigmoid(logits[size_t(q) * textLength + t]));
        }
        if (best <= threshold)
            continue;
        const float *b = boxes.data() + size_t(q) * 4; // centre x, centre y, width, height of 0–1
        QRectF r((b[0] - b[2] / 2) * picture.width(), (b[1] - b[3] / 2) * picture.height(), b[2] * picture.width(), b[3] * picture.height());
        r = r.intersected(QRectF(QPointF(0, 0), QSizeF(picture)));
        if (r.width() < 1 || r.height() < 1)
            continue;
        found << Detection{r, best};
    }
    std::sort(found.begin(), found.end(), [](const Detection &a, const Detection &b) { return a.score > b.score; });
    QList<Detection> kept;
    for (const Detection &d : std::as_const(found)) {
        bool overlaps = false;
        for (const Detection &k : std::as_const(kept))
            overlaps = overlaps || iou(d.box, k.box) > 0.5;
        if (!overlaps)
            kept << d;
    }
    return kept;
}

std::shared_ptr<GroundingDino> GroundingDino::open(std::shared_ptr<OnnxRuntime> runtime, const QString &modelPath, const QString &vocabPath,
                                                   QString *error)
{
    auto d = std::make_shared<GroundingDino>();
    if (!d->m_words.load(vocabPath, error))
        return {};
    d->m_model = OnnxSession::open(runtime, modelPath, error);
    if (!d->m_model)
        return {};
    return d;
}

QList<Detection> GroundingDino::detect(const QImage &picture, const QString &description, float threshold, QString *error)
{
    QMutexLocker lock(&m_mutex);
    const std::vector<int64_t> ids = m_words.encode(dinoText(description));
    const int64_t n = int64_t(ids.size());
    // The positions of the words (not [CLS], [SEP] or the closing ".").
    std::vector<int> words;
    const int dot = m_words.id(QStringLiteral("."));
    for (int i = 1; i + 1 < int(ids.size()); ++i) {
        if (ids[size_t(i)] != dot)
            words.push_back(i);
    }
    if (words.empty()) {
        if (error)
            *error = QCoreApplication::translate("lumen::ai::Detector", "Say what to find.");
        return {};
    }
    QHash<QString, OnnxTensor> inputs{
        {QStringLiteral("pixel_values"), OnnxTensor::floatTensor({1, 3, kSide, kSide}, dinoPixels(picture))},
        {QStringLiteral("input_ids"), OnnxTensor::intTensor({1, n}, ids)},
        {QStringLiteral("token_type_ids"), OnnxTensor::intTensor({1, n}, std::vector<int64_t>(size_t(n), 0))},
        {QStringLiteral("attention_mask"), OnnxTensor::intTensor({1, n}, std::vector<int64_t>(size_t(n), 1))},
        {QStringLiteral("pixel_mask"), OnnxTensor::intTensor({1, kSide, kSide}, std::vector<int64_t>(size_t(kSide) * kSide, 1))},
    };
    const auto out = m_model->run(inputs, {QStringLiteral("logits"), QStringLiteral("pred_boxes")}, error);
    if (!out)
        return {};
    const OnnxTensor logits = out->value(QStringLiteral("logits"));
    const OnnxTensor boxes = out->value(QStringLiteral("pred_boxes"));
    if (logits.shape.size() != 3 || boxes.shape.size() != 3) {
        if (error)
            *error = QCoreApplication::translate("lumen::ai::Detector", "The detection model gave an answer Lumen does not understand.");
        return {};
    }
    return dinoDecode(logits.floats, boxes.floats, int(logits.shape[1]), int(logits.shape[2]), words, picture.size(), threshold);
}

QImage findObjects(GroundingDino &detector, Segmenter &segmenter, const QImage &picture, const QString &description, int *found,
                   QString *error, float threshold)
{
    const QList<Detection> boxes = detector.detect(picture, description, threshold, error);
    if (found)
        *found = int(boxes.size());
    if (boxes.isEmpty()) {
        if (error && error->isEmpty())
            *error = QCoreApplication::translate("lumen::ai::Detector", "Nothing that looks like \"%1\" was found.").arg(description);
        return {};
    }
    QImage mask = img::createMask(picture.size());
    for (const Detection &d : boxes) {
        SegmentPrompt prompt;
        prompt.box = d.box;
        const QImage object = segmenter.segment(picture, prompt, error);
        if (object.isNull())
            return {};
        mask = img::combineMasks(mask, object, img::MaskOp::Union);
    }
    return mask;
}

} // namespace lumen::ai
