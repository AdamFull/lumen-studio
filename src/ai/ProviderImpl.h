#pragma once

// Template implementations for Provider.h (kept separate for readability).

#include "net/HttpClient.h"

#include <QPromise>

#include <memory>

namespace lumen::ai {

template<typename T>
QFuture<T> futureFromCall(net::HttpCall *call, std::function<T(net::HttpCall *)> fn)
{
    auto promise = std::make_shared<QPromise<T>>();
    QFuture<T> future = promise->future();
    promise->start();
    QObject::connect(call, &net::HttpCall::finished, call, [call, promise, fn = std::move(fn)] {
        try {
            if (!call->ok())
                throw ProviderError(call->errorString());
            if constexpr (std::is_void_v<T>)
                fn(call);
            else
                promise->addResult(fn(call));
        } catch (...) {
            promise->setException(std::current_exception());
        }
        promise->finish();
    });
    return future;
}

} // namespace lumen::ai
