#pragma once

#include <QByteArray>
#include <QImage>
#include <QMutex>

#include <memory>

namespace lumen::doc {

// Holds an image that is compressed (zlib, raw pixels) in the background after creation, so undo
// history occupies a fraction of the raw memory. Copies share state. Thread-safe.
class CompressedImage
{
public:
    CompressedImage() = default;
    explicit CompressedImage(const QImage &image, bool compressInBackground = true);

    bool isNull() const;
    QImage image() const;          // decompresses on demand when needed
    QSize size() const;
    qint64 memoryBytes() const;    // current footprint

private:
    struct State
    {
        mutable QMutex mutex;
        QImage image;
        QByteArray compressed;
        QSize size;
        QImage::Format format = QImage::Format_Invalid;
        qsizetype bytesPerLine = 0;
    };
    std::shared_ptr<State> m_state;
};

} // namespace lumen::doc
