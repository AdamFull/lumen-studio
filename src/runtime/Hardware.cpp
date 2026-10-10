#include "runtime/Hardware.h"

#include "core/Async.h"

#include <QCoreApplication>
#include <QProcess>
#include <QRegularExpression>
#include <QThread>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dxgi.h>
#endif

#include <algorithm>

namespace lumen::runtime {

QString GpuInfo::vendor() const
{
    switch (vendorId) {
    case 0x10DE:
        return QStringLiteral("NVIDIA");
    case 0x1002:
        return QStringLiteral("AMD");
    case 0x8086:
        return QStringLiteral("Intel");
    default:
        return QStringLiteral("0x%1").arg(vendorId, 4, 16, QLatin1Char('0'));
    }
}

quint64 HardwareInfo::maxVram() const
{
    quint64 best = 0;
    for (const GpuInfo &g : gpus)
        best = std::max(best, g.dedicatedVram);
    return best;
}

const GpuInfo *HardwareInfo::primaryGpu() const
{
    return gpus.isEmpty() ? nullptr : &gpus.first();
}

QString formatBytes(qint64 bytes)
{
    const double b = double(bytes);
    if (b >= 1024.0 * 1024 * 1024)
        return QStringLiteral("%1 GB").arg(b / (1024.0 * 1024 * 1024), 0, 'f', 1);
    if (b >= 1024.0 * 1024)
        return QStringLiteral("%1 MB").arg(b / (1024.0 * 1024), 0, 'f', 0);
    if (b >= 1024.0)
        return QStringLiteral("%1 KB").arg(b / 1024.0, 0, 'f', 0);
    return QStringLiteral("%1 B").arg(bytes);
}

QString HardwareInfo::summary() const
{
    QStringList parts;
    for (const GpuInfo &g : gpus) {
        QString s = g.name;
        if (!g.integrated)
            s += QStringLiteral(" (%1)").arg(formatBytes(qint64(g.dedicatedVram)));
        parts << s;
    }
    if (parts.isEmpty())
        parts << QCoreApplication::translate("lumen::runtime::Hardware", "No GPU found");
    QString out = parts.join(QStringLiteral(" · "));
    if (!cudaVersion.isNull())
        out += QStringLiteral(" · CUDA %1").arg(cudaVersion.toString());
    if (systemRam)
        out += QCoreApplication::translate("lumen::runtime::Hardware", " · %1 RAM").arg(formatBytes(qint64(systemRam)));
    return out;
}

void parseNvidiaSmi(const QString &output, HardwareInfo *info)
{
    // "Driver Version: 581.57 … CUDA Version: 13.0" (older drivers) or
    // "KMD Version: 610.62 … CUDA UMD Version: 13.3" (newer ones).
    static const QRegularExpression driver(QStringLiteral(R"((?:Driver|KMD) Version:\s*([0-9.]+))"));
    static const QRegularExpression cuda(QStringLiteral(R"(CUDA (?:UMD )?Version:\s*([0-9.]+))"));
    if (auto m = driver.match(output); m.hasMatch())
        info->nvidiaDriver = m.captured(1);
    if (auto m = cuda.match(output); m.hasMatch())
        info->cudaVersion = QVersionNumber::fromString(m.captured(1));
}

namespace {

HardwareInfo probeBlocking()
{
    HardwareInfo info;
    info.cpuThreads = QThread::idealThreadCount();
#ifdef Q_OS_WIN
    MEMORYSTATUSEX mem;
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem))
        info.systemRam = mem.ullTotalPhys;

    IDXGIFactory1 *factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory)))) {
        IDXGIAdapter1 *adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc;
            if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                GpuInfo g;
                g.name = QString::fromWCharArray(desc.Description);
                g.vendorId = desc.VendorId;
                g.dedicatedVram = desc.DedicatedVideoMemory;
                g.integrated = desc.DedicatedVideoMemory < 512ull * 1024 * 1024;
                bool duplicate = false;
                for (const GpuInfo &other : std::as_const(info.gpus))
                    duplicate |= other.name == g.name && other.dedicatedVram == g.dedicatedVram;
                if (!duplicate)
                    info.gpus << g;
            }
            adapter->Release();
        }
        factory->Release();
    }
#endif
    std::stable_sort(info.gpus.begin(), info.gpus.end(), [](const GpuInfo &a, const GpuInfo &b) {
        if (a.integrated != b.integrated)
            return !a.integrated;
        return a.dedicatedVram > b.dedicatedVram;
    });
    const bool nvidia = std::any_of(info.gpus.begin(), info.gpus.end(), [](const GpuInfo &g) { return g.isNvidia(); });
#ifdef Q_OS_WIN
    const bool askDriver = nvidia || info.gpus.isEmpty();
#else
    const bool askDriver = true;
    Q_UNUSED(nvidia)
#endif
    if (askDriver) {
        QProcess smi;
        smi.start(QStringLiteral("nvidia-smi"), {});
        if (smi.waitForStarted(3000) && smi.waitForFinished(8000))
            parseNvidiaSmi(QString::fromLocal8Bit(smi.readAllStandardOutput()), &info);
        else
            smi.kill();
    }
    return info;
}

} // namespace

QFuture<HardwareInfo> probeHardware()
{
    return async::run([] { return probeBlocking(); });
}

} // namespace lumen::runtime
