#include "MockHttpServer.h"

#include "core/Job.h"
#include "core/JobQueue.h"
#include "net/HttpClient.h"
#include "runtime/Catalog.h"
#include "runtime/Download.h"
#include "runtime/Hardware.h"
#include "runtime/RuntimeStore.h"
#include "runtime/ServerProfile.h"
#include "runtime/Supervisor.h"
#include "runtime/Zip.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <zlib.h>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

using namespace lumen;
using namespace lumen::runtime;

namespace {

struct ZipItem
{
    QString name;
    QByteArray data;
    bool deflate = true;
    bool symlink = false;
};

void put16(QByteArray &b, quint16 v)
{
    b.append(char(v & 0xFF)).append(char(v >> 8));
}

void put32(QByteArray &b, quint32 v)
{
    put16(b, quint16(v & 0xFFFF));
    put16(b, quint16(v >> 16));
}

QByteArray rawDeflate(const QByteArray &in)
{
    z_stream zs = {};
    deflateInit2(&zs, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    QByteArray out(int(deflateBound(&zs, uLong(in.size()))), Qt::Uninitialized);
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.constData()));
    zs.avail_in = uInt(in.size());
    zs.next_out = reinterpret_cast<Bytef *>(out.data());
    zs.avail_out = uInt(out.size());
    deflate(&zs, Z_FINISH);
    out.resize(int(zs.total_out));
    deflateEnd(&zs);
    return out;
}

// Minimal zip writer, so tests can also build archives no real tool would produce.
QByteArray makeZip(const QList<ZipItem> &items)
{
    QByteArray file;
    QByteArray central;
    for (const ZipItem &it : items) {
        const QByteArray name = it.name.toUtf8();
        const QByteArray data = it.deflate ? rawDeflate(it.data) : it.data;
        const quint32 crc = quint32(crc32(0, reinterpret_cast<const Bytef *>(it.data.constData()), uInt(it.data.size())));
        const quint32 offset = quint32(file.size());
        put32(file, 0x04034b50);
        put16(file, 20);
        put16(file, 0x0800);
        put16(file, it.deflate ? 8 : 0);
        put16(file, 0);
        put16(file, 0x21);
        put32(file, crc);
        put32(file, quint32(data.size()));
        put32(file, quint32(it.data.size()));
        put16(file, quint16(name.size()));
        put16(file, 0);
        file += name + data;
        put32(central, 0x02014b50);
        put16(central, it.symlink ? quint16(0x0314) : quint16(20));
        put16(central, 20);
        put16(central, 0x0800);
        put16(central, it.deflate ? 8 : 0);
        put16(central, 0);
        put16(central, 0x21);
        put32(central, crc);
        put32(central, quint32(data.size()));
        put32(central, quint32(it.data.size()));
        put16(central, quint16(name.size()));
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put32(central, it.symlink ? quint32(0xA1FF) << 16 : 0);
        put32(central, offset);
        central += name;
    }
    const quint32 cdOffset = quint32(file.size());
    file += central;
    put32(file, 0x06054b50);
    put16(file, 0);
    put16(file, 0);
    put16(file, quint16(items.size()));
    put16(file, quint16(items.size()));
    put32(file, quint32(central.size()));
    put32(file, cdOffset);
    put16(file, 0);
    return file;
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

QString sha256(const QByteArray &data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

QByteArray readAll(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// Serves `data` at a path with HTTP Range support.
void serveFile(MockHttpServer &server, const QString &path, const QByteArray &data, int *requests = nullptr,
               QByteArray *lastRange = nullptr, bool honourRange = true)
{
    server.route("GET", path, [data, requests, lastRange, honourRange](const MockRequest &req) {
        if (requests)
            ++*requests;
        MockResponse r;
        r.contentType = "application/octet-stream";
        const QByteArray range = req.headers.value("range");
        if (lastRange)
            *lastRange = range;
        if (honourRange && range.startsWith("bytes=")) {
            const qint64 from = range.mid(6).split('-').value(0).toLongLong();
            if (from >= data.size()) {
                r.status = 416;
                return r;
            }
            r.status = 206;
            r.body = data.mid(from);
            r.headers.append({"Content-Range", "bytes " + QByteArray::number(from) + '-' + QByteArray::number(data.size() - 1) + '/'
                                                   + QByteArray::number(data.size())});
            return r;
        }
        r.body = data;
        return r;
    });
}

QByteArray fakeServerBinary()
{
    return readAll(QStringLiteral(FAKE_SERVER_PATH));
}

#ifdef Q_OS_WIN
bool processAlive(qint64 pid)
{
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!h)
        return false;
    const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}
#endif

} // namespace

class TestRuntime : public QObject
{
    Q_OBJECT

private slots:
    void zipSafePaths();
    void zipExtract();
    void zipRejectsUnsafeArchives();
    void downloadVerifiesAndResumes();
    void downloadRejectsTampering();
    void catalogAndBackendChoice();
    void hardwareProbe();
    void planFromReleases();
    void launchArguments();
    void supervisorLifecycle();
    void supervisorCrashAndFailures();
    void supervisorKillsProcessTree();
    void supervisorUnloadsModels();
    void installFromGithub();
    void adoptExisting();
    void adoptPythonApps();

private:
    QTemporaryDir m_tmp;
};

void TestRuntime::zipSafePaths()
{
    QCOMPARE(safeEntryPath(QStringLiteral("bin/llama-server.exe")), QStringLiteral("bin/llama-server.exe"));
    QCOMPARE(safeEntryPath(QStringLiteral("./a\\b.dll")), QStringLiteral("a/b.dll"));
    QVERIFY(safeEntryPath(QStringLiteral("../evil.dll")).isEmpty());
    QVERIFY(safeEntryPath(QStringLiteral("a/../../evil.dll")).isEmpty());
    QVERIFY(safeEntryPath(QStringLiteral("/etc/passwd")).isEmpty());
    QVERIFY(safeEntryPath(QStringLiteral("C:/Windows/evil.dll")).isEmpty());
    QVERIFY(safeEntryPath(QStringLiteral("file.txt:stream")).isEmpty());
    QVERIFY(safeEntryPath(QStringLiteral("dir/CON.txt")).isEmpty());
    QVERIFY(safeEntryPath(QStringLiteral("trailingdot.")).isEmpty());
}

void TestRuntime::zipExtract()
{
    QByteArray big;
    for (int i = 0; i < 200000; ++i)
        big += QByteArray::number(i % 977) + ' ';
    const QByteArray zip = makeZip({{QStringLiteral("llama-server.exe"), QByteArray("MZ fake exe")},
                                    {QStringLiteral("lib/"), {}, false},
                                    {QStringLiteral("lib/ggml.dll"), big},
                                    {QStringLiteral("stored.txt"), QByteArray("plain"), false},
                                    {QStringLiteral("empty.txt"), {}}});
    const QString path = m_tmp.filePath(QStringLiteral("ok.zip"));
    QVERIFY(writeFile(path, zip));
    QList<ZipEntry> entries;
    QCOMPARE(listZip(path, &entries), QString());
    QCOMPARE(entries.size(), 5);
    QCOMPARE(entries[2].size, quint64(big.size()));
    const QString dest = m_tmp.filePath(QStringLiteral("ok"));
    qint64 lastDone = 0;
    QCOMPARE(extractZip(path, dest, [&](qint64 done, qint64) {
                 lastDone = done;
                 return true;
             }),
             QString());
    QCOMPARE(lastDone, qint64(big.size() + 16));
    QCOMPARE(readAll(dest + QStringLiteral("/lib/ggml.dll")), big);
    QCOMPARE(readAll(dest + QStringLiteral("/stored.txt")), QByteArray("plain"));
    QVERIFY(QFileInfo::exists(dest + QStringLiteral("/empty.txt")));

    // Cancelling stops early.
    QVERIFY(!extractZip(path, m_tmp.filePath(QStringLiteral("cancel")), [](qint64, qint64) { return false; }).isEmpty());

    // The real release archives (when downloaded for a manual check).
    const QString real = qEnvironmentVariable("LUMEN_TEST_RELEASE_ZIP");
    if (!real.isEmpty()) {
        QTemporaryDir out;
        QCOMPARE(extractZip(real, out.path()), QString()); // CRC-checked
        QList<ZipEntry> realEntries;
        QCOMPARE(listZip(real, &realEntries), QString());
        for (const ZipEntry &e : std::as_const(realEntries)) {
            if (!e.directory)
                QCOMPARE(QFileInfo(out.filePath(e.name)).size(), qint64(e.size));
        }
    }
}

void TestRuntime::zipRejectsUnsafeArchives()
{
    const QString dest = m_tmp.filePath(QStringLiteral("unsafe"));
    auto check = [&](const QList<ZipItem> &items, const char *expect) {
        const QString path = m_tmp.filePath(QStringLiteral("unsafe.zip"));
        QVERIFY(writeFile(path, makeZip(items)));
        const QString err = extractZip(path, dest);
        QVERIFY2(err.contains(QLatin1String(expect)), qPrintable(err));
        // Nothing was written before the check failed.
        QVERIFY(!QFileInfo::exists(dest + QStringLiteral("/good.txt")));
    };
    check({{QStringLiteral("good.txt"), "x"}, {QStringLiteral("../evil.txt"), "x"}}, "unsafe path");
    check({{QStringLiteral("good.txt"), "x"}, {QStringLiteral("C:/evil.txt"), "x"}}, "unsafe path");
    check({{QStringLiteral("good.txt"), "x"}, {QStringLiteral("link"), "/etc", false, true}}, "symbolic link");
    QVERIFY(!QFileInfo::exists(m_tmp.filePath(QStringLiteral("evil.txt"))));

    // Corrupted data is caught by the CRC check.
    QByteArray zip = makeZip({{QStringLiteral("data.bin"), QByteArray(5000, 'a'), false}});
    zip[100] = 'b';
    const QString path = m_tmp.filePath(QStringLiteral("corrupt.zip"));
    QVERIFY(writeFile(path, zip));
    QVERIFY(extractZip(path, m_tmp.filePath(QStringLiteral("corrupt"))).contains(QLatin1String("checksum")));
    QList<ZipEntry> none;
    QVERIFY(listZip(m_tmp.filePath(QStringLiteral("missing.zip")), &none).contains(QLatin1String("Cannot open")));
}

void TestRuntime::downloadVerifiesAndResumes()
{
    MockHttpServer server;
    QByteArray payload;
    for (int i = 0; i < 300000; ++i)
        payload += char('a' + i % 26);
    int requests = 0;
    QByteArray range;
    serveFile(server, QStringLiteral("/file.zip"), payload, &requests, &range);
    QNetworkAccessManager nam;

    // Fresh download.
    DownloadSpec spec;
    spec.url = QUrl(server.baseUrl().toString() + QStringLiteral("/file.zip"));
    spec.destination = m_tmp.filePath(QStringLiteral("dl/file.zip"));
    spec.sha256 = sha256(payload);
    spec.size = payload.size();
    {
        FileDownload d(&nam, spec);
        QSignalSpy done(&d, &FileDownload::finished);
        d.start();
        QVERIFY(done.wait(10000));
        QVERIFY2(d.ok(), qPrintable(d.errorString()));
        QCOMPARE(readAll(spec.destination), payload);
        QVERIFY(!QFileInfo::exists(FileDownload::partPath(spec.destination)));
    }
    // Already there and verified: no request at all.
    {
        requests = 0;
        FileDownload d(&nam, spec);
        QSignalSpy done(&d, &FileDownload::finished);
        d.start();
        QVERIFY(done.wait(10000));
        QVERIFY(d.ok());
        QCOMPARE(requests, 0);
    }
    // Resume from a .part file left by an interrupted run.
    QFile::remove(spec.destination);
    QVERIFY(writeFile(FileDownload::partPath(spec.destination), payload.left(123456)));
    {
        FileDownload d(&nam, spec);
        QSignalSpy done(&d, &FileDownload::finished);
        d.start();
        QVERIFY(done.wait(10000));
        QVERIFY2(d.ok(), qPrintable(d.errorString()));
        QCOMPARE(range, QByteArray("bytes=123456-"));
        QCOMPARE(readAll(spec.destination), payload);
    }
    // A server that ignores Range sends everything: the download starts over and still verifies.
    MockHttpServer noRange;
    serveFile(noRange, QStringLiteral("/file.zip"), payload, nullptr, nullptr, false);
    spec.url = QUrl(noRange.baseUrl().toString() + QStringLiteral("/file.zip"));
    spec.destination = m_tmp.filePath(QStringLiteral("dl/norange.zip"));
    QVERIFY(writeFile(FileDownload::partPath(spec.destination), payload.left(1000)));
    {
        FileDownload d(&nam, spec);
        QSignalSpy done(&d, &FileDownload::finished);
        d.start();
        QVERIFY(done.wait(10000));
        QVERIFY2(d.ok(), qPrintable(d.errorString()));
        QCOMPARE(readAll(spec.destination), payload);
    }
}

void TestRuntime::downloadRejectsTampering()
{
    MockHttpServer server;
    const QByteArray payload(50000, 'x');
    serveFile(server, QStringLiteral("/file.zip"), payload);
    server.route("GET", QStringLiteral("/missing.zip"), [](const MockRequest &) { return MockResponse::json({}, 404); });
    QNetworkAccessManager nam;
    DownloadSpec spec;
    spec.url = QUrl(server.baseUrl().toString() + QStringLiteral("/file.zip"));
    spec.destination = m_tmp.filePath(QStringLiteral("dl/tampered.zip"));
    spec.sha256 = sha256("something else");
    {
        FileDownload d(&nam, spec);
        QSignalSpy done(&d, &FileDownload::finished);
        d.start();
        QVERIFY(done.wait(10000));
        QVERIFY(!d.ok());
        QVERIFY2(d.errorString().contains(QLatin1String("Checksum")), qPrintable(d.errorString()));
        QVERIFY(!QFileInfo::exists(spec.destination));
        QVERIFY(!QFileInfo::exists(FileDownload::partPath(spec.destination)));
    }
    spec.sha256.clear();
    spec.size = 100; // smaller than what the server sends
    {
        FileDownload d(&nam, spec);
        QSignalSpy done(&d, &FileDownload::finished);
        d.start();
        QVERIFY(done.wait(10000));
        QVERIFY(!d.ok());
        QVERIFY(!QFileInfo::exists(spec.destination));
    }
    spec.url = QUrl(server.baseUrl().toString() + QStringLiteral("/missing.zip"));
    spec.size = -1;
    {
        FileDownload d(&nam, spec);
        QSignalSpy done(&d, &FileDownload::finished);
        d.start();
        QVERIFY(done.wait(10000));
        QVERIFY(!d.ok());
        QVERIFY(d.errorString().contains(QLatin1String("404")));
    }
}

void TestRuntime::catalogAndBackendChoice()
{
    const Catalog catalog = Catalog::fromJson(readAll(QStringLiteral(LUMEN_SOURCE_DIR "/resources/runtimes/catalog.json")),
                                              QStringLiteral("windows-x64"));
    const RuntimeSpec *llama = catalog.runtime(QStringLiteral("llama.cpp"));
    const RuntimeSpec *sd = catalog.runtime(QStringLiteral("sd.cpp"));
    QVERIFY(llama && sd);
    QCOMPARE(llama->repo, QStringLiteral("ggml-org/llama.cpp"));
    QCOMPARE(sd->executable, QStringLiteral("sd-server.exe"));
    QVERIFY(catalog.runtimeForExecutable(QStringLiteral("LLAMA-SERVER.EXE")) == llama);
    QVERIFY(catalog.runtimeForExecutable(QStringLiteral("sd-server")) == sd);
    QVERIFY(catalog.runtimeForExecutable(QStringLiteral("notepad.exe")) == nullptr);
    QVERIFY(Catalog::instance().runtime(QStringLiteral("llama.cpp")) != nullptr); // embedded resource

    HardwareInfo nvidia13;
    nvidia13.gpus << GpuInfo{QStringLiteral("NVIDIA GeForce RTX 4060 Laptop GPU"), 0x10DE, 8ull << 30, false};
    parseNvidiaSmi(QStringLiteral("| NVIDIA-SMI 581.57   Driver Version: 581.57   CUDA Version: 13.0     |"), &nvidia13);
    QCOMPARE(nvidia13.nvidiaDriver, QStringLiteral("581.57"));
    QCOMPARE(nvidia13.cudaVersion, QVersionNumber(13, 0));
    QCOMPARE(recommendedBackend(*llama, nvidia13), QStringLiteral("cuda-13"));
    HardwareInfo newBanner;
    parseNvidiaSmi(QStringLiteral("| NVIDIA-SMI 610.62                 KMD Version: 610.62        CUDA UMD Version: 13.3     |"), &newBanner);
    QCOMPARE(newBanner.nvidiaDriver, QStringLiteral("610.62"));
    QCOMPARE(newBanner.cudaVersion, QVersionNumber(13, 3));
    QCOMPARE(recommendedBackend(*sd, nvidia13), QStringLiteral("cuda-12"));

    HardwareInfo oldDriver = nvidia13;
    oldDriver.cudaVersion = QVersionNumber(12, 2);
    QString why;
    QVERIFY(!backendSupported(*llama->backend(QStringLiteral("cuda-12")), oldDriver, &why));
    QVERIFY(why.contains(QLatin1String("12.4")));
    QCOMPARE(recommendedBackend(*llama, oldDriver), QStringLiteral("vulkan"));

    HardwareInfo amd;
    amd.gpus << GpuInfo{QStringLiteral("AMD Radeon RX 7800 XT"), 0x1002, 16ull << 30, false};
    QCOMPARE(recommendedBackend(*llama, amd), QStringLiteral("vulkan"));
    QCOMPARE(recommendedBackend(*sd, HardwareInfo()), QStringLiteral("cpu"));
    QVERIFY(nvidia13.summary().contains(QLatin1String("8.0 GB")));
}

void TestRuntime::hardwareProbe()
{
    QFuture<HardwareInfo> f = probeHardware();
    QTRY_VERIFY_WITH_TIMEOUT(f.isFinished(), 15000);
    const HardwareInfo hw = f.result();
    qInfo() << "hardware:" << hw.summary();
    QVERIFY(hw.cpuThreads > 0);
    QVERIFY(hw.systemRam > 0);
    const QString backend = recommendedBackend(*Catalog::instance().runtime(QStringLiteral("llama.cpp")), hw);
    qInfo() << "recommended llama.cpp build:" << backend;
    QVERIFY(!backend.isEmpty());
}

void TestRuntime::planFromReleases()
{
    const Catalog catalog = Catalog::fromJson(readAll(QStringLiteral(LUMEN_SOURCE_DIR "/resources/runtimes/catalog.json")),
                                              QStringLiteral("windows-x64"));
    const RuntimeSpec &llama = *catalog.runtime(QStringLiteral("llama.cpp"));
    const RuntimeSpec &sd = *catalog.runtime(QStringLiteral("sd.cpp"));
    auto asset = [](const QString &name, bool digest = true) {
        return QJsonObject{{QStringLiteral("name"), name},
                           {QStringLiteral("size"), 1000},
                           {QStringLiteral("browser_download_url"), QStringLiteral("https://github.com/x/") + name},
                           {QStringLiteral("digest"), digest ? QStringLiteral("sha256:AB12") : QString()}};
    };
    auto release = [](const QString &tag, const QString &date, const QJsonArray &assets) {
        return QJsonObject{{QStringLiteral("tag_name"), tag},
                           {QStringLiteral("published_at"), date},
                           {QStringLiteral("prerelease"), true},
                           {QStringLiteral("assets"), assets}};
    };
    const QJsonArray releases{
        // Newest build still uploading: no Windows CUDA archive yet.
        release(QStringLiteral("b11540"), QStringLiteral("2026-10-10T08:00:00Z"),
                {asset(QStringLiteral("llama-b11540-bin-win-cpu-x64.zip"))}),
        release(QStringLiteral("b11539"), QStringLiteral("2026-10-09T22:38:06Z"),
                {asset(QStringLiteral("cudart-llama-bin-win-cuda-12.4-x64.zip")), asset(QStringLiteral("llama-b11539-bin-win-cuda-12.4-x64.zip")),
                 asset(QStringLiteral("llama-b11539-bin-win-cuda-12.4-arm64.zip")), asset(QStringLiteral("llama-b11539-bin-win-vulkan-x64.zip"), false)}),
        release(QStringLiteral("v0.6.0"), QStringLiteral("2026-10-09T23:00:00Z"), {asset(QStringLiteral("unrelated.zip"))}),
        release(QStringLiteral("master-951-f89d9b1"), QStringLiteral("2026-10-09T20:41:56Z"),
                {asset(QStringLiteral("cudart-sd-bin-win-cu12-x64.zip")), asset(QStringLiteral("sd-master-f89d9b1-bin-win-cuda12-x64.zip")),
                 asset(QStringLiteral("sd-master-f89d9b1-bin-win-vulkan-x64.zip"))}),
    };
    QString error;
    const QList<Release> parsed = parseReleases(QJsonDocument(releases).toJson(), &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(parsed.first().tag, QStringLiteral("b11540")); // newest first
    QCOMPARE(parsed[1].tag, QStringLiteral("v0.6.0"));

    const InstallPlan cuda = planInstall(llama, QStringLiteral("cuda-12"), parsed);
    QVERIFY2(cuda.ok(), qPrintable(cuda.error));
    QCOMPARE(cuda.tag, QStringLiteral("b11539"));
    QCOMPARE(cuda.main.name, QStringLiteral("llama-b11539-bin-win-cuda-12.4-x64.zip"));
    QCOMPARE(cuda.libraries.size(), 1);
    QCOMPARE(cuda.libraries.first().name, QStringLiteral("cudart-llama-bin-win-cuda-12.4-x64.zip"));
    QCOMPARE(cuda.main.sha256, QStringLiteral("ab12"));
    QCOMPARE(cuda.downloadSize(), qint64(2000));

    QCOMPARE(planInstall(llama, QStringLiteral("cpu"), parsed).tag, QStringLiteral("b11540"));
    const InstallPlan vulkan = planInstall(llama, QStringLiteral("vulkan"), parsed);
    QVERIFY(!vulkan.ok());
    QVERIFY2(vulkan.error.contains(QLatin1String("checksum")), qPrintable(vulkan.error));
    QVERIFY(!planInstall(llama, QStringLiteral("cuda-13"), parsed).ok());

    const InstallPlan sdPlan = planInstall(sd, QStringLiteral("cuda-12"), parsed);
    QVERIFY2(sdPlan.ok(), qPrintable(sdPlan.error));
    QCOMPARE(sdPlan.main.name, QStringLiteral("sd-master-f89d9b1-bin-win-cuda12-x64.zip"));
    QCOMPARE(sdPlan.libraries.first().name, QStringLiteral("cudart-sd-bin-win-cu12-x64.zip"));
    QCOMPARE(assetPatternFor(sd, QStringLiteral("^sd-master-{2}-x$"), QStringLiteral("master-951-f89d9b1")), QStringLiteral("^sd-master-f89d9b1-x$"));
    QVERIFY(assetPatternFor(sd, QStringLiteral("{2}"), QStringLiteral("b123")).isEmpty());
}

void TestRuntime::launchArguments()
{
    const QString model = m_tmp.filePath(QStringLiteral("models/Qwen3VL-8B Q4.gguf"));
    const QString mmproj = m_tmp.filePath(QStringLiteral("models/mmproj.gguf"));
    QVERIFY(writeFile(model, "gguf"));
    QVERIFY(writeFile(mmproj, "gguf"));
    InstalledRuntime rt;
    rt.kind = QStringLiteral("llama.cpp");
    rt.executable = m_tmp.filePath(QStringLiteral("llama/llama-server.exe"));
    rt.options = {QStringLiteral("--model"), QStringLiteral("--mmproj")}; // an old build: no router mode
    QVERIFY(!llamaRouter(rt));
    ServerProfile p;
    p.name = QStringLiteral("Vision");
    p.port = 8099;
    p.options = {{QStringLiteral("model"), model},
                 {QStringLiteral("mmproj"), mmproj},
                 {QStringLiteral("extra_args"), QStringLiteral("--temp 0.6 --alias \"my model\"")}};
    LaunchCommand cmd = buildLaunch(p, rt);
    QVERIFY2(cmd.error.isEmpty(), qPrintable(cmd.error));
    const QStringList expected = {QStringLiteral("-m"), QDir::toNativeSeparators(model), QStringLiteral("--mmproj"), QDir::toNativeSeparators(mmproj),
                                  QStringLiteral("--host"), QStringLiteral("127.0.0.1"), QStringLiteral("--port"), QStringLiteral("8099"),
                                  QStringLiteral("-c"), QStringLiteral("16384"),
                                  QStringLiteral("-ctk"), QStringLiteral("q8_0"), QStringLiteral("-ctv"), QStringLiteral("q8_0"),
                                  QStringLiteral("-fa"), QStringLiteral("on"), QStringLiteral("--jinja"),
                                  QStringLiteral("--sleep-idle-seconds"), QStringLiteral("30"),
                                  QStringLiteral("--temp"), QStringLiteral("0.6"), QStringLiteral("--alias"), QStringLiteral("my model")};
    QCOMPARE(cmd.arguments, expected);
    QVERIFY(cmd.commandLine().contains(QStringLiteral("\"my model\"")));
    QCOMPARE(providerUrl(rt.kind, 8099), QStringLiteral("http://127.0.0.1:8099/v1"));

    // Router mode: a presets file with every model; shared settings in [*].
    InstalledRuntime router = rt;
    router.options << QStringLiteral("--models-preset");
    QVERIFY(llamaRouter(router));
    const QString textModel = m_tmp.filePath(QStringLiteral("models/Qwen3-8B-Q4_K_M.gguf"));
    QVERIFY(writeFile(textModel, "gguf"));
    ServerProfile multi = p;
    multi.id = QStringLiteral("abc");
    multi.options.insert(QStringLiteral("model_name"), QStringLiteral("vision"));
    multi.options.insert(QStringLiteral("gpu_layers"), 99);
    multi.options.insert(QStringLiteral("more_models"),
                         QVariantList{QVariantMap{{QStringLiteral("model"), textModel}},
                                      QVariantMap{{QStringLiteral("name"), QStringLiteral("vision")}, {QStringLiteral("model"), textModel}}});
    const QList<LlamaModel> models = llamaModels(multi);
    QCOMPARE(models.size(), 3);
    QCOMPARE(models[0].name, QStringLiteral("vision"));
    QVERIFY(models[0].vision());
    QCOMPARE(models[1].name, QStringLiteral("Qwen3-8B-Q4_K_M"));
    QCOMPARE(models[2].name, QStringLiteral("vision-2")); // names stay unique
    const QString filesDir = m_tmp.filePath(QStringLiteral("servers"));
    cmd = buildLaunch(multi, router, filesDir);
    QVERIFY2(cmd.error.isEmpty(), qPrintable(cmd.error));
    const QString ini = QDir(filesDir).filePath(QStringLiteral("abc-models.ini"));
    QVERIFY(cmd.files.contains(ini));
    QCOMPARE(cmd.arguments.mid(0, 4), (QStringList{QStringLiteral("--models-preset"), QDir::toNativeSeparators(ini),
                                                   QStringLiteral("--models-max"), QStringLiteral("1")}));
    QVERIFY(cmd.arguments.contains(QStringLiteral("--temp"))); // extra arguments go to the router
    QVERIFY(!cmd.arguments.contains(QStringLiteral("-m")));
    const QString text = QString::fromUtf8(cmd.files.value(ini));
    QVERIFY2(text.startsWith(QStringLiteral("version = 1\n\n[*]\nc = 16384\nngl = 99\nctk = q8_0\nctv = q8_0\nfa = on\njinja = true\n"
                                            "sleep-idle-seconds = 30\n")),
             qPrintable(text));
    QVERIFY(text.contains(QStringLiteral("\n[vision]\nmodel = %1\nmmproj = %2\n").arg(QDir::fromNativeSeparators(model), QDir::fromNativeSeparators(mmproj))));
    QVERIFY(text.contains(QStringLiteral("\n[Qwen3-8B-Q4_K_M]\nmodel = %1\n").arg(QDir::fromNativeSeparators(textModel))));
    // Changing a model changes the signature (so a running server needs a restart) but not the command line.
    ServerProfile changed = multi;
    changed.options.insert(QStringLiteral("model_name"), QStringLiteral("seer"));
    const LaunchCommand cmd2 = buildLaunch(changed, router, filesDir);
    QCOMPARE(cmd2.commandLine(), cmd.commandLine());
    QVERIFY(cmd2.signature() != cmd.signature());

    // Projectors are found next to their model.
    QVERIFY(writeFile(m_tmp.filePath(QStringLiteral("dl/Qwen3VL-8B-Instruct-Q4_K_M.gguf")), "g"));
    QVERIFY(writeFile(m_tmp.filePath(QStringLiteral("dl/mmproj-Qwen3VL-8B-Instruct-Q8_0.gguf")), "g"));
    QVERIFY(writeFile(m_tmp.filePath(QStringLiteral("dl/Qwen3-8B-Q4_K_M.gguf")), "g"));
    QCOMPARE(findProjector(m_tmp.filePath(QStringLiteral("dl/Qwen3VL-8B-Instruct-Q4_K_M.gguf"))),
             QDir(m_tmp.filePath(QStringLiteral("dl"))).filePath(QStringLiteral("mmproj-Qwen3VL-8B-Instruct-Q8_0.gguf")));
    QVERIFY(findProjector(m_tmp.filePath(QStringLiteral("dl/Qwen3-8B-Q4_K_M.gguf"))).isEmpty());

    p.options.remove(QStringLiteral("model"));
    QVERIFY(buildLaunch(p, rt).error.contains(QLatin1String("model")));
    p.options.insert(QStringLiteral("model"), m_tmp.filePath(QStringLiteral("nope.gguf")));
    QVERIFY(buildLaunch(p, rt).error.contains(QLatin1String("does not exist")));

    rt.kind = QStringLiteral("sd.cpp");
    const QString ckpt = m_tmp.filePath(QStringLiteral("models/waiIllustrious.safetensors"));
    QVERIFY(writeFile(ckpt, "st"));
    ServerProfile s;
    s.name = QStringLiteral("SDXL");
    s.port = 7861;
    s.options = {{QStringLiteral("model"), ckpt}, {QStringLiteral("offload_to_cpu"), true}, {QStringLiteral("max_vram"), 6.5},
                 {QStringLiteral("ip_adapter"), ckpt}};
    QVERIFY(buildLaunch(s, rt).error.contains(QLatin1String("CLIP vision")));
    s.options.insert(QStringLiteral("clip_vision"), ckpt);
    cmd = buildLaunch(s, rt);
    QVERIFY2(cmd.error.isEmpty(), qPrintable(cmd.error));
    QCOMPARE(cmd.arguments.mid(0, 2), (QStringList{QStringLiteral("-m"), QDir::toNativeSeparators(ckpt)}));
    QVERIFY(cmd.arguments.contains(QStringLiteral("--ip-adapter")));
    QVERIFY(cmd.arguments.contains(QStringLiteral("--clip_vision")));
    const qsizetype listen = cmd.arguments.indexOf(QStringLiteral("--listen-port"));
    QCOMPARE(cmd.arguments.value(listen + 1), QStringLiteral("7861"));
    QVERIFY(cmd.arguments.contains(QStringLiteral("--offload-to-cpu")));
    QCOMPARE(cmd.arguments.value(cmd.arguments.indexOf(QStringLiteral("--max-vram")) + 1), QStringLiteral("6.5"));
    QVERIFY(cmd.arguments.contains(QStringLiteral("--diffusion-fa")));

    // VRAM presets.
    QCOMPARE(vramTierFor(8ull << 30), VramTier::Small);
    QCOMPARE(vramTierFor(12ull << 30), VramTier::Medium);
    QCOMPARE(vramTierFor(24ull << 30), VramTier::Large);
    QCOMPARE(presetOptions(QStringLiteral("sd.cpp"), VramTier::Small).value(QStringLiteral("offload_to_cpu")).toBool(), true);
    QCOMPARE(presetOptions(QStringLiteral("sd.cpp"), VramTier::Large).value(QStringLiteral("weight_type")).toString(), QStringLiteral("file"));
    QCOMPARE(presetOptions(QStringLiteral("llama.cpp"), VramTier::Large).value(QStringLiteral("unload_for_images")).toBool(), false);
    QVERIFY(presetOptions(QStringLiteral("sdwebui"), VramTier::Small).isEmpty());

    QCOMPARE(splitArguments(QStringLiteral(" a  \"b c\" \"\" d")), (QStringList{QStringLiteral("a"), QStringLiteral("b c"), QString(), QStringLiteral("d")}));
}

namespace {

struct FakeRuntime
{
    QTemporaryDir dir;
    net::HttpClient http;
    JobQueue jobs;
    RuntimeStore store{&http, &jobs};
    ServerSupervisor supervisor{&store, &http};
    QString model;

    FakeRuntime()
    {
        store.setRoot(dir.filePath(QStringLiteral("runtimes")));
        supervisor.setConfigPath(dir.filePath(QStringLiteral("servers.json")));
        supervisor.setLogDir(dir.filePath(QStringLiteral("logs")));
        supervisor.healthIntervalMs = 100;
        const QString exe = dir.filePath(QStringLiteral("bin/llama-server.exe"));
        writeFile(exe, fakeServerBinary());
        QFile::setPermissions(exe, QFile::permissions(exe) | QFileDevice::ExeOwner);
        InstalledRuntime rt;
        rt.id = QStringLiteral("adopted/fake");
        rt.kind = QStringLiteral("llama.cpp");
        rt.executable = exe;
        rt.directory = QFileInfo(exe).absolutePath();
        store.addInstalled(rt);
        model = dir.filePath(QStringLiteral("model.gguf"));
        writeFile(model, "gguf");
    }

    ServerProfile profile(const QString &extra)
    {
        ServerProfile p = supervisor.createProfile(QStringLiteral("adopted/fake"), QStringLiteral("Fake"));
        p.options = {{QStringLiteral("model"), model}, {QStringLiteral("extra_args"), extra}};
        p.startupTimeoutSec = 20;
        supervisor.addProfile(p);
        return p;
    }
};

QString await(const QFuture<QString> &f, int ms = 20000)
{
    QElapsedTimer t;
    t.start();
    while (!f.isFinished() && t.elapsed() < ms)
        QTest::qWait(20);
    return f.isFinished() ? f.result() : QStringLiteral("timeout");
}

} // namespace

void TestRuntime::supervisorLifecycle()
{
    FakeRuntime fx;
    const ServerProfile p = fx.profile(QStringLiteral("--fake-ready-ms 400"));
    QVERIFY(p.port > 0);
    QCOMPARE(fx.supervisor.state(p.id), ServerState::Stopped);

    QSignalSpy states(&fx.supervisor, &ServerSupervisor::stateChanged);
    QFuture<QString> first = fx.supervisor.acquire(p.id);
    QFuture<QString> second = fx.supervisor.start(p.id); // joins the same start
    QCOMPARE(fx.supervisor.state(p.id), ServerState::Starting);
    QCOMPARE(await(first), QString());
    QCOMPARE(await(second), QString());
    QCOMPARE(fx.supervisor.state(p.id), ServerState::Ready);
    QVERIFY(fx.supervisor.log(p.id).join(QLatin1Char('\n')).contains(QLatin1String("load_tensors")));
    QVERIFY(QFileInfo(fx.supervisor.logPath(p.id)).size() > 0);
    QVERIFY(!fx.supervisor.needsRestart(p.id));
    QCOMPARE(await(fx.supervisor.start(p.id)), QString()); // already running

    // Editing the profile while it runs flags a restart.
    ServerProfile edited = *fx.supervisor.profile(p.id);
    edited.options.insert(QStringLiteral("context"), 4096);
    fx.supervisor.updateProfile(edited);
    QVERIFY(fx.supervisor.needsRestart(p.id));
    QCOMPARE(await(fx.supervisor.restart(p.id)), QString());
    QCOMPARE(fx.supervisor.state(p.id), ServerState::Ready);
    QVERIFY(!fx.supervisor.needsRestart(p.id));

    // Idle stop once the last user releases it.
    ServerProfile idle = *fx.supervisor.profile(p.id);
    idle.idleStopMinutes = 1;
    fx.supervisor.updateProfile(idle);
    fx.supervisor.release(p.id);
    QCOMPARE(fx.supervisor.users(p.id), 0);
    // (The idle timer runs in minutes; stopping is checked directly.)
    fx.supervisor.stop(p.id);
    QTRY_COMPARE_WITH_TIMEOUT(fx.supervisor.state(p.id), ServerState::Stopped, 10000);
    QVERIFY(portIsFree(p.port));

    // Persistence.
    ServerSupervisor reloaded(&fx.store, &fx.http);
    reloaded.setConfigPath(fx.dir.filePath(QStringLiteral("servers.json")));
    reloaded.load();
    QCOMPARE(reloaded.profiles().size(), 1);
    QCOMPARE(reloaded.profiles().first().idleStopMinutes, 1);
    QCOMPARE(reloaded.profiles().first().options.value(QStringLiteral("context")).toInt(), 4096);
}

void TestRuntime::supervisorCrashAndFailures()
{
    FakeRuntime fx;
    // Exits while loading: start fails with the server's last words.
    const ServerProfile early = fx.profile(QStringLiteral("--fake-ready-ms 5000 --fake-exit-after-ms 200"));
    const QString err = await(fx.supervisor.start(early.id));
    QVERIFY2(err.contains(QLatin1String("exited while starting")), qPrintable(err));
    QVERIFY2(err.contains(QLatin1String("fake crash")), qPrintable(err));
    QCOMPARE(fx.supervisor.state(early.id), ServerState::Failed);

    // Crashes while in use: restarted automatically.
    const ServerProfile flaky = fx.profile(QStringLiteral("--fake-ready-ms 100 --fake-exit-after-ms 1500"));
    QCOMPARE(await(fx.supervisor.acquire(flaky.id)), QString());
    QTRY_COMPARE_WITH_TIMEOUT(fx.supervisor.state(flaky.id), ServerState::Failed, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(fx.supervisor.state(flaky.id), ServerState::Ready, 15000);
    QVERIFY(fx.supervisor.log(flaky.id).join(QLatin1Char('\n')).contains(QLatin1String("restarting")));
    fx.supervisor.stop(flaky.id);
    QTRY_COMPARE_WITH_TIMEOUT(fx.supervisor.state(flaky.id), ServerState::Stopped, 10000);

    // Missing model: refused before starting anything.
    ServerProfile broken = fx.profile(QString());
    broken.options.insert(QStringLiteral("model"), fx.dir.filePath(QStringLiteral("missing.gguf")));
    fx.supervisor.updateProfile(broken);
    QVERIFY(await(fx.supervisor.start(broken.id)).contains(QLatin1String("does not exist")));

    // Port taken by someone else.
    QTcpServer squatter;
    QVERIFY(squatter.listen(QHostAddress::LocalHost, 0));
    ServerProfile clash = fx.profile(QString());
    clash.port = squatter.serverPort();
    fx.supervisor.updateProfile(clash);
    QVERIFY(await(fx.supervisor.start(clash.id)).contains(QLatin1String("already used")));

    // Never becomes ready: start-up timeout.
    ServerProfile slow = fx.profile(QStringLiteral("--fake-ready-ms 600000"));
    slow.startupTimeoutSec = 1;
    fx.supervisor.updateProfile(slow);
    QVERIFY(await(fx.supervisor.start(slow.id), 30000).contains(QLatin1String("did not become ready")));
}

void TestRuntime::supervisorKillsProcessTree()
{
#ifndef Q_OS_WIN
    QSKIP("Job objects are Windows-only");
#else
    FakeRuntime fx;
    const ServerProfile p = fx.profile(QStringLiteral("--fake-child --fake-ready-ms 100"));
    QCOMPARE(await(fx.supervisor.start(p.id)), QString());
    qint64 child = 0;
    for (const QString &line : fx.supervisor.log(p.id)) {
        if (line.startsWith(QLatin1String("child ")))
            child = line.mid(6).toLongLong();
    }
    QVERIFY(child > 0);
    QVERIFY(processAlive(child));
    fx.supervisor.stop(p.id);
    QTRY_COMPARE_WITH_TIMEOUT(fx.supervisor.state(p.id), ServerState::Stopped, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!processAlive(child), 5000);

    // Destroying the supervisor (app exit) ends running servers too.
    qint64 child2 = 0;
    {
        ServerSupervisor other(&fx.store, &fx.http);
        other.setConfigPath(fx.dir.filePath(QStringLiteral("servers.json")));
        other.setLogDir(fx.dir.filePath(QStringLiteral("logs2")));
        other.healthIntervalMs = 100;
        other.load();
        QCOMPARE(await(other.start(p.id)), QString());
        for (const QString &line : other.log(p.id)) {
            if (line.startsWith(QLatin1String("child ")))
                child2 = line.mid(6).toLongLong();
        }
        QVERIFY(processAlive(child2));
    }
    QTRY_VERIFY_WITH_TIMEOUT(!processAlive(child2), 5000);
#endif
}

void TestRuntime::supervisorUnloadsModels()
{
    FakeRuntime fx; // the fake llama-server was not probed: router mode
    const ServerProfile p = fx.profile(QStringLiteral("--fake-ready-ms 100"));
    QVERIFY(QString::fromUtf8(fx.supervisor.launchFor(p.id).signature()).contains(QLatin1String("[model]")));
    // Stopped servers have nothing to unload.
    QFuture<void> idle = fx.supervisor.unloadModels(p.id);
    QVERIFY(idle.isFinished());
    QCOMPARE(await(fx.supervisor.start(p.id)), QString());
    QVERIFY(QFileInfo::exists(QDir(fx.supervisor.filesDir()).filePath(p.id + QStringLiteral("-models.ini"))));
    QElapsedTimer t;
    t.start();
    QFuture<void> unload = fx.supervisor.unloadModels(p.id);
    QTRY_VERIFY_WITH_TIMEOUT(unload.isFinished(), 10000);
    // The fake answers "loaded" for 300 ms after the unload request: the supervisor waited for it.
    QVERIFY(t.elapsed() >= 250);
    QCOMPARE(fx.supervisor.state(p.id), ServerState::Ready); // the server keeps running
    fx.supervisor.stop(p.id);
    QTRY_COMPARE_WITH_TIMEOUT(fx.supervisor.state(p.id), ServerState::Stopped, 10000);
}

void TestRuntime::installFromGithub()
{
    MockHttpServer github;
    QTemporaryDir root;
    net::HttpClient http;
    JobQueue jobs;
    RuntimeStore store(&http, &jobs);
    store.setRoot(root.filePath(QStringLiteral("runtimes")));
    store.feed()->setApiBase(github.baseUrl());

    const QByteArray exe = fakeServerBinary();
    QVERIFY(!exe.isEmpty());
    const QByteArray mainZip = makeZip({{QStringLiteral("llama-server.exe"), exe}, {QStringLiteral("ggml-base.dll"), "dll"}});
    const QByteArray mainZip2 = makeZip({{QStringLiteral("llama-server.exe"), exe}, {QStringLiteral("ggml-base.dll"), "dll v2"}});
    const QByteArray cudart = makeZip({{QStringLiteral("cudart64_12.dll"), "cuda runtime"}});
    int cudartRequests = 0;
    serveFile(github, QStringLiteral("/dl/llama-b100-bin-win-cuda-12.4-x64.zip"), mainZip);
    serveFile(github, QStringLiteral("/dl/llama-b101-bin-win-cuda-12.4-x64.zip"), mainZip2);
    serveFile(github, QStringLiteral("/dl/cudart-llama-bin-win-cuda-12.4-x64.zip"), cudart, &cudartRequests);
    auto asset = [&](const QString &name, const QByteArray &data) {
        return QJsonObject{{QStringLiteral("name"), name},
                           {QStringLiteral("size"), data.size()},
                           {QStringLiteral("browser_download_url"), github.baseUrl().toString() + QStringLiteral("/dl/") + name},
                           {QStringLiteral("digest"), QStringLiteral("sha256:") + sha256(data)}};
    };
    QString latestTag = QStringLiteral("b100");
    github.route("GET", QStringLiteral("/repos/ggml-org/llama.cpp/releases"), [&](const MockRequest &req) {
        if (req.headers.value("user-agent").isEmpty())
            return MockResponse::json({}, 403);
        QJsonArray rel;
        rel.append(QJsonObject{{QStringLiteral("tag_name"), QStringLiteral("b100")},
                               {QStringLiteral("published_at"), QStringLiteral("2026-10-01T00:00:00Z")},
                               {QStringLiteral("assets"), QJsonArray{asset(QStringLiteral("llama-b100-bin-win-cuda-12.4-x64.zip"), mainZip),
                                                                     asset(QStringLiteral("cudart-llama-bin-win-cuda-12.4-x64.zip"), cudart)}}});
        if (latestTag == QLatin1String("b101")) {
            rel.prepend(QJsonObject{{QStringLiteral("tag_name"), QStringLiteral("b101")},
                                    {QStringLiteral("published_at"), QStringLiteral("2026-10-02T00:00:00Z")},
                                    {QStringLiteral("assets"), QJsonArray{asset(QStringLiteral("llama-b101-bin-win-cuda-12.4-x64.zip"), mainZip2),
                                                                          asset(QStringLiteral("cudart-llama-bin-win-cuda-12.4-x64.zip"), cudart)}}});
        }
        MockResponse r;
        r.body = QJsonDocument(rel).toJson();
        r.headers.append({"ETag", "\"" + latestTag.toUtf8() + "\""});
        return r;
    });

    const RuntimeSpec &spec = *Catalog::instance().runtime(QStringLiteral("llama.cpp"));
    auto installTag = [&]() -> QString {
        QFuture<QList<Release>> releases = store.feed()->fetch(spec);
        QElapsedTimer wait;
        wait.start();
        while (!releases.isFinished() && wait.elapsed() < 10000)
            QTest::qWait(20);
        if (!releases.isFinished())
            return QStringLiteral("releases: timeout");
        const InstallPlan plan = planInstall(spec, QStringLiteral("cuda-12"), releases.result());
        if (!plan.ok())
            return QStringLiteral("plan: ") + plan.error;
        Job *job = store.install(plan);
        QElapsedTimer t;
        t.start();
        while (!job->isFinished() && t.elapsed() < 30000)
            QTest::qWait(20);
        return job->state() == Job::State::Succeeded ? job->result().toString() : QStringLiteral("job: ") + job->errorString();
    };

    const QString id = installTag();
    QCOMPARE(id, QStringLiteral("llama.cpp/b100-cuda-12"));
    auto rt = store.runtime(id);
    QVERIFY(rt);
    QVERIFY(rt->managed);
    QCOMPARE(rt->version, QStringLiteral("b100"));
    QCOMPARE(rt->versionInfo, QStringLiteral("version: 42 (fake)"));
    QVERIFY(QFileInfo::exists(rt->executable));
    QCOMPARE(rt->libraryDirs.size(), 1);
    QVERIFY(QFileInfo::exists(rt->libraryDirs.first() + QStringLiteral("/cudart64_12.dll")));
    QVERIFY(runtimeEnvironment(*rt).value(QStringLiteral("PATH")).startsWith(QDir::toNativeSeparators(rt->libraryDirs.first())));
    QCOMPARE(cudartRequests, 1);
    // The archives are gone once unpacked.
    QVERIFY(QDir(store.root() + QStringLiteral("/downloads")).entryList(QDir::Files).isEmpty());

    // An update reuses the shared CUDA runtime and keeps the previous version for rollback.
    latestTag = QStringLiteral("b101");
    const QString id2 = installTag();
    QCOMPARE(id2, QStringLiteral("llama.cpp/b101-cuda-12"));
    QCOMPARE(cudartRequests, 1);
    QCOMPARE(store.runtimes(QStringLiteral("llama.cpp")).size(), 2);
    QCOMPARE(store.latestManaged(QStringLiteral("llama.cpp"))->version, QStringLiteral("b101"));
    QCOMPARE(readAll(store.runtime(id2)->directory + QStringLiteral("/ggml-base.dll")), QByteArray("dll v2"));

    // Persisted.
    RuntimeStore reloaded(&http, &jobs);
    reloaded.setRoot(store.root());
    reloaded.load();
    QCOMPARE(reloaded.runtimes().size(), 2);

    // Removing the old one keeps the shared library (still used); removing the last deletes it.
    const QString libDir = rt->libraryDirs.first();
    QVERIFY(store.remove(id));
    QVERIFY(!QFileInfo::exists(rt->directory));
    QVERIFY(QFileInfo::exists(libDir));
    QVERIFY(store.remove(id2));
    QVERIFY(!QFileInfo::exists(libDir));

    // A tampered archive fails verification and installs nothing.
    serveFile(github, QStringLiteral("/dl/llama-b101-bin-win-cuda-12.4-x64.zip"), mainZip2 + "tampered");
    const QString bad = installTag();
    QVERIFY2(bad.contains(QLatin1String("larger than expected")) || bad.contains(QLatin1String("Checksum")), qPrintable(bad));
    QVERIFY(store.runtimes().isEmpty());
}

void TestRuntime::adoptExisting()
{
    QTemporaryDir dir;
    net::HttpClient http;
    JobQueue jobs;
    RuntimeStore store(&http, &jobs);
    store.setRoot(dir.filePath(QStringLiteral("runtimes")));
    const QString exe = dir.filePath(QStringLiteral("llama-b10964-bin-win-cuda-12.4-x64/llama-server.exe"));
    QVERIFY(writeFile(exe, fakeServerBinary()));
    QFuture<InstalledRuntime> f = store.adopt(exe);
    QTRY_VERIFY_WITH_TIMEOUT(f.isFinished(), 20000);
    const InstalledRuntime rt = f.result();
    QVERIFY(!rt.managed);
    QCOMPARE(rt.kind, QStringLiteral("llama.cpp"));
    QCOMPARE(rt.version, QStringLiteral("42"));
    QCOMPARE(store.runtimes().size(), 1);
    // Adopting the same executable again returns the existing entry.
    QFuture<InstalledRuntime> again = store.adopt(exe);
    QTRY_VERIFY_WITH_TIMEOUT(again.isFinished(), 5000);
    QCOMPARE(again.result().id, rt.id);
    // Removing an adopted runtime only forgets it.
    QVERIFY(store.remove(rt.id));
    QVERIFY(QFileInfo::exists(exe));

    // Not a server Lumen knows.
    const QString other = dir.filePath(QStringLiteral("notepad.exe"));
    QVERIFY(writeFile(other, "x"));
    QFuture<InstalledRuntime> refused = store.adopt(other);
    QTRY_VERIFY_WITH_TIMEOUT(refused.isFinished(), 5000);
    bool threw = false;
    try {
        refused.result();
    } catch (const std::exception &) {
        threw = true;
    }
    QVERIFY(threw);
}

void TestRuntime::adoptPythonApps()
{
    QTemporaryDir dir;
    // ComfyUI portable: python_embeded next to the ComfyUI folder.
    const QString root = dir.filePath(QStringLiteral("ComfyUI_windows_portable"));
    QVERIFY(writeFile(root + QStringLiteral("/ComfyUI/main.py"), "print('comfy')"));
    QVERIFY(writeFile(root + QStringLiteral("/ComfyUI/comfy/__init__.py"), ""));
    QVERIFY(writeFile(root + QStringLiteral("/ComfyUI/comfyui_version.py"), "__version__ = \"0.18.5\"\n"));
    QVERIFY(writeFile(root + QStringLiteral("/python_embeded/python.exe"), "MZ"));
    QString error;
    auto comfy = detectPythonApp(root + QStringLiteral("/ComfyUI/main.py"), &error);
    QVERIFY2(comfy, qPrintable(error));
    QCOMPARE(comfy->kind, QStringLiteral("comfyui"));
    QCOMPARE(comfy->version, QStringLiteral("0.18.5"));
    QCOMPARE(QFileInfo(comfy->executable).fileName(), QStringLiteral("python.exe"));
    ServerProfile p;
    p.name = QStringLiteral("ComfyUI");
    p.port = 8190;
    p.options.insert(QStringLiteral("vram_mode"), QStringLiteral("lowvram"));
    LaunchCommand cmd = buildLaunch(p, *comfy);
    QVERIFY2(cmd.error.isEmpty(), qPrintable(cmd.error));
    QCOMPARE(cmd.program, comfy->executable);
    QCOMPARE(cmd.arguments, (QStringList{QDir::toNativeSeparators(root + QStringLiteral("/ComfyUI/main.py")), QStringLiteral("--listen"),
                                         QStringLiteral("127.0.0.1"), QStringLiteral("--port"), QStringLiteral("8190"),
                                         QStringLiteral("--disable-auto-launch"), QStringLiteral("--lowvram")}));
    QCOMPARE(cmd.environment.value(QStringLiteral("PYTHONUNBUFFERED")), QStringLiteral("1"));
    QCOMPARE(providerTypeFor(comfy->kind), QStringLiteral("comfyui"));
    QCOMPARE(providerUrl(comfy->kind, 8190), QStringLiteral("http://127.0.0.1:8190"));

    // A checkout without any Python environment is refused with a reason.
    QVERIFY(writeFile(dir.filePath(QStringLiteral("bare/main.py")), ""));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("bare/comfy/x.py")), ""));
    QVERIFY(!detectPythonApp(dir.filePath(QStringLiteral("bare")), &error));
    QVERIFY(error.contains(QLatin1String("Python")));

    // Forge: webui.bat, API and port through COMMANDLINE_ARGS.
    QVERIFY(writeFile(dir.filePath(QStringLiteral("stable-diffusion-webui-forge/webui.bat")), "@echo off"));
    auto forge = detectPythonApp(dir.filePath(QStringLiteral("stable-diffusion-webui-forge")), &error);
    QVERIFY2(forge, qPrintable(error));
    QCOMPARE(forge->kind, QStringLiteral("sdwebui"));
    QCOMPARE(forge->version, QStringLiteral("Forge"));
    ServerProfile w;
    w.port = 7870;
    w.options.insert(QStringLiteral("extra_args"), QStringLiteral("--xformers"));
    cmd = buildLaunch(w, *forge);
    QVERIFY2(cmd.error.isEmpty(), qPrintable(cmd.error));
#ifdef Q_OS_WIN
    QCOMPARE(cmd.program, QStringLiteral("cmd.exe"));
    QCOMPARE(cmd.arguments.value(0), QStringLiteral("/c"));
#endif
    QCOMPARE(cmd.environment.value(QStringLiteral("COMMANDLINE_ARGS")), QStringLiteral("--api --port 7870 --skip-version-check --xformers"));

    QVERIFY(!detectPythonApp(dir.filePath(QStringLiteral("nothing-here")), &error));

    // Adopting goes through the same detection and does not run anything.
    net::HttpClient http;
    JobQueue jobs;
    RuntimeStore store(&http, &jobs);
    store.setRoot(dir.filePath(QStringLiteral("runtimes")));
    QFuture<InstalledRuntime> adopted = store.adopt(root + QStringLiteral("/ComfyUI"));
    QVERIFY(adopted.isFinished());
    QCOMPARE(adopted.result().kind, QStringLiteral("comfyui"));
    QCOMPARE(store.adopt(root + QStringLiteral("/ComfyUI/main.py")).result().id, adopted.result().id);
    QCOMPARE(store.runtimes().size(), 1);
    RuntimeStore reloaded(&http, &jobs);
    reloaded.setRoot(store.root());
    reloaded.load();
    QCOMPARE(reloaded.runtimes().first().settings.value(QStringLiteral("script")).toString(), root + QStringLiteral("/ComfyUI/main.py"));
}

QTEST_MAIN(TestRuntime)
#include "tst_runtime.moc"
