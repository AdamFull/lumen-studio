#pragma once

#include "ai/Operation.h"
#include "net/HttpClient.h"

#include <QNetworkRequest>

#include <functional>
#include <memory>

namespace lumen::ai {

struct Payload
{
    QByteArray body;
    QByteArray contentType = QByteArrayLiteral("application/json");
};

struct HttpResponse
{
    int status = 0;
    QByteArray body;
    QByteArray contentType;
};

// State shared between an operation and its in-flight HTTP call.
struct RequestState
{
    QPointer<net::HttpCall> call;
    bool cancelled = false;
};

// Runs an image request: builds the payload on a worker thread (base64/PNG encoding of large
// images), sends it, and parses the response on a worker thread (image decoding). Handles cancel.
// onStarted is invoked on the GUI thread right after the request was sent.
void runImageRequest(ImageOperation *op, net::HttpClient *http, const QNetworkRequest &request,
                     std::function<Payload()> build, std::function<ImageResult(const HttpResponse &)> parse,
                     const net::CallOptions &options = {}, std::function<void(net::HttpCall *)> onStarted = {},
                     std::function<void()> onCancel = {});

// Same for GET requests without a body.
void runImageGet(ImageOperation *op, net::HttpClient *http, const QNetworkRequest &request,
                 std::function<ImageResult(const HttpResponse &)> parse, const net::CallOptions &options = {});

// Fails an operation asynchronously (so callers can connect before the signal fires).
void failLater(Operation *op, const QString &message);

QString toBase64Png(const QImage &image);
QImage decodeBase64Image(const QString &data);
// Reads an image either from a base64 JSON payload or from raw bytes depending on content type.
QImage decodeImageBytes(const QByteArray &bytes);

// "WxH" helpers.
QString sizeString(const QSize &s);
// Closest supported aspect ratio string (e.g. "16:9") for providers that take ratios.
QString closestAspectRatio(const QSize &size, const QStringList &ratios);

} // namespace lumen::ai
