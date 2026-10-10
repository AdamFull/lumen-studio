#pragma once

#include <QFuture>
#include <QList>
#include <QString>
#include <QVersionNumber>

namespace lumen::runtime {

struct GpuInfo
{
    QString name;
    quint32 vendorId = 0;     // PCI vendor: 0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel
    quint64 dedicatedVram = 0; // bytes
    bool integrated = false;   // shares system memory (little or no dedicated VRAM)

    bool isNvidia() const { return vendorId == 0x10DE; }
    bool isAmd() const { return vendorId == 0x1002; }
    bool isIntel() const { return vendorId == 0x8086; }
    QString vendor() const;
};

struct HardwareInfo
{
    QList<GpuInfo> gpus;          // hardware adapters, discrete ones first
    QString nvidiaDriver;         // e.g. "581.57" (empty without an NVIDIA driver)
    QVersionNumber cudaVersion;   // highest CUDA version the NVIDIA driver supports
    quint64 systemRam = 0;        // bytes
    int cpuThreads = 0;

    // Largest dedicated VRAM among the GPUs, in bytes.
    quint64 maxVram() const;
    const GpuInfo *primaryGpu() const;
    QString summary() const; // one line for the UI
};

// Probes GPUs (DXGI on Windows), the NVIDIA driver (nvidia-smi) and memory on a worker thread.
QFuture<HardwareInfo> probeHardware();

// Parses `nvidia-smi` output (driver and CUDA version from the banner).
void parseNvidiaSmi(const QString &output, HardwareInfo *info);

QString formatBytes(qint64 bytes);

} // namespace lumen::runtime
