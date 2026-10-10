#pragma once

#include "ai/Provider.h"

#include <functional>

namespace lumen::ai {

// Entry points for provider requests that respect the provider's gate (ProviderGate): when a
// managed server has to start first, they return a proxy operation at once that reports
// "Starting server…", sends the request once the gate opens, forwards progress/previews/text and
// releases the gate when done. Without a gate they call the provider directly.
ImageOperation *gatedGenerate(ImageProvider *provider, const ImageRequest &request);
ImageOperation *gatedUtility(ImageProvider *provider, Utility utility, const QImage &image, const QJsonObject &params);
ChatOperation *gatedChat(LlmProvider *provider, const ChatRequest &request);

// For one-off calls (test connection, model lists): acquires the gate, runs fn, releases.
QFuture<QString> gatedCall(Provider *provider, std::function<QFuture<QString>()> fn);
QFuture<QStringList> gatedList(Provider *provider, std::function<QFuture<QStringList>()> fn);

} // namespace lumen::ai
