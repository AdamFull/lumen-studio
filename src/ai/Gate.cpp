#include "ai/Gate.h"

#include "core/Error.h"

#include <QPointer>

namespace lumen::ai {

namespace {

template<typename Op>
void forwardCommon(Op *outer, Op *inner)
{
    QObject::connect(inner, &Operation::progress, outer, &Operation::reportProgress);
    QObject::connect(inner, &Operation::preview, outer, &Operation::reportPreview);
}

void forwardExtra(ImageOperation *, ImageOperation *) {}

void forwardExtra(ChatOperation *outer, ChatOperation *inner)
{
    QObject::connect(inner, &ChatOperation::textDelta, outer, &ChatOperation::appendText);
    QObject::connect(inner, &ChatOperation::reasoningDelta, outer, &ChatOperation::appendReasoning);
}

template<typename Op>
void settle(Op *outer, Op *inner)
{
    switch (inner->state()) {
    case Operation::State::Succeeded:
        outer->succeed(inner->result());
        break;
    case Operation::State::Cancelled:
        outer->reportCancelled();
        break;
    case Operation::State::Failed:
        outer->fail(inner->errorString());
        break;
    case Operation::State::Running:
        break;
    }
}

template<typename Op>
Op *gated(Provider *provider, std::function<Op *()> start)
{
    const std::shared_ptr<ProviderGate> gate = provider->gate();
    if (!gate)
        return start();
    auto *outer = new Op(provider);
    auto inner = std::make_shared<QPointer<Op>>();
    auto released = std::make_shared<bool>(false);
    auto release = [gate, released] {
        if (!*released) {
            *released = true;
            gate->release();
        }
    };
    outer->onCancel([outer, inner] {
        if (*inner)
            (*inner)->cancel();
        else
            outer->reportCancelled(); // still waiting for the server
    });
    QObject::connect(outer, &Operation::finished, outer, release);
    outer->reportProgress(-1, QObject::tr("Starting %1…").arg(provider->displayName()));
    QPointer<Op> guard(outer);
    gate->acquire().then(outer, [guard, inner, start](const QString &error) {
        if (!guard || guard->isFinished())
            return;
        if (!error.isEmpty()) {
            guard->fail(error);
            return;
        }
        Op *op = start();
        *inner = op;
        if (!op) {
            guard->fail(QObject::tr("The provider did not start the request."));
            return;
        }
        forwardCommon(guard.data(), op);
        forwardExtra(guard.data(), op);
        if (op->isFinished()) {
            settle(guard.data(), op);
            return;
        }
        QObject::connect(op, &Operation::finished, guard.data(), [guard, op] {
            if (guard)
                settle(guard.data(), op);
        });
    });
    return outer;
}

template<typename T>
QFuture<T> gatedFuture(Provider *provider, std::function<QFuture<T>()> fn)
{
    const std::shared_ptr<ProviderGate> gate = provider->gate();
    if (!gate)
        return fn();
    return gate->acquire().then(provider, [gate, fn](const QString &error) {
        if (!error.isEmpty()) {
            gate->release();
            throw Error(error);
        }
        return fn()
            .then([gate](T value) {
                gate->release();
                return value;
            })
            .onFailed([gate](const QException &e) -> T {
                gate->release();
                e.raise();
                return T();
            });
    }).unwrap();
}

} // namespace

ImageOperation *gatedGenerate(ImageProvider *provider, const ImageRequest &request)
{
    return gated<ImageOperation>(provider, [provider, request] { return provider->generate(request); });
}

ImageOperation *gatedUtility(ImageProvider *provider, Utility utility, const QImage &image, const QJsonObject &params)
{
    return gated<ImageOperation>(provider, [provider, utility, image, params] { return provider->runUtility(utility, image, params); });
}

ChatOperation *gatedChat(LlmProvider *provider, const ChatRequest &request)
{
    return gated<ChatOperation>(provider, [provider, request] { return provider->chat(request); });
}

QFuture<QString> gatedCall(Provider *provider, std::function<QFuture<QString>()> fn)
{
    return gatedFuture<QString>(provider, std::move(fn));
}

QFuture<QStringList> gatedList(Provider *provider, std::function<QFuture<QStringList>()> fn)
{
    return gatedFuture<QStringList>(provider, std::move(fn));
}

} // namespace lumen::ai
