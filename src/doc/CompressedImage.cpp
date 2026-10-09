#include "doc/CompressedImage.h"

#include "core/Async.h"

namespace lumen::doc {

namespace {
// Small regions are cheap to keep raw and not worth a background task.
constexpr qint64 kCompressThreshold = 64 * 1024;
}

CompressedImage::CompressedImage(const QImage &image, bool compressInBackground)
    : m_state(std::make_shared<State>())
{
    m_state->image = image;
    m_state->size = image.size();
    m_state->format = image.format();
    m_state->bytesPerLine = image.bytesPerLine();
    if (!compressInBackground || image.isNull() || image.sizeInBytes() < kCompressThreshold)
        return;
    std::weak_ptr<State> weak = m_state;
    async::run([weak, image] {
        const QByteArray raw = QByteArray::fromRawData(reinterpret_cast<const char *>(image.constBits()), image.sizeInBytes());
        QByteArray packed = qCompress(raw, 1);
        if (auto s = weak.lock()) {
            QMutexLocker lock(&s->mutex);
            if (s->compressed.isEmpty() && packed.size() < raw.size()) {
                s->compressed = std::move(packed);
                s->image = QImage();
            }
        }
    });
}

bool CompressedImage::isNull() const
{
    if (!m_state)
        return true;
    QMutexLocker lock(&m_state->mutex);
    return m_state->image.isNull() && m_state->compressed.isEmpty();
}

QImage CompressedImage::image() const
{
    if (!m_state)
        return {};
    QMutexLocker lock(&m_state->mutex);
    if (!m_state->image.isNull())
        return m_state->image;
    if (m_state->compressed.isEmpty())
        return {};
    const QByteArray raw = qUncompress(m_state->compressed);
    QImage img(m_state->size, m_state->format);
    if (raw.size() != img.sizeInBytes() || img.bytesPerLine() != m_state->bytesPerLine)
        return {};
    memcpy(img.bits(), raw.constData(), static_cast<size_t>(raw.size()));
    return img;
}

QSize CompressedImage::size() const
{
    return m_state ? m_state->size : QSize();
}

qint64 CompressedImage::memoryBytes() const
{
    if (!m_state)
        return 0;
    QMutexLocker lock(&m_state->mutex);
    return m_state->image.isNull() ? m_state->compressed.size() : m_state->image.sizeInBytes();
}

} // namespace lumen::doc
