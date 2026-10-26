#include "ai/Onnx.h"

#include <onnxruntime_c_api.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QMutex>

namespace lumen::ai {

namespace {

// The message of an ONNX Runtime status, which is released; empty for success.
QString takeError(const OrtApi *api, OrtStatus *status)
{
    if (!status)
        return {};
    const QString message = QString::fromUtf8(api->GetErrorMessage(status));
    api->ReleaseStatus(status);
    return message.isEmpty() ? QCoreApplication::translate("lumen::ai::Onnx", "ONNX Runtime failed.") : message;
}

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

std::weak_ptr<OnnxRuntime> &loaded()
{
    static std::weak_ptr<OnnxRuntime> runtime;
    return runtime;
}

QMutex &loadMutex()
{
    static QMutex mutex;
    return mutex;
}

} // namespace

OnnxTensor OnnxTensor::floatTensor(std::vector<int64_t> shape, std::vector<float> values)
{
    OnnxTensor t;
    t.shape = std::move(shape);
    t.floats = std::move(values);
    return t;
}

OnnxTensor OnnxTensor::intTensor(std::vector<int64_t> shape, std::vector<int64_t> values)
{
    OnnxTensor t;
    t.shape = std::move(shape);
    t.ints = std::move(values);
    t.isInt = true;
    return t;
}

int64_t OnnxTensor::count() const
{
    int64_t n = 1;
    for (int64_t d : shape)
        n *= d;
    return n;
}

std::shared_ptr<OnnxRuntime> OnnxRuntime::load(const QString &dllPath, QString *error)
{
    QMutexLocker lock(&loadMutex());
    if (std::shared_ptr<OnnxRuntime> rt = loaded().lock())
        return rt;
    // Its own folder first, so onnxruntime_providers_shared.dll next to it is the one found.
    QLibrary library(dllPath);
    library.setLoadHints(QLibrary::PreventUnloadHint);
    if (!library.load()) {
        setError(error, QCoreApplication::translate("lumen::ai::Onnx", "ONNX Runtime could not be loaded from %1: %2").arg(QDir::toNativeSeparators(dllPath), library.errorString()));
        return {};
    }
    using GetApiBase = const OrtApiBase *(*)();
    auto getBase = reinterpret_cast<GetApiBase>(library.resolve("OrtGetApiBase"));
    const OrtApiBase *base = getBase ? getBase() : nullptr;
    const OrtApi *api = base ? base->GetApi(ORT_API_VERSION) : nullptr;
    if (!api) {
        setError(error, QCoreApplication::translate("lumen::ai::Onnx", "%1 is not ONNX Runtime %2 or newer.").arg(QDir::toNativeSeparators(dllPath)).arg(QStringLiteral("1.%1").arg(ORT_API_VERSION)));
        return {};
    }
    std::shared_ptr<OnnxRuntime> rt(new OnnxRuntime);
    rt->m_api = api;
    rt->m_version = QString::fromUtf8(base->GetVersionString());
    rt->m_path = dllPath;
    if (const QString e = takeError(api, api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "lumen", &rt->m_env)); !e.isEmpty()) {
        setError(error, e);
        return {};
    }
    loaded() = rt;
    return rt;
}

OnnxRuntime::~OnnxRuntime()
{
    if (m_api && m_env)
        m_api->ReleaseEnv(m_env);
}

std::unique_ptr<OnnxSession> OnnxSession::open(std::shared_ptr<OnnxRuntime> runtime, const QString &modelPath, QString *error, int threads)
{
    if (!runtime) {
        setError(error, QCoreApplication::translate("lumen::ai::Onnx", "ONNX Runtime is not loaded."));
        return {};
    }
    if (!QFileInfo::exists(modelPath)) {
        setError(error, QCoreApplication::translate("lumen::ai::Onnx", "The model file %1 is not there.").arg(QDir::toNativeSeparators(modelPath)));
        return {};
    }
    const OrtApi *api = runtime->api();
    OrtSessionOptions *options = nullptr;
    if (const QString e = takeError(api, api->CreateSessionOptions(&options)); !e.isEmpty()) {
        setError(error, e);
        return {};
    }
    if (threads > 0)
        takeError(api, api->SetIntraOpNumThreads(options, threads));
    takeError(api, api->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL));
    std::unique_ptr<OnnxSession> s(new OnnxSession);
    s->m_runtime = runtime;
    // The model's external weights (".onnx_data") are found next to it.
    const std::wstring path = QDir::toNativeSeparators(modelPath).toStdWString();
    const QString e = takeError(api, api->CreateSession(runtime->env(), path.c_str(), options, &s->m_session));
    api->ReleaseSessionOptions(options);
    if (!e.isEmpty()) {
        setError(error, QCoreApplication::translate("lumen::ai::Onnx", "The model %1 could not be loaded: %2").arg(QFileInfo(modelPath).fileName(), e));
        return {};
    }
    OrtAllocator *allocator = nullptr;
    takeError(api, api->GetAllocatorWithDefaultOptions(&allocator));
    size_t count = 0;
    takeError(api, api->SessionGetInputCount(s->m_session, &count));
    for (size_t i = 0; i < count; ++i) {
        char *name = nullptr;
        if (takeError(api, api->SessionGetInputName(s->m_session, i, allocator, &name)).isEmpty() && name) {
            s->m_inputs << QString::fromUtf8(name);
            allocator->Free(allocator, name);
        }
    }
    takeError(api, api->SessionGetOutputCount(s->m_session, &count));
    for (size_t i = 0; i < count; ++i) {
        char *name = nullptr;
        if (takeError(api, api->SessionGetOutputName(s->m_session, i, allocator, &name)).isEmpty() && name) {
            s->m_outputs << QString::fromUtf8(name);
            allocator->Free(allocator, name);
        }
    }
    return s;
}

OnnxSession::~OnnxSession()
{
    if (m_session)
        m_runtime->api()->ReleaseSession(m_session);
}

std::optional<QHash<QString, OnnxTensor>> OnnxSession::run(const QHash<QString, OnnxTensor> &inputs, const QStringList &wanted,
                                                           QString *error) const
{
    const OrtApi *api = m_runtime->api();
    OrtMemoryInfo *memory = nullptr;
    if (const QString e = takeError(api, api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory)); !e.isEmpty()) {
        setError(error, e);
        return std::nullopt;
    }
    std::vector<QByteArray> inNames;
    std::vector<const char *> inPtrs;
    std::vector<OrtValue *> inValues;
    auto release = [&] {
        for (OrtValue *v : inValues)
            api->ReleaseValue(v);
        api->ReleaseMemoryInfo(memory);
    };
    for (auto it = inputs.cbegin(); it != inputs.cend(); ++it) {
        const OnnxTensor &t = it.value();
        OrtValue *value = nullptr;
        void *data = t.isInt ? static_cast<void *>(const_cast<int64_t *>(t.ints.data())) : static_cast<void *>(const_cast<float *>(t.floats.data()));
        const size_t bytes = t.isInt ? t.ints.size() * sizeof(int64_t) : t.floats.size() * sizeof(float);
        const QString e = takeError(api, api->CreateTensorWithDataAsOrtValue(memory, data, bytes, t.shape.data(), t.shape.size(),
                                                                             t.isInt ? ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64
                                                                                     : ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                                                                             &value));
        if (!e.isEmpty()) {
            release();
            setError(error, QCoreApplication::translate("lumen::ai::Onnx", "The input %1 does not fit the model: %2").arg(it.key(), e));
            return std::nullopt;
        }
        inNames.push_back(it.key().toUtf8());
        inValues.push_back(value);
    }
    for (const QByteArray &n : inNames)
        inPtrs.push_back(n.constData());
    const QStringList outNamesList = wanted.isEmpty() ? m_outputs : wanted;
    std::vector<QByteArray> outNames;
    std::vector<const char *> outPtrs;
    for (const QString &n : outNamesList)
        outNames.push_back(n.toUtf8());
    for (const QByteArray &n : outNames)
        outPtrs.push_back(n.constData());
    std::vector<OrtValue *> outValues(outPtrs.size(), nullptr);
    const QString e = takeError(api, api->Run(m_session, nullptr, inPtrs.data(), inValues.data(), inValues.size(), outPtrs.data(),
                                              outPtrs.size(), outValues.data()));
    release();
    if (!e.isEmpty()) {
        setError(error, e);
        return std::nullopt;
    }
    QHash<QString, OnnxTensor> out;
    QString readError;
    for (size_t i = 0; i < outValues.size(); ++i) {
        OrtValue *v = outValues[i];
        OrtTensorTypeAndShapeInfo *info = nullptr;
        if (readError.isEmpty())
            readError = takeError(api, api->GetTensorTypeAndShape(v, &info));
        if (readError.isEmpty()) {
            size_t dims = 0;
            api->GetDimensionsCount(info, &dims);
            std::vector<int64_t> shape(dims);
            api->GetDimensions(info, shape.data(), dims);
            ONNXTensorElementDataType type = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
            api->GetTensorElementType(info, &type);
            size_t n = 0;
            api->GetTensorShapeElementCount(info, &n);
            void *data = nullptr;
            readError = takeError(api, api->GetTensorMutableData(v, &data));
            if (readError.isEmpty()) {
                if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
                    const auto *f = static_cast<const float *>(data);
                    out.insert(outNamesList.at(int(i)), OnnxTensor::floatTensor(shape, std::vector<float>(f, f + n)));
                } else if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64) {
                    const auto *f = static_cast<const int64_t *>(data);
                    out.insert(outNamesList.at(int(i)), OnnxTensor::intTensor(shape, std::vector<int64_t>(f, f + n)));
                } else {
                    readError = QCoreApplication::translate("lumen::ai::Onnx", "The output %1 has a type Lumen does not read.").arg(outNamesList.at(int(i)));
                }
            }
        }
        if (info)
            api->ReleaseTensorTypeAndShapeInfo(info);
        if (v)
            api->ReleaseValue(v);
    }
    if (!readError.isEmpty()) {
        setError(error, readError);
        return std::nullopt;
    }
    return out;
}

} // namespace lumen::ai
