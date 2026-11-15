#include "gpu/Gpu.h"

#include "core/Log.h"
#include "gpu/Vulkan.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <atomic>
#include <cstring>
#include <vector>

namespace lumen::gpu {

namespace {

struct Text
{
    Q_DECLARE_TR_FUNCTIONS(Gpu)
};

VkFormat vkFormat(Format f)
{
    return f == Format::RGBA8 ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R16G16B16A16_SFLOAT;
}

int bytesPerPixel(Format f)
{
    return f == Format::RGBA8 ? 4 : 8;
}

QString formatName(Format f)
{
    return f == Format::RGBA8 ? QStringLiteral("rgba8") : QStringLiteral("rgba16f");
}

QString vkError(VkResult r)
{
    return QStringLiteral("VkResult %1").arg(int(r));
}

// volk loads vulkan-1.dll once per process.
bool loadVulkan()
{
    static const bool loaded = volkInitialize() == VK_SUCCESS;
    return loaded;
}

} // namespace

// ---- Device ----------------------------------------------------------------------------------------

struct DeviceState
{
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VolkDeviceTable vk{};
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VmaAllocator vma = nullptr;
    float timestampPeriod = 1;
    bool timestamps = false;
    QString name;
    QMutex queueLock; // vkQueueSubmit needs the queue to itself
    std::atomic<int> validationMessages{0};

    ~DeviceState()
    {
        if (device) {
            vk.vkDeviceWaitIdle(device);
            if (vma)
                vmaDestroyAllocator(vma);
            vk.vkDestroyDevice(device, nullptr);
        }
        if (messenger)
            vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
        if (instance)
            vkDestroyInstance(instance, nullptr);
    }
};

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL onValidation(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                            const VkDebugUtilsMessengerCallbackDataEXT *data, void *user)
{
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        static_cast<DeviceState *>(user)->validationMessages.fetch_add(1);
        qCWarning(lcGpu) << "Vulkan validation:" << data->pMessage;
    }
    return VK_FALSE;
}

bool hasLayer(const char *name)
{
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const VkLayerProperties &l : layers) {
        if (std::strcmp(l.layerName, name) == 0)
            return true;
    }
    return false;
}

bool supportsStorage(VkPhysicalDevice physical, VkFormat format)
{
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(physical, format, &props);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT
                                      | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    return (props.optimalTilingFeatures & need) == need;
}

std::shared_ptr<DeviceState> createDevice(const Context::Options &options, QString *reason)
{
    if (!loadVulkan()) {
        *reason = Text::tr("Vulkan is not installed on this computer (it comes with the graphics driver).");
        return nullptr;
    }
    uint32_t version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion)
        vkEnumerateInstanceVersion(&version);
    if (version < VK_API_VERSION_1_3) {
        *reason = Text::tr("The Vulkan here is version %1.%2; effects need 1.3. Updating the graphics driver may help.")
                      .arg(VK_API_VERSION_MAJOR(version))
                      .arg(VK_API_VERSION_MINOR(version));
        return nullptr;
    }

    auto d = std::make_shared<DeviceState>();
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    const QByteArray appName = QCoreApplication::applicationName().toUtf8();
    app.pApplicationName = appName.isEmpty() ? "Lumen Studio" : appName.constData();
    app.pEngineName = "lumen_gpu";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    const char *layer = "VK_LAYER_KHRONOS_validation";
    const char *debugUtils = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    const bool validate = options.validation && hasLayer(layer);
    if (options.validation && !validate)
        qCInfo(lcGpu) << "The Vulkan validation layer is not installed; running without it";
    if (validate) {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = &layer;
        ici.enabledExtensionCount = 1;
        ici.ppEnabledExtensionNames = &debugUtils;
    }
    if (VkResult r = vkCreateInstance(&ici, nullptr, &d->instance); r != VK_SUCCESS) {
        d->instance = VK_NULL_HANDLE;
        *reason = Text::tr("Vulkan could not start (%1).").arg(vkError(r));
        return nullptr;
    }
    volkLoadInstanceOnly(d->instance);
    if (validate) {
        VkDebugUtilsMessengerCreateInfoEXT mci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        mci.pfnUserCallback = onValidation;
        mci.pUserData = d.get();
        vkCreateDebugUtilsMessengerEXT(d->instance, &mci, nullptr, &d->messenger);
    }

    // The graphics card: one that has Vulkan 1.3 compute and storage images in the formats effects
    // use; the named one first, else a discrete GPU before an integrated one, a software one last.
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(d->instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(d->instance, &count, devices.data());
    int bestScore = -1;
    QStringList refused;
    for (VkPhysicalDevice physical : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical, &props);
        const QString name = QString::fromUtf8(props.deviceName);
        VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &v13;
        vkGetPhysicalDeviceFeatures2(physical, &features);
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
        int family = -1;
        for (uint32_t i = 0; i < familyCount && family < 0; ++i) {
            if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
                family = int(i);
        }
        if (props.apiVersion < VK_API_VERSION_1_3 || !v13.synchronization2 || family < 0
            || !supportsStorage(physical, VK_FORMAT_R8G8B8A8_UNORM) || !supportsStorage(physical, VK_FORMAT_R16G16B16A16_SFLOAT)) {
            refused << name;
            continue;
        }
        int score = 0;
        switch (props.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: score = 40; break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score = 30; break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: score = 20; break;
        default: score = 10; break;
        }
        if (!options.preferredDevice.isEmpty() && name.contains(options.preferredDevice, Qt::CaseInsensitive))
            score += 100;
        if (score > bestScore) {
            bestScore = score;
            d->physical = physical;
            d->family = uint32_t(family);
            d->name = name;
            d->timestampPeriod = props.limits.timestampPeriod;
            d->timestamps = families[size_t(family)].timestampValidBits > 0 && props.limits.timestampComputeAndGraphics;
        }
    }
    if (!d->physical) {
        *reason = refused.isEmpty() ? Text::tr("No graphics card with Vulkan was found.")
                                    : Text::tr("The graphics card (%1) cannot run effects: they need Vulkan 1.3. Updating its driver may help.")
                                          .arg(refused.join(QStringLiteral(", ")));
        return nullptr;
    }

    const float priority = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = d->family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    v13.synchronization2 = VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &v13;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    if (VkResult r = vkCreateDevice(d->physical, &dci, nullptr, &d->device); r != VK_SUCCESS) {
        d->device = VK_NULL_HANDLE;
        *reason = Text::tr("The graphics card %1 could not be opened (%2).").arg(d->name, vkError(r));
        return nullptr;
    }
    // Each device keeps its own function table, so two devices in one process cannot mix them up.
    volkLoadDeviceTable(&d->vk, d->device);
    d->vk.vkGetDeviceQueue(d->device, d->family, 0, &d->queue);

    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo aci{};
    aci.physicalDevice = d->physical;
    aci.device = d->device;
    aci.instance = d->instance;
    aci.vulkanApiVersion = VK_API_VERSION_1_3;
    aci.pVulkanFunctions = &functions;
    if (VkResult r = vmaCreateAllocator(&aci, &d->vma); r != VK_SUCCESS) {
        d->vma = nullptr;
        *reason = Text::tr("The graphics card %1 could not be opened (%2).").arg(d->name, vkError(r));
        return nullptr;
    }
    qCInfo(lcGpu) << "Effects run on" << d->name << (validate ? "with the validation layer" : "");
    return d;
}

} // namespace

// ---- Textures and kernels ------------------------------------------------------------------------

struct TextureState
{
    std::shared_ptr<DeviceState> dev;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    QSize size;
    Format format = Format::RGBA8;
    // What the last command did with it: barriers start from here.
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;

    ~TextureState()
    {
        if (view)
            dev->vk.vkDestroyImageView(dev->device, view, nullptr);
        if (image)
            vmaDestroyImage(dev->vma, image, alloc);
    }
};

QSize Texture::size() const
{
    return d ? d->size : QSize();
}

Format Texture::format() const
{
    return d ? d->format : Format::RGBA8;
}

struct KernelState
{
    std::shared_ptr<DeviceState> dev;
    KernelInterface iface;
    QList<int> bindingSlots; // the binding indexes, ascending
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    ~KernelState()
    {
        if (pipeline)
            dev->vk.vkDestroyPipeline(dev->device, pipeline, nullptr);
        if (layout)
            dev->vk.vkDestroyPipelineLayout(dev->device, layout, nullptr);
        if (setLayout)
            dev->vk.vkDestroyDescriptorSetLayout(dev->device, setLayout, nullptr);
    }
};

const KernelInterface &Kernel::info() const
{
    static const KernelInterface none;
    return d ? d->iface : none;
}

const ParamField *KernelInterface::param(const QString &name) const
{
    for (const ParamField &f : params) {
        if (f.name == name)
            return &f;
    }
    return nullptr;
}

KernelInterface parseReflection(const QByteArray &json, const QString &entry, QString *error)
{
    KernelInterface k;
    auto failed = [&](const QString &message) {
        if (error)
            *error = message;
        return KernelInterface();
    };
    QJsonParseError pe;
    const QJsonObject root = QJsonDocument::fromJson(json, &pe).object();
    if (pe.error != QJsonParseError::NoError)
        return failed(pe.errorString());

    QJsonObject entryPoint;
    for (const QJsonValue &v : root.value(QStringLiteral("entryPoints")).toArray()) {
        if (v.toObject().value(QStringLiteral("name")).toString() == entry)
            entryPoint = v.toObject();
    }
    if (entryPoint.isEmpty())
        return failed(QStringLiteral("no entry point %1").arg(entry));
    if (entryPoint.value(QStringLiteral("stage")).toString() != QLatin1String("compute"))
        return failed(QStringLiteral("%1 is not a compute entry").arg(entry));
    k.entry = entry;
    const QJsonArray group = entryPoint.value(QStringLiteral("threadGroupSize")).toArray();
    for (int i = 0; i < 3; ++i)
        k.groupSize[i] = std::max(1, group.at(i).toInt(1));

    for (const QJsonValue &v : entryPoint.value(QStringLiteral("bindings")).toArray()) {
        const QJsonObject b = v.toObject();
        const QJsonObject binding = b.value(QStringLiteral("binding")).toObject();
        if (binding.value(QStringLiteral("kind")).toString() != QLatin1String("descriptorTableSlot"))
            continue;
        if (!binding.value(QStringLiteral("used")).toInt(1))
            continue;
        const QString name = b.value(QStringLiteral("name")).toString();
        k.bindings.insert(name, binding.value(QStringLiteral("index")).toInt());
        if (b.contains(QStringLiteral("format")))
            k.formats.insert(name, b.value(QStringLiteral("format")).toString());
    }

    // Push constants: the one parameter bound as a push-constant buffer, a struct of scalars and
    // vectors.
    for (const QJsonValue &v : root.value(QStringLiteral("parameters")).toArray()) {
        const QJsonObject p = v.toObject();
        if (p.value(QStringLiteral("binding")).toObject().value(QStringLiteral("kind")).toString() != QLatin1String("pushConstantBuffer"))
            continue;
        const QJsonObject element = p.value(QStringLiteral("type")).toObject().value(QStringLiteral("elementType")).toObject();
        for (const QJsonValue &fv : element.value(QStringLiteral("fields")).toArray()) {
            const QJsonObject f = fv.toObject();
            const QJsonObject type = f.value(QStringLiteral("type")).toObject();
            const QJsonObject binding = f.value(QStringLiteral("binding")).toObject();
            ParamField field;
            field.name = f.value(QStringLiteral("name")).toString();
            field.offset = binding.value(QStringLiteral("offset")).toInt();
            field.size = binding.value(QStringLiteral("size")).toInt();
            const QString kind = type.value(QStringLiteral("kind")).toString();
            if (kind == QLatin1String("scalar")) {
                field.scalar = type.value(QStringLiteral("scalarType")).toString();
            } else if (kind == QLatin1String("vector")) {
                field.components = type.value(QStringLiteral("elementCount")).toInt(1);
                field.scalar = type.value(QStringLiteral("elementType")).toObject().value(QStringLiteral("scalarType")).toString();
            } else {
                return failed(QStringLiteral("the parameter %1 is a %2; effects take numbers and vectors").arg(field.name, kind));
            }
            if (field.scalar != QLatin1String("float32") && field.scalar != QLatin1String("int32") && field.scalar != QLatin1String("uint32"))
                return failed(QStringLiteral("the parameter %1 is %2; effects take float, int and uint").arg(field.name, field.scalar));
            k.pushSize = std::max(k.pushSize, field.offset + field.size);
            k.params << field;
        }
    }
    k.pushSize = (k.pushSize + 3) / 4 * 4;
    return k;
}

// ---- Context ---------------------------------------------------------------------------------------

Context &Context::instance()
{
    static Context context([] {
        Options o;
        o.disabled = qEnvironmentVariable("LUMEN_GPU").compare(QLatin1String("off"), Qt::CaseInsensitive) == 0;
        o.validation = qEnvironmentVariableIntValue("LUMEN_GPU_VALIDATE") != 0;
        o.preferredDevice = qEnvironmentVariable("LUMEN_GPU_DEVICE");
        return o;
    }());
    return context;
}

Context::Context(const Options &options)
    : m_options(options)
{
}

Context::~Context()
{
    m_kernels.clear();
    m_device.reset();
}

bool Context::ensureDevice()
{
    QMutexLocker lock(&m_lock);
    if (!m_tried) {
        m_tried = true;
        if (m_options.disabled)
            m_reason = Text::tr("Effects on the graphics card are turned off.");
        else
            m_device = createDevice(m_options, &m_reason);
        if (!m_device)
            qCInfo(lcGpu) << "No GPU effects:" << m_reason;
    }
    return m_device != nullptr;
}

bool Context::isAvailable()
{
    return ensureDevice();
}

QString Context::unavailableReason()
{
    ensureDevice();
    return m_reason;
}

QString Context::deviceName()
{
    return ensureDevice() ? m_device->name : QString();
}

int Context::validationMessages() const
{
    return m_device ? m_device->validationMessages.load() : 0;
}

Kernel Context::kernel(const QString &name, QString *error)
{
    {
        QMutexLocker lock(&m_lock);
        if (const auto it = m_kernels.constFind(name); it != m_kernels.cend())
            return *it;
    }
    QFile spv(QStringLiteral(":/gpu/%1.spv").arg(name));
    QFile json(QStringLiteral(":/gpu/%1.json").arg(name));
    if (!spv.open(QIODevice::ReadOnly) || !json.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("no kernel %1 in Lumen").arg(name);
        return {};
    }
    Kernel k = kernel(spv.readAll(), json.readAll(), name.section(u'.', -1), error);
    if (!k.isNull()) {
        QMutexLocker lock(&m_lock);
        m_kernels.insert(name, k);
    }
    return k;
}

Kernel Context::kernel(const QByteArray &spirv, const QByteArray &reflection, const QString &entry, QString *error)
{
    auto failed = [error](const QString &message) {
        if (error)
            *error = message;
        return Kernel();
    };
    if (!ensureDevice())
        return failed(m_reason);
    if (spirv.isEmpty() || spirv.size() % 4 != 0)
        return failed(QStringLiteral("not SPIR-V"));
    QString why;
    KernelInterface iface = parseReflection(reflection, entry, &why);
    if (iface.entry.isEmpty())
        return failed(why);
    DeviceState &dev = *m_device;

    auto state = std::make_shared<KernelState>();
    state->dev = m_device;
    state->iface = iface;
    for (int slot : iface.bindings) {
        if (!state->bindingSlots.contains(slot))
            state->bindingSlots << slot;
    }
    std::sort(state->bindingSlots.begin(), state->bindingSlots.end());

    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (int slot : std::as_const(state->bindingSlots))
        bindings.push_back({uint32_t(slot), VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = uint32_t(bindings.size());
    lci.pBindings = bindings.data();
    if (VkResult r = dev.vk.vkCreateDescriptorSetLayout(dev.device, &lci, nullptr, &state->setLayout); r != VK_SUCCESS)
        return failed(vkError(r));
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, uint32_t(iface.pushSize)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &state->setLayout;
    plci.pushConstantRangeCount = iface.pushSize > 0 ? 1 : 0;
    plci.pPushConstantRanges = &push;
    if (VkResult r = dev.vk.vkCreatePipelineLayout(dev.device, &plci, nullptr, &state->layout); r != VK_SUCCESS)
        return failed(vkError(r));

    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = size_t(spirv.size());
    smci.pCode = reinterpret_cast<const uint32_t *>(spirv.constData());
    VkShaderModule module = VK_NULL_HANDLE;
    if (VkResult r = dev.vk.vkCreateShaderModule(dev.device, &smci, nullptr, &module); r != VK_SUCCESS)
        return failed(vkError(r));
    const QByteArray entryName = entry.toUtf8();
    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, entryName.constData(), nullptr};
    cpci.layout = state->layout;
    const VkResult r = dev.vk.vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &cpci, nullptr, &state->pipeline);
    dev.vk.vkDestroyShaderModule(dev.device, module, nullptr);
    if (r != VK_SUCCESS)
        return failed(vkError(r));
    Kernel k;
    k.d = std::move(state);
    return k;
}

Texture Context::texture(const QSize &size, Format format, QString *error)
{
    auto failed = [error](const QString &message) {
        if (error)
            *error = message;
        return Texture();
    };
    if (!ensureDevice())
        return failed(m_reason);
    if (size.isEmpty())
        return failed(QStringLiteral("an empty texture"));
    auto state = std::make_shared<TextureState>();
    state->dev = m_device;
    state->size = size;
    state->format = format;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = vkFormat(format);
    ici.extent = {uint32_t(size.width()), uint32_t(size.height()), 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (VkResult r = vmaCreateImage(m_device->vma, &ici, &aci, &state->image, &state->alloc, nullptr); r != VK_SUCCESS) {
        state->image = VK_NULL_HANDLE;
        return failed(r == VK_ERROR_OUT_OF_DEVICE_MEMORY ? Text::tr("The graphics card has no memory left for a %1 × %2 picture.")
                                                               .arg(size.width())
                                                               .arg(size.height())
                                                         : vkError(r));
    }
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = state->image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = ici.format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (VkResult r = m_device->vk.vkCreateImageView(m_device->device, &vci, nullptr, &state->view); r != VK_SUCCESS)
        return failed(vkError(r));
    Texture t;
    t.d = std::move(state);
    return t;
}

// ---- Run -------------------------------------------------------------------------------------------

namespace {

struct HostBuffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    void *mapped = nullptr;
};

} // namespace

struct Run::Recording
{
    std::shared_ptr<DeviceState> dev;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> descriptorPools;
    std::vector<HostBuffer> staging;
    struct ReadBack
    {
        HostBuffer host;
        QSize size;
        Format format;
    };
    std::vector<ReadBack> readBacks;
    QList<Texture> textures; // alive until the work is done
    QList<Kernel> kernels;

    ~Recording()
    {
        DeviceState &d = *dev;
        if (fence)
            d.vk.vkDestroyFence(d.device, fence, nullptr);
        if (queries)
            d.vk.vkDestroyQueryPool(d.device, queries, nullptr);
        if (pool)
            d.vk.vkDestroyCommandPool(d.device, pool, nullptr); // frees the command buffer
        for (VkDescriptorPool p : descriptorPools)
            d.vk.vkDestroyDescriptorPool(d.device, p, nullptr);
        for (HostBuffer &b : staging)
            vmaDestroyBuffer(d.vma, b.buffer, b.alloc);
        for (ReadBack &r : readBacks)
            vmaDestroyBuffer(d.vma, r.host.buffer, r.host.alloc);
    }

    // Moves a texture from what its last command did to what the next one does.
    void transition(TextureState &t, VkImageLayout layout, VkPipelineStageFlags2 stage, VkAccessFlags2 access)
    {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = t.stage;
        b.srcAccessMask = t.access;
        b.dstStageMask = stage;
        b.dstAccessMask = access;
        b.oldLayout = t.layout;
        b.newLayout = layout;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = t.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        dev->vk.vkCmdPipelineBarrier2(cmd, &dep);
        t.layout = layout;
        t.stage = stage;
        t.access = access;
    }

    bool hostBuffer(VkDeviceSize bytes, bool readBack, HostBuffer &out)
    {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = bytes;
        bci.usage = readBack ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT
                    | (readBack ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
        VmaAllocationInfo info;
        if (vmaCreateBuffer(dev->vma, &bci, &aci, &out.buffer, &out.alloc, &info) != VK_SUCCESS)
            return false;
        out.mapped = info.pMappedData;
        return true;
    }

    VkDescriptorSet descriptorSet(VkDescriptorSetLayout layout)
    {
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (!descriptorPools.empty()) {
            dsai.descriptorPool = descriptorPools.back();
            if (dev->vk.vkAllocateDescriptorSets(dev->device, &dsai, &set) == VK_SUCCESS)
                return set;
        }
        // The pool is full (or there is none yet): another one.
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256};
        VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpci.maxSets = 64;
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes = &size;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (dev->vk.vkCreateDescriptorPool(dev->device, &dpci, nullptr, &pool) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        descriptorPools.push_back(pool);
        dsai.descriptorPool = pool;
        return dev->vk.vkAllocateDescriptorSets(dev->device, &dsai, &set) == VK_SUCCESS ? set : VK_NULL_HANDLE;
    }
};

Run::Run(Context &context)
    : m_context(context)
{
    if (!m_context.ensureDevice()) {
        m_error = m_context.m_reason;
        return;
    }
    m_rec = std::make_unique<Recording>();
    Recording &rec = *m_rec;
    rec.dev = m_context.m_device;
    DeviceState &d = *rec.dev;
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = d.family;
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (VkResult r = d.vk.vkCreateCommandPool(d.device, &pci, nullptr, &rec.pool); r != VK_SUCCESS) {
        fail(vkError(r));
        return;
    }
    cbai.commandPool = rec.pool;
    if (VkResult r = d.vk.vkAllocateCommandBuffers(d.device, &cbai, &rec.cmd); r != VK_SUCCESS) {
        fail(vkError(r));
        return;
    }
    if (VkResult r = d.vk.vkCreateFence(d.device, &fci, nullptr, &rec.fence); r != VK_SUCCESS) {
        fail(vkError(r));
        return;
    }
    d.vk.vkBeginCommandBuffer(rec.cmd, &begin);
    if (d.timestamps) {
        VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpci.queryCount = 2;
        if (d.vk.vkCreateQueryPool(d.device, &qpci, nullptr, &rec.queries) == VK_SUCCESS) {
            d.vk.vkCmdResetQueryPool(rec.cmd, rec.queries, 0, 2);
            d.vk.vkCmdWriteTimestamp2(rec.cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, rec.queries, 0);
        }
    }
}

Run::~Run() = default;

void Run::fail(const QString &message)
{
    if (m_error.isEmpty())
        m_error = message;
    qCWarning(lcGpu) << "GPU work failed:" << message;
}

Texture Run::upload(const QImage &image)
{
    if (!m_rec || !m_error.isEmpty())
        return {};
    if (m_submitted) {
        fail(QStringLiteral("the run was submitted already"));
        return {};
    }
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    QString why;
    Texture t = m_context.texture(rgba.size(), Format::RGBA8, &why);
    if (t.isNull()) {
        fail(why);
        return {};
    }
    Recording &rec = *m_rec;
    const int w = rgba.width();
    const int h = rgba.height();
    HostBuffer staging;
    if (!rec.hostBuffer(VkDeviceSize(w) * h * 4, false, staging)) {
        fail(Text::tr("Not enough memory to send a %1 × %2 picture to the graphics card.").arg(w).arg(h));
        return {};
    }
    rec.staging.push_back(staging);
    for (int y = 0; y < h; ++y)
        std::memcpy(static_cast<char *>(staging.mapped) + qsizetype(y) * w * 4, rgba.constScanLine(y), size_t(w) * 4);
    vmaFlushAllocation(rec.dev->vma, staging.alloc, 0, VK_WHOLE_SIZE);

    TextureState &ts = *t.d;
    rec.transition(ts, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {uint32_t(w), uint32_t(h), 1};
    rec.dev->vk.vkCmdCopyBufferToImage(rec.cmd, staging.buffer, ts.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    rec.textures << t;
    return t;
}

void Run::dispatch(const Kernel &kernel, const QHash<QString, Texture> &textures, const QVariantMap &params, const QSize &threads)
{
    if (!m_rec || !m_error.isEmpty())
        return;
    if (m_submitted)
        return fail(QStringLiteral("the run was submitted already"));
    if (kernel.isNull())
        return fail(QStringLiteral("no kernel"));
    Recording &rec = *m_rec;
    const KernelState &k = *kernel.d;
    if (k.dev != rec.dev)
        return fail(QStringLiteral("the kernel %1 belongs to another device").arg(k.iface.entry));

    // Each binding the kernel uses needs a texture of its format.
    QHash<int, TextureState *> bound;
    for (auto it = k.iface.bindings.cbegin(); it != k.iface.bindings.cend(); ++it) {
        const Texture t = textures.value(it.key());
        if (t.isNull())
            continue;
        if (t.d->dev != rec.dev)
            return fail(QStringLiteral("the texture %1 belongs to another device").arg(it.key()));
        const QString wanted = k.iface.formats.value(it.key());
        if (!wanted.isEmpty() && wanted != formatName(t.d->format))
            return fail(QStringLiteral("%1 of %2 takes %3, not %4").arg(it.key(), k.iface.entry, wanted, formatName(t.d->format)));
        bound.insert(it.value(), t.d.get());
        rec.textures << t;
    }
    for (int slot : k.bindingSlots) {
        if (!bound.contains(slot))
            return fail(QStringLiteral("%1 needs a texture for %2").arg(k.iface.entry, k.iface.bindings.key(slot)));
    }

    // Push constants by name; "size" is the threads' size unless given.
    QByteArray push(k.iface.pushSize, '\0');
    QVariantMap values = params;
    if (k.iface.param(QStringLiteral("size")) && !values.contains(QStringLiteral("size")))
        values.insert(QStringLiteral("size"), QVariantList{threads.width(), threads.height()});
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        const ParamField *field = k.iface.param(it.key());
        if (!field)
            return fail(QStringLiteral("%1 has no parameter %2").arg(k.iface.entry, it.key()));
        const QVariantList list = it.value().typeId() == QMetaType::QVariantList ? it.value().toList() : QVariantList{it.value()};
        if (list.size() != field->components)
            return fail(QStringLiteral("%1 of %2 takes %3 value(s)").arg(it.key(), k.iface.entry).arg(field->components));
        for (int i = 0; i < field->components; ++i) {
            char *at = push.data() + field->offset + i * 4;
            if (field->scalar == QLatin1String("float32")) {
                const float f = list.at(i).toFloat();
                std::memcpy(at, &f, 4);
            } else if (field->scalar == QLatin1String("int32")) {
                const qint32 v = list.at(i).toInt();
                std::memcpy(at, &v, 4);
            } else {
                const quint32 v = list.at(i).toUInt();
                std::memcpy(at, &v, 4);
            }
        }
    }

    DeviceState &d = *rec.dev;
    VkDescriptorSet set = rec.descriptorSet(k.setLayout);
    if (!set)
        return fail(QStringLiteral("no descriptor set for %1").arg(k.iface.entry));
    std::vector<VkDescriptorImageInfo> infos;
    infos.reserve(size_t(k.bindingSlots.size()));
    std::vector<VkWriteDescriptorSet> writes;
    for (int slot : k.bindingSlots) {
        TextureState *t = bound.value(slot);
        // Read and written as far as the barriers know: the reflection does not say which.
        rec.transition(*t, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        infos.push_back({VK_NULL_HANDLE, t->view, VK_IMAGE_LAYOUT_GENERAL});
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = uint32_t(slot);
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w.pImageInfo = &infos.back();
        writes.push_back(w);
    }
    d.vk.vkUpdateDescriptorSets(d.device, uint32_t(writes.size()), writes.data(), 0, nullptr);
    d.vk.vkCmdBindPipeline(rec.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.pipeline);
    d.vk.vkCmdBindDescriptorSets(rec.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.layout, 0, 1, &set, 0, nullptr);
    if (k.iface.pushSize > 0)
        d.vk.vkCmdPushConstants(rec.cmd, k.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, uint32_t(push.size()), push.constData());
    const uint32_t gx = uint32_t((threads.width() + k.iface.groupSize[0] - 1) / k.iface.groupSize[0]);
    const uint32_t gy = uint32_t((threads.height() + k.iface.groupSize[1] - 1) / k.iface.groupSize[1]);
    d.vk.vkCmdDispatch(rec.cmd, std::max(1u, gx), std::max(1u, gy), 1);
    rec.kernels << kernel;
}

int Run::readBack(const Texture &texture)
{
    if (!m_rec || !m_error.isEmpty())
        return -1;
    if (m_submitted) {
        fail(QStringLiteral("the run was submitted already"));
        return -1;
    }
    if (texture.isNull() || texture.d->dev != m_rec->dev) {
        fail(QStringLiteral("no texture to read back"));
        return -1;
    }
    Recording &rec = *m_rec;
    TextureState &t = *texture.d;
    Recording::ReadBack rb;
    rb.size = t.size;
    rb.format = t.format;
    if (!rec.hostBuffer(VkDeviceSize(t.size.width()) * t.size.height() * bytesPerPixel(t.format), true, rb.host)) {
        fail(Text::tr("Not enough memory for a %1 × %2 picture from the graphics card.").arg(t.size.width()).arg(t.size.height()));
        return -1;
    }
    rec.readBacks.push_back(rb);
    rec.transition(t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {uint32_t(t.size.width()), uint32_t(t.size.height()), 1};
    rec.dev->vk.vkCmdCopyImageToBuffer(rec.cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.host.buffer, 1, &region);
    rec.textures << texture;
    return int(rec.readBacks.size()) - 1;
}

bool Run::submit()
{
    if (!m_rec || !m_error.isEmpty())
        return false;
    if (m_submitted) {
        fail(QStringLiteral("the run was submitted already"));
        return false;
    }
    m_submitted = true;
    Recording &rec = *m_rec;
    DeviceState &d = *rec.dev;
    // What the copies wrote is for the CPU to read.
    VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    host.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    host.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &host;
    d.vk.vkCmdPipelineBarrier2(rec.cmd, &dep);
    if (rec.queries)
        d.vk.vkCmdWriteTimestamp2(rec.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, rec.queries, 1);
    if (VkResult r = d.vk.vkEndCommandBuffer(rec.cmd); r != VK_SUCCESS) {
        fail(vkError(r));
        return false;
    }
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &rec.cmd;
    {
        QMutexLocker lock(&d.queueLock);
        if (VkResult r = d.vk.vkQueueSubmit(d.queue, 1, &si, rec.fence); r != VK_SUCCESS) {
            fail(vkError(r));
            return false;
        }
    }
    const VkResult waited = d.vk.vkWaitForFences(d.device, 1, &rec.fence, VK_TRUE, 120ull * 1000 * 1000 * 1000);
    if (waited != VK_SUCCESS) {
        fail(waited == VK_TIMEOUT ? Text::tr("The graphics card did not finish within two minutes.")
                                  : Text::tr("The graphics card stopped working (%1); its driver may have been reset.").arg(vkError(waited)));
        return false;
    }
    if (rec.queries) {
        uint64_t stamps[2] = {};
        if (d.vk.vkGetQueryPoolResults(d.device, rec.queries, 0, 2, sizeof stamps, stamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT)
            == VK_SUCCESS)
            m_gpuMs = double(stamps[1] - stamps[0]) * d.timestampPeriod / 1e6;
    }
    for (const Recording::ReadBack &rb : rec.readBacks) {
        vmaInvalidateAllocation(d.vma, rb.host.alloc, 0, VK_WHOLE_SIZE);
        QImage image(rb.size, rb.format == Format::RGBA8 ? QImage::Format_RGBA8888 : QImage::Format_RGBA16FPx4);
        const qsizetype row = qsizetype(rb.size.width()) * bytesPerPixel(rb.format);
        for (int y = 0; y < rb.size.height(); ++y)
            std::memcpy(image.scanLine(y), static_cast<const char *>(rb.host.mapped) + row * y, size_t(row));
        m_images << image;
    }
    m_rec.reset();
    return true;
}

QImage Run::image(int index) const
{
    return m_images.value(index);
}

} // namespace lumen::gpu
