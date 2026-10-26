#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

struct OrtApi;
struct OrtEnv;
struct OrtSession;

namespace lumen::ai {

// A tensor going into or coming out of a model: float or int64 values with a shape.
struct OnnxTensor
{
    std::vector<int64_t> shape;
    std::vector<float> floats;
    std::vector<int64_t> ints;
    bool isInt = false;

    static OnnxTensor floatTensor(std::vector<int64_t> shape, std::vector<float> values);
    static OnnxTensor intTensor(std::vector<int64_t> shape, std::vector<int64_t> values);
    int64_t count() const;
};

// ONNX Runtime, loaded from its DLL at run time through its C API (plan M18 §18.1): Lumen does not
// link it; the runtime is a component downloaded when a feature first needs it
// (runtime/Components). One per process.
class OnnxRuntime
{
public:
    // The runtime in `dllPath` (onnxruntime.dll); the one already loaded when there is one.
    static std::shared_ptr<OnnxRuntime> load(const QString &dllPath, QString *error = nullptr);
    ~OnnxRuntime();

    QString version() const { return m_version; }
    const OrtApi *api() const { return m_api; }
    OrtEnv *env() const { return m_env; }

private:
    OnnxRuntime() = default;
    const OrtApi *m_api = nullptr;
    OrtEnv *m_env = nullptr;
    QString m_version;
    QString m_path;
};

// A model loaded into the runtime, run on the CPU. Runs may come from several threads at once.
class OnnxSession
{
public:
    // `threads` 0 = ONNX Runtime's choice (the processor's cores).
    static std::unique_ptr<OnnxSession> open(std::shared_ptr<OnnxRuntime> runtime, const QString &modelPath, QString *error = nullptr,
                                             int threads = 0);
    ~OnnxSession();

    QStringList inputNames() const { return m_inputs; }
    QStringList outputNames() const { return m_outputs; }
    // Runs the model with `inputs` by name and returns the `outputs` asked for (all when empty).
    std::optional<QHash<QString, OnnxTensor>> run(const QHash<QString, OnnxTensor> &inputs, const QStringList &outputs = {},
                                                  QString *error = nullptr) const;

private:
    OnnxSession() = default;
    std::shared_ptr<OnnxRuntime> m_runtime;
    OrtSession *m_session = nullptr;
    QStringList m_inputs;
    QStringList m_outputs;
};

} // namespace lumen::ai
