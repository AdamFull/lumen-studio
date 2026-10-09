#include "ai/ProviderUtil.h"

#include "core/Async.h"
#include "core/ImageOps.h"

#include <cmath>

namespace lumen::ai {

namespace {

void sendAndParse(ImageOperation *op, net::HttpClient *http, std::shared_ptr<RequestState> state,
                  std::function<net::HttpCall *()> send, std::function<ImageResult(const HttpResponse &)> parse,
                  std::function<void(net::HttpCall *)> onStarted)
{
    if (state->cancelled) {
        op->reportCancelled();
        return;
    }
    Q_UNUSED(http);
    net::HttpCall *call = send();
    state->call = call;
    QObject::connect(call, &net::HttpCall::finished, op, [op, call, state, parse] {
        state->call = nullptr;
        if (state->cancelled || call->isAborted()) {
            op->reportCancelled();
            return;
        }
        if (!call->ok()) {
            op->fail(call->errorString());
            return;
        }
        HttpResponse resp{call->status(), call->body(), call->rawHeader("content-type")};
        op->reportProgress(-1.0, QObject::tr("Decoding result..."));
        async::run([parse, resp] { return parse(resp); })
            .then(op, [op](ImageResult result) {
                if (result.images.isEmpty() && result.text.isEmpty())
                    op->fail(QObject::tr("The provider returned no images"));
                else
                    op->succeed(std::move(result));
            })
            .onFailed(op, [op](const std::exception &e) { op->fail(describeException(e)); });
    });
    if (onStarted)
        onStarted(call);
}

} // namespace

void runImageRequest(ImageOperation *op, net::HttpClient *http, const QNetworkRequest &request,
                     std::function<Payload()> build, std::function<ImageResult(const HttpResponse &)> parse,
                     const net::CallOptions &options, std::function<void(net::HttpCall *)> onStarted,
                     std::function<void()> onCancel)
{
    auto state = std::make_shared<RequestState>();
    QPointer<ImageOperation> guard(op);
    op->onCancel([state, guard, onCancel] {
        state->cancelled = true;
        if (onCancel)
            onCancel();
        if (state->call)
            state->call->abort();
        else if (guard)
            guard->reportCancelled();
    });
    op->reportProgress(-1.0, QObject::tr("Preparing request..."));
    async::run([build] { return build(); })
        .then(op, [op, http, request, parse, options, onStarted, state](Payload payload) {
            op->reportProgress(-1.0, QObject::tr("Waiting for the server..."));
            sendAndParse(op, http, state, [http, request, payload, options] {
                return http->post(request, payload.body, payload.contentType, options);
            }, parse, onStarted);
        })
        .onFailed(op, [op](const std::exception &e) { op->fail(describeException(e)); });
}

void runImageGet(ImageOperation *op, net::HttpClient *http, const QNetworkRequest &request,
                 std::function<ImageResult(const HttpResponse &)> parse, const net::CallOptions &options)
{
    auto state = std::make_shared<RequestState>();
    op->onCancel([state, op] {
        state->cancelled = true;
        if (state->call)
            state->call->abort();
        else
            op->reportCancelled();
    });
    sendAndParse(op, http, state, [http, request, options] { return http->get(request, options); }, parse, {});
}

void failLater(Operation *op, const QString &message)
{
    QMetaObject::invokeMethod(op, [op, message] { op->fail(message); }, Qt::QueuedConnection);
}

QString toBase64Png(const QImage &image)
{
    return QString::fromLatin1(img::toBase64(image, "PNG", 1));
}

QImage decodeBase64Image(const QString &data)
{
    return img::normalized(img::fromBase64(data.toLatin1()));
}

QImage decodeImageBytes(const QByteArray &bytes)
{
    return img::normalized(img::decode(bytes));
}

QString sizeString(const QSize &s)
{
    return QStringLiteral("%1x%2").arg(s.width()).arg(s.height());
}

QString closestAspectRatio(const QSize &size, const QStringList &ratios)
{
    if (ratios.isEmpty() || size.isEmpty())
        return QStringLiteral("1:1");
    const double target = std::log(double(size.width()) / size.height());
    QString best = ratios.first();
    double bestDiff = 1e9;
    for (const QString &r : ratios) {
        const QStringList parts = r.split(u':');
        if (parts.size() != 2)
            continue;
        const double v = std::log(parts[0].toDouble() / parts[1].toDouble());
        const double diff = std::abs(v - target);
        if (diff < bestDiff) {
            bestDiff = diff;
            best = r;
        }
    }
    return best;
}

} // namespace lumen::ai
