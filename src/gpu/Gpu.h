#pragma once

#include <QHash>
#include <QImage>
#include <QList>
#include <QMutex>
#include <QSize>
#include <QString>
#include <QVariant>

#include <memory>

// Compute work on the graphics card through Vulkan (plan M20): pictures go up, Slang kernels run on
// them, results come back. Headless: nothing here draws to the screen. The design follows the user's
// nx-gfx (resources that know their state, barriers from that state, uploads and read-backs, kernels
// described by their reflection), on Qt's types.
namespace lumen::gpu {

enum class Format { RGBA8, RGBA16F };

struct DeviceState;
struct TextureState;
struct KernelState;

// A picture on the graphics card. Copies share it; it is freed with the last one. A texture is used by
// one Run at a time.
class Texture
{
public:
    Texture() = default;
    bool isNull() const { return !d; }
    QSize size() const;
    Format format() const;
    friend bool operator==(const Texture &a, const Texture &b) { return a.d == b.d; }

private:
    friend class Context;
    friend class Run;
    std::shared_ptr<TextureState> d;
};

// One push-constant field of a kernel, from Slang's reflection.
struct ParamField
{
    QString name;
    int offset = 0;
    int size = 0;
    QString scalar;   // "float32", "int32", "uint32"
    int components = 1;
    QHash<QString, QVariantList> attributes; // Slang user attributes: [Range(-5, 5)] → "Range": {-5, 5}
};

// What a compute entry needs, from Slang's reflection: its group size, its push constants, and the
// storage images it uses (by name, with their binding).
struct KernelInterface
{
    QString entry;
    int groupSize[3] = {1, 1, 1};
    int pushSize = 0;
    QList<ParamField> params;
    QHash<QString, int> bindings;
    QHash<QString, QString> formats; // binding name → "rgba8", "rgba16f"
    QHash<QString, QVariantList> attributes; // the entry's Slang user attributes
    QHash<QString, QString> inputs;          // bindings marked [Input(kind)]: name → kind ("curve", "lut")

    const ParamField *param(const QString &name) const;
};

// Reads slangc's -reflection-json output for one entry; empty entry on errors.
KernelInterface parseReflection(const QByteArray &json, const QString &entry, QString *error = nullptr);

class Kernel
{
public:
    Kernel() = default;
    bool isNull() const { return !d; }
    const KernelInterface &info() const;

private:
    friend class Context;
    friend class Run;
    std::shared_ptr<KernelState> d;
};

// The graphics card Lumen uses. The device is made when it is first needed; without one, everything
// here says why instead of failing later.
class Context
{
public:
    struct Options
    {
        bool disabled = false;   // as if there were no Vulkan (tests, the LUMEN_GPU=off variable)
        bool validation = false; // the validation layer, where the Vulkan SDK provides it
        QString preferredDevice; // part of a device's name; else a discrete GPU first
    };

    // The application's context: options from LUMEN_GPU, LUMEN_GPU_VALIDATE and LUMEN_GPU_DEVICE.
    static Context &instance();

    explicit Context(const Options &options);
    ~Context();
    Context(const Context &) = delete;
    Context &operator=(const Context &) = delete;

    bool isAvailable();
    QString unavailableReason();
    QString deviceName();
    // Errors and warnings the validation layer reported so far (0 without the layer).
    int validationMessages() const;

    // A kernel compiled with Lumen (":/gpu/<shader>.<entry>.spv" and ".json"), or from SPIR-V and its
    // reflection. Null on errors, with the reason in `error`.
    Kernel kernel(const QString &name, QString *error = nullptr);
    Kernel kernel(const QByteArray &spirv, const QByteArray &reflection, const QString &entry, QString *error = nullptr);

    Texture texture(const QSize &size, Format format, QString *error = nullptr);

private:
    friend class Run;
    bool ensureDevice();

    Options m_options;
    QMutex m_lock;
    bool m_tried = false;
    QString m_reason;
    std::shared_ptr<DeviceState> m_device;
    QHash<QString, Kernel> m_kernels;
};

// One batch of work: uploads, dispatches and read-backs, recorded in order, then submit() runs them
// and waits. Barriers between them come from each texture's state. Single use.
class Run
{
public:
    explicit Run(Context &context = Context::instance());
    ~Run();
    Run(const Run &) = delete;
    Run &operator=(const Run &) = delete;

    // An RGBA8 texture with the picture's straight (not premultiplied) colours; a half- or full-float
    // picture (Format_RGBA16FPx4, Format_RGBA32FPx4…) gives an RGBA16F texture with its values.
    Texture upload(const QImage &image);
    // Runs `kernel` over `threads` (usually the picture's size). `textures` binds the kernel's storage
    // images by name; `params` sets its push constants by name: numbers, or lists for vectors.
    void dispatch(const Kernel &kernel, const QHash<QString, Texture> &textures, const QVariantMap &params, const QSize &threads);
    // Asks for a texture's pixels; image(index) has them after submit().
    int readBack(const Texture &texture);

    bool submit();
    QString error() const { return m_error; }
    // RGBA8 → Format_RGBA8888, RGBA16F → Format_RGBA16FPx4.
    QImage image(int index) const;
    // The recorded work's time on the graphics card.
    double gpuMilliseconds() const { return m_gpuMs; }

private:
    struct Recording;
    void fail(const QString &message);

    Context &m_context;
    std::unique_ptr<Recording> m_rec;
    QString m_error;
    QList<QImage> m_images;
    double m_gpuMs = 0;
    bool m_submitted = false;
};

} // namespace lumen::gpu
