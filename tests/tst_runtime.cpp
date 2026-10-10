#include "MockHttpServer.h"

#include "core/Job.h"
#include "core/JobQueue.h"
#include "net/HttpClient.h"
#include "runtime/Catalog.h"
#include "runtime/Download.h"
#include "runtime/Hardware.h"
#include "runtime/ModelFormats.h"
#include "runtime/ModelHub.h"
#include "runtime/ModelLibrary.h"
#include "runtime/RuntimeStore.h"
#include "runtime/ServerProfile.h"
#include "runtime/Supervisor.h"
#include "runtime/Zip.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QRegularExpression>
#include <QUrlQuery>
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
    void downloadRedirectsKeepTokenAtHome();
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
    void inspectRealModels();
    void modelFormats();
    void modelLibrary();
    void hubNamesAndStarters();
    void hubSearch();
    void hubDownloads();

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

namespace {

// A safetensors file with the given tensors (name -> shape) and metadata; the data is not needed
// to read the header.
QByteArray makeSafetensors(const QList<QPair<QString, QList<int>>> &tensors, const QJsonObject &meta = {}, const QString &dtype = QStringLiteral("F16"))
{
    QJsonObject header;
    for (const auto &[name, shape] : tensors) {
        QJsonArray dims;
        for (int d : shape)
            dims << d;
        header.insert(name, QJsonObject{{QStringLiteral("dtype"), dtype}, {QStringLiteral("shape"), dims}, {QStringLiteral("data_offsets"), QJsonArray{0, 0}}});
    }
    if (!meta.isEmpty())
        header.insert(QStringLiteral("__metadata__"), meta);
    const QByteArray json = QJsonDocument(header).toJson(QJsonDocument::Compact);
    QByteArray out;
    const quint64 n = quint64(json.size());
    for (int i = 0; i < 8; ++i)
        out.append(char((n >> (8 * i)) & 0xFF));
    return out + json;
}

void putU32(QByteArray &b, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        b.append(char((v >> (8 * i)) & 0xFF));
}

void putU64(QByteArray &b, quint64 v)
{
    for (int i = 0; i < 8; ++i)
        b.append(char((v >> (8 * i)) & 0xFF));
}

void putStr(QByteArray &b, const QByteArray &s)
{
    putU64(b, quint64(s.size()));
    b += s;
}

// A GGUF v3 header: metadata (strings, u32 values, a long string array like a vocabulary) and
// tensor infos.
QByteArray makeGguf(const QString &arch, int fileType, const QList<QPair<QString, QList<int>>> &tensors, bool llmMeta = true)
{
    QByteArray b;
    putU32(b, 0x46554747u);
    putU32(b, 3);
    putU64(b, quint64(tensors.size()));
    QList<QPair<QByteArray, quint32>> ints;
    if (llmMeta) {
        const QByteArray a = arch.toUtf8();
        ints = {{a + ".context_length", 40960}, {a + ".block_count", 36}, {a + ".attention.head_count", 32},
                {a + ".attention.head_count_kv", 8}, {a + ".embedding_length", 4096}};
    }
    putU64(b, quint64(4 + ints.size()));
    putStr(b, "general.architecture");
    putU32(b, 8);
    putStr(b, arch.toUtf8());
    putStr(b, "general.name");
    putU32(b, 8);
    putStr(b, "Test Model 8B");
    putStr(b, "general.file_type");
    putU32(b, 4);
    putU32(b, quint32(fileType));
    putStr(b, "tokenizer.ggml.tokens"); // skipped: too long to keep
    putU32(b, 9);
    putU32(b, 8);
    putU64(b, 1000);
    for (int i = 0; i < 1000; ++i)
        putStr(b, "tok" + QByteArray::number(i));
    for (const auto &[key, value] : ints) {
        putStr(b, key);
        putU32(b, 4);
        putU32(b, value);
    }
    for (const auto &[name, shape] : tensors) {
        putStr(b, name.toUtf8());
        putU32(b, quint32(shape.size()));
        for (int d : shape)
            putU64(b, quint64(d));
        putU32(b, 12);
        putU64(b, 0);
    }
    return b;
}

} // namespace

void TestRuntime::modelFormats()
{
    QTemporaryDir dir;
    auto inspect = [&](const QString &name, const QByteArray &data) {
        const QString path = dir.filePath(name);
        writeFile(path, data);
        return inspectModel(path);
    };
    // SDXL checkpoint (ldm layout), Illustrious by name.
    ModelInfo i = inspect(QStringLiteral("checkpoints/waiIllustriousSDXL_v170.safetensors"),
                          makeSafetensors({{QStringLiteral("model.diffusion_model.input_blocks.0.0.weight"), {320, 4, 3, 3}},
                                           {QStringLiteral("model.diffusion_model.label_emb.0.0.weight"), {1280, 2816}},
                                           {QStringLiteral("conditioner.embedders.1.model.ln_final.weight"), {1280}},
                                           {QStringLiteral("first_stage_model.decoder.conv_in.weight"), {512, 4, 3, 3}}}));
    QCOMPARE(i.kind, ModelKind::Checkpoint);
    QCOMPARE(i.family, QStringLiteral("SDXL"));
    QCOMPARE(i.variant, QStringLiteral("Illustrious"));
    QCOMPARE(i.precision, QStringLiteral("F16"));
    QCOMPARE(i.parameters, qint64(320 * 4 * 9 + 1280 * 2816 + 1280 + 512 * 4 * 9));
    // SD 1.5 checkpoint.
    i = inspect(QStringLiteral("a.safetensors"), makeSafetensors({{QStringLiteral("model.diffusion_model.input_blocks.0.0.weight"), {320, 4, 3, 3}},
                                                                   {QStringLiteral("cond_stage_model.transformer.text_model.final_layer_norm.weight"), {768}}}));
    QCOMPARE(i.family, QStringLiteral("SD 1.5"));
    // Kohya LoRA with SDXL metadata and a trigger phrase.
    i = inspect(QStringLiteral("loras/akane.safetensors"),
                makeSafetensors({{QStringLiteral("lora_unet_input_blocks_4_1_proj_in.lora_down.weight"), {16, 640}},
                                 {QStringLiteral("lora_unet_input_blocks_4_1_proj_in.lora_up.weight"), {640, 16}}},
                                {{QStringLiteral("ss_base_model_version"), QStringLiteral("sdxl_base_v1-0")},
                                 {QStringLiteral("modelspec.trigger_phrase"), QStringLiteral("lmn_akane, red hair")}},
                                QStringLiteral("BF16")));
    QCOMPARE(i.kind, ModelKind::Lora);
    QCOMPARE(i.family, QStringLiteral("SDXL"));
    QCOMPARE(i.triggerWords, (QStringList{QStringLiteral("lmn_akane"), QStringLiteral("red hair")}));
    QCOMPARE(i.precision, QStringLiteral("BF16"));
    // ControlNet (diffusers SDXL), IP-Adapter (SDXL width), CLIP vision, VAE (FLUX.2), upscaler, Anima DiT.
    QCOMPARE(inspect(QStringLiteral("cn.safetensors"), makeSafetensors({{QStringLiteral("controlnet_cond_embedding.conv_in.weight"), {16, 3, 3, 3}},
                                                                         {QStringLiteral("add_embedding.linear_1.weight"), {1280, 2816}}}))
                 .family,
             QStringLiteral("SDXL"));
    i = inspect(QStringLiteral("ipa.safetensors"), makeSafetensors({{QStringLiteral("image_proj.proj.weight"), {8192, 1024}},
                                                                     {QStringLiteral("ip_adapter.1.to_k_ip.weight"), {640, 2048}}}));
    QCOMPARE(i.kind, ModelKind::IpAdapter);
    QCOMPARE(i.family, QStringLiteral("SDXL"));
    QCOMPARE(inspect(QStringLiteral("cv.safetensors"), makeSafetensors({{QStringLiteral("vision_model.encoder.layers.0.mlp.fc1.weight"), {5120, 1280}}})).kind,
             ModelKind::ClipVision);
    i = inspect(QStringLiteral("vae.safetensors"), makeSafetensors({{QStringLiteral("encoder.down_blocks.0.resnets.0.conv1.weight"), {128, 128, 3, 3}},
                                                                     {QStringLiteral("decoder.conv_in.weight"), {512, 32, 3, 3}},
                                                                     {QStringLiteral("decoder.up_blocks.0.resnets.0.conv1.weight"), {512, 512, 3, 3}}}));
    QCOMPARE(i.kind, ModelKind::Vae);
    QCOMPARE(i.family, QStringLiteral("FLUX.2"));
    QCOMPARE(inspect(QStringLiteral("x4.safetensors"), makeSafetensors({{QStringLiteral("conv_first.weight"), {64, 3, 3, 3}},
                                                                         {QStringLiteral("body.0.rdb1.conv1.weight"), {32, 64, 3, 3}}}))
                 .kind,
             ModelKind::Upscaler);
    i = inspect(QStringLiteral("anima.safetensors"), makeSafetensors({{QStringLiteral("net.blocks.0.self_attn.q_proj.weight"), {2048, 2048}},
                                                                       {QStringLiteral("net.llm_adapter.proj.weight"), {2048, 1024}}}));
    QCOMPARE(i.kind, ModelKind::DiffusionModel);
    QCOMPARE(i.family, QStringLiteral("Anima"));
    // Pickle files are never opened as pickles: their kind comes from the folder.
    i = inspect(QStringLiteral("upscale_models/RealESRGAN_x4plus_anime_6B.pth"), QByteArray("PK\x03\x04 pickle"));
    QCOMPARE(i.kind, ModelKind::Upscaler);
    QVERIFY(i.unsafeFormat);
    // A damaged header is reported, not crashed on.
    QVERIFY(!inspect(QStringLiteral("bad.safetensors"), QByteArray("\xff\xff\xff\xff\xff\xff\xff\x7f{")).error.isEmpty());

    // GGUF language model: architecture, quantisation, attention shape; the vocabulary is skipped.
    i = inspect(QStringLiteral("Qwen3-8B-Q4_K_M.gguf"),
                makeGguf(QStringLiteral("qwen3"), 15, {{QStringLiteral("blk.0.attn_q.weight"), {4096, 4096}}, {QStringLiteral("token_embd.weight"), {4096, 1000}}}));
    QVERIFY2(i.error.isEmpty(), qPrintable(i.error));
    QCOMPARE(i.kind, ModelKind::Llm);
    QCOMPARE(i.family, QStringLiteral("qwen3"));
    QCOMPARE(i.precision, QStringLiteral("Q4_K_M"));
    QCOMPARE(i.title, QStringLiteral("Test Model 8B"));
    QCOMPARE(i.contextLength, 40960);
    QCOMPARE(i.layers, 36);
    QCOMPARE(i.kvHeads, 8);
    QCOMPARE(i.headDim, 128);
    QCOMPARE(i.parameters, qint64(4096) * 4096 + qint64(4096) * 1000);
    // KV cache for 16k context at q8_0: 2 × 36 layers × 8 heads × 128 × 16384 × 1.0625 bytes ≈ 1.2 GB.
    const qint64 mem = i.estimatedMemory(5000ll * 1024 * 1024, 16384);
    QVERIFY(mem > 5000ll * 1024 * 1024 + 1100ll * 1024 * 1024 && mem < 5000ll * 1024 * 1024 + 1900ll * 1024 * 1024);
    // Vision projector.
    i = inspect(QStringLiteral("mmproj-Qwen3VL-8B-Instruct-Q8_0.gguf"), makeGguf(QStringLiteral("clip"), 7, {{QStringLiteral("v.blk.0.attn_q.weight"), {1152, 1152}}}, false));
    QCOMPARE(i.kind, ModelKind::Projector);
    QCOMPARE(i.precision, QStringLiteral("Q8_0"));
    // Quantised diffusion model for stable-diffusion.cpp.
    i = inspect(QStringLiteral("flux1-dev-Q4_0.gguf"), makeGguf(QString(), 2, {{QStringLiteral("double_blocks.0.img_attn.qkv.weight"), {3072, 9216}}}, false));
    QCOMPARE(i.kind, ModelKind::DiffusionModel);
    QCOMPARE(i.family, QStringLiteral("FLUX"));
    QVERIFY(!inspect(QStringLiteral("trunc.gguf"), QByteArray("GGUF\x03\x00\x00\x00", 8)).error.isEmpty());

    QCOMPARE(modelKindFromKey(modelKindKey(ModelKind::ControlNet)), ModelKind::ControlNet);
    QCOMPARE(ggufFileTypeName(30), QStringLiteral("IQ4_XS"));
}

void TestRuntime::modelLibrary()
{
    QTemporaryDir data;
    QTemporaryDir comfy;
    MockHttpServer civitai;
    int lookups = 0;
    QString askedHash;
    civitai.route("GET", QString(), [](const MockRequest &) { return MockResponse::json({}, 404); });
    net::HttpClient http;
    ModelLibrary lib(&http);
    lib.setDataDir(data.path());
    lib.setCivitaiBase(civitai.baseUrl().toString());
    QStringList trashed;
    lib.moveToTrash = [&trashed](const QString &path, QString *) {
        trashed << path;
        return QFile::remove(path);
    };
    lib.load();
    QCOMPARE(lib.roots().size(), 1);
    QVERIFY(lib.roots().first().builtin);
    QVERIFY(QFileInfo(lib.defaultFolder() + QStringLiteral("/loras")).isDir());

    // A ComfyUI-style folder with a checkpoint, a LoRA and a small file that is not a model.
    const QByteArray padding(2 * 1024 * 1024, '\0'); // the scan ignores files under 1 MB
    const QByteArray ckpt = makeSafetensors({{QStringLiteral("model.diffusion_model.input_blocks.0.0.weight"), {320, 4, 3, 3}},
                                             {QStringLiteral("model.diffusion_model.label_emb.0.0.weight"), {1280, 2816}},
                                             {QStringLiteral("conditioner.embedders.1.model.ln_final.weight"), {1280}}}) + padding;
    const QByteArray lora = makeSafetensors({{QStringLiteral("lora_unet_a.lora_down.weight"), {8, 640}}},
                                            {{QStringLiteral("ss_base_model_version"), QStringLiteral("sdxl_base_v1-0")}}) + padding;
    QVERIFY(writeFile(comfy.filePath(QStringLiteral("checkpoints/wai.safetensors")), ckpt));
    QVERIFY(writeFile(comfy.filePath(QStringLiteral("loras/style.safetensors")), lora));
    QVERIFY(writeFile(comfy.filePath(QStringLiteral("checkpoints/put_checkpoints_here.safetensors")), "x"));
    QVERIFY(writeFile(lib.defaultFolder() + QStringLiteral("/llm/Qwen3-8B-Q4_K_M.gguf"),
                      makeGguf(QStringLiteral("qwen3"), 15, {{QStringLiteral("blk.0.attn_q.weight"), {4096, 4096}}}) + padding));

    QSignalSpy finished(&lib, &ModelLibrary::scanFinished);
    QVERIFY(lib.addRoot(comfy.path(), QStringLiteral("ComfyUI")));
    QVERIFY(!lib.addRoot(comfy.path())); // already there
    QVERIFY(finished.wait(10000));
    QCOMPARE(lib.entries().size(), 3);
    QCOMPARE(lib.entries(ModelKind::Checkpoint).size(), 1);
    QCOMPARE(lib.entries(ModelKind::Lora).first().info.family, QStringLiteral("SDXL"));
    QCOMPARE(lib.entries(ModelKind::Llm).first().info.precision, QStringLiteral("Q4_K_M"));

    // The index survives a restart; unchanged files are not read again, changed ones are.
    const QString ckptPath = QDir::cleanPath(comfy.filePath(QStringLiteral("checkpoints/wai.safetensors")));
    {
        ModelLibrary again(&http);
        again.setDataDir(data.path());
        again.load();
        QCOMPARE(again.roots().size(), 2);
        QCOMPARE(again.entries().size(), 3);
        QCOMPARE(again.entry(ckptPath)->info.family, QStringLiteral("SDXL"));
    }
    QTest::qWait(1100); // a newer modification time
    QVERIFY(writeFile(ckptPath, makeSafetensors({{QStringLiteral("model.diffusion_model.input_blocks.0.0.weight"), {320, 4, 3, 3}},
                                                 {QStringLiteral("cond_stage_model.transformer.text_model.x.weight"), {768}}}) + padding));
    lib.rescan();
    QVERIFY(finished.wait(10000));
    QCOMPARE(lib.entry(ckptPath)->info.family, QStringLiteral("SD 1.5"));

    // Hash and Civitai lookup (the hash is what is sent).
    QFuture<QString> hash = lib.computeHash(ckptPath);
    QTRY_VERIFY_WITH_TIMEOUT(hash.isFinished(), 10000);
    QCOMPARE(hash.result(), sha256(readAll(ckptPath)));
    civitai.route("GET", QStringLiteral("/api/v1/model-versions/by-hash/") + hash.result().toUpper(), [&](const MockRequest &req) {
        ++lookups;
        askedHash = req.path.section(QLatin1Char('/'), -1);
        return MockResponse::json({{QStringLiteral("id"), 403131},
                                   {QStringLiteral("modelId"), 260267},
                                   {QStringLiteral("name"), QStringLiteral("v3.1")},
                                   {QStringLiteral("baseModel"), QStringLiteral("SDXL 1.0")},
                                   {QStringLiteral("trainedWords"), QJsonArray{QStringLiteral("anime")}},
                                   {QStringLiteral("model"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Animagine XL")}, {QStringLiteral("type"), QStringLiteral("Checkpoint")}}},
                                   {QStringLiteral("images"), QJsonArray{QJsonObject{{QStringLiteral("url"), QStringLiteral("https://image/nsfw.png")}, {QStringLiteral("nsfwLevel"), 8}},
                                                                         QJsonObject{{QStringLiteral("url"), QStringLiteral("https://image/ok.png")}, {QStringLiteral("nsfwLevel"), 1}}}}});
    });
    QFuture<QJsonObject> found = lib.lookupCivitai(ckptPath);
    QTRY_VERIFY_WITH_TIMEOUT(found.isFinished(), 10000);
    QCOMPARE(lookups, 1);
    QCOMPARE(askedHash, hash.result().toUpper());
    QCOMPARE(found.result().value(QStringLiteral("name")).toString(), QStringLiteral("Animagine XL"));
    QCOMPARE(found.result().value(QStringLiteral("preview")).toString(), QStringLiteral("https://image/ok.png"));
    QCOMPARE(found.result().value(QStringLiteral("page")).toString(), QStringLiteral("https://civitai.com/models/260267?modelVersionId=403131"));
    QCOMPARE(lib.entry(ckptPath)->civitai.value(QStringLiteral("baseModel")).toString(), QStringLiteral("SDXL 1.0"));
    // Unknown files are remembered as such.
    const QString loraPath = QDir::cleanPath(comfy.filePath(QStringLiteral("loras/style.safetensors")));
    QFuture<QJsonObject> missing = lib.lookupCivitai(loraPath);
    QTRY_VERIFY_WITH_TIMEOUT(missing.isFinished(), 10000);
    QVERIFY(missing.result().value(QStringLiteral("notFound")).toBool());

    // Suggestions: a folder with models that is not a root yet.
    QTemporaryDir downloads;
    QVERIFY(writeFile(downloads.filePath(QStringLiteral("big.gguf")), QByteArray(11 * 1024 * 1024, 'x')));
    QVERIFY(lib.suggestedRoots(nullptr, {downloads.filePath(QStringLiteral("big.gguf"))}).contains(QDir::cleanPath(downloads.path())));
    QVERIFY(!lib.suggestedRoots(nullptr, {ckptPath}).contains(QDir::cleanPath(comfy.filePath(QStringLiteral("checkpoints")))));

    // Removing a root forgets its files; moving a file to the recycle bin removes it.
    lib.removeRoot(comfy.path());
    QCOMPARE(lib.entries().size(), 1);
    const QString gguf = lib.entries().first().path;
    QString error;
    QVERIFY2(lib.trash(gguf, &error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(gguf));
    QCOMPARE(trashed, QStringList{gguf});
    QVERIFY(lib.entries().isEmpty());
}

// LUMEN_TEST_MODEL_DIRS=<dir>;<dir>: prints how every model file found there is classified.
void TestRuntime::downloadRedirectsKeepTokenAtHome()
{
    QTemporaryDir dir;
    MockHttpServer hub;
    MockHttpServer cdn;
    const QByteArray data(300000, 'm');
    QByteArray hubAuth;
    QByteArray cdnAuth = "unset";
    hub.route("GET", QStringLiteral("/org/repo/resolve/abc/model.gguf"), [&](const MockRequest &req) {
        hubAuth = req.headers.value("authorization");
        MockResponse r;
        r.status = 302;
        r.headers.append({"Location", cdn.baseUrl().toString().toUtf8() + "/blob/signed?x=1"});
        return r;
    });
    // A relative redirect stays at home: the token goes along.
    hub.route("GET", QStringLiteral("/relative"), [](const MockRequest &) {
        MockResponse r;
        r.status = 307;
        r.headers.append(qMakePair(QByteArrayLiteral("Location"), QByteArrayLiteral("/org/repo/resolve/abc/model.gguf")));
        return r;
    });
    cdn.route("GET", QStringLiteral("/blob/signed"), [&](const MockRequest &req) {
        cdnAuth = req.headers.value("authorization");
        MockResponse r;
        r.contentType = "application/octet-stream";
        r.body = data;
        return r;
    });
    QNetworkAccessManager nam;
    DownloadSpec spec;
    spec.url = QUrl(hub.baseUrl().toString() + QStringLiteral("/relative"));
    spec.destination = dir.filePath(QStringLiteral("model.gguf"));
    spec.sha256 = sha256(data);
    spec.size = data.size();
    spec.headers << qMakePair(QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer secret"));
    FileDownload d(&nam, spec);
    QSignalSpy done(&d, &FileDownload::finished);
    d.start();
    QVERIFY(done.wait(10000));
    QVERIFY2(d.ok(), qPrintable(d.errorString()));
    QCOMPARE(hubAuth, QByteArray("Bearer secret"));
    QVERIFY(cdnAuth.isEmpty()); // the CDN never sees the token
    QCOMPARE(readAll(spec.destination), data);

    // Redirect loops end with an error.
    hub.route("GET", QStringLiteral("/loop"), [](const MockRequest &) {
        MockResponse r;
        r.status = 302;
        r.headers.append(qMakePair(QByteArrayLiteral("Location"), QByteArrayLiteral("/loop")));
        return r;
    });
    DownloadSpec loop = spec;
    loop.url = QUrl(hub.baseUrl().toString() + QStringLiteral("/loop"));
    loop.destination = dir.filePath(QStringLiteral("loop.gguf"));
    FileDownload l(&nam, loop);
    QSignalSpy loopDone(&l, &FileDownload::finished);
    l.start();
    QVERIFY(loopDone.wait(10000));
    QVERIFY(!l.ok());
    QVERIFY(l.errorString().contains(QStringLiteral("redirect")));
}

void TestRuntime::hubNamesAndStarters()
{
    QCOMPARE(precisionFromName(QStringLiteral("Qwen3VL-8B-Instruct-Q4_K_M.gguf")), QStringLiteral("Q4_K_M"));
    QCOMPARE(precisionFromName(QStringLiteral("mmproj-Qwen3VL-8B-Instruct-Q8_0.gguf")), QStringLiteral("Q8_0"));
    QCOMPARE(precisionFromName(QStringLiteral("gemma-3-12b-it-IQ4_XS.gguf")), QStringLiteral("IQ4_XS"));
    QCOMPARE(precisionFromName(QStringLiteral("mmproj-F16.gguf")), QStringLiteral("F16"));
    QCOMPARE(precisionFromName(QStringLiteral("model.fp16.safetensors")), QStringLiteral("FP16"));
    QCOMPARE(precisionFromName(QStringLiteral("animagine-xl-4.0-opt.safetensors")), QString());

    auto file = [](const QString &path) {
        HubFile f;
        f.path = path;
        f.format = path.endsWith(QLatin1String(".gguf")) ? QStringLiteral("gguf") : QStringLiteral("safetensors");
        f.kind = path.contains(QLatin1String("mmproj")) ? ModelKind::Projector : ModelKind::Llm;
        return f;
    };
    const QList<HubFile> files{file(QStringLiteral("Big-Q4_K_M-00002-of-00003.gguf")), file(QStringLiteral("Big-Q4_K_M-00001-of-00003.gguf")),
                               file(QStringLiteral("Big-Q4_K_M-00003-of-00003.gguf")), file(QStringLiteral("Big-Q8_0-00001-of-00002.gguf")),
                               file(QStringLiteral("Small-Q4_K_M.gguf")), file(QStringLiteral("mmproj-BF16.gguf")),
                               file(QStringLiteral("mmproj-Q8_0.gguf"))};
    const QList<HubFile> parts = splitParts(files, files[0]);
    QCOMPARE(parts.size(), 3);
    QCOMPARE(parts.first().path, QStringLiteral("Big-Q4_K_M-00001-of-00003.gguf"));
    QCOMPARE(splitParts(files, files[4]).size(), 1);
    QCOMPARE(projectorFor(files, files[4])->path, QStringLiteral("mmproj-Q8_0.gguf"));
    QVERIFY(!projectorFor(files, files[5])); // a projector has none
    // mradermacher-style names: "<model>.mmproj-Q8_0.gguf".
    QCOMPARE(projectorFor({file(QStringLiteral("M.Q4_K_M.gguf")), file(QStringLiteral("M.mmproj-f16.gguf")), file(QStringLiteral("M.mmproj-Q8_0.gguf"))},
                          file(QStringLiteral("M.Q4_K_M.gguf")))->path,
             QStringLiteral("M.mmproj-Q8_0.gguf"));

    // The bundled starter sets: pinned commits, checksums, and the files the wizard needs.
    const QList<StarterSet> sets = starterSets();
    QCOMPARE(sets.size(), 3);
    for (const StarterSet &s : sets) {
        for (const char *role : {"language", "projector", "checkpoint", "control_net", "ip_adapter", "clip_vision", "upscaler"})
            QVERIFY2(s.file(QLatin1String(role)), role);
        for (const StarterFile &f : s.files) {
            QVERIFY2(QRegularExpression(QStringLiteral("^[0-9a-f]{40}$")).match(f.revision).hasMatch(), qPrintable(f.id));
            QVERIFY2(QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(f.sha256).hasMatch(), qPrintable(f.id));
            QVERIFY(f.size > 1000000);
            QVERIFY(f.kind != ModelKind::Unknown);
            QVERIFY(!f.license.isEmpty());
            QVERIFY(f.fileName().endsWith(QLatin1String(".gguf")) || f.fileName().endsWith(QLatin1String(".safetensors")));
        }
    }
    const quint64 gb = 1024ull * 1024 * 1024;
    QCOMPARE(pickStarterSet(sets, 0)->id, QStringLiteral("compact"));
    QCOMPARE(pickStarterSet(sets, 4 * gb)->id, QStringLiteral("compact"));
    QCOMPARE(pickStarterSet(sets, quint64(7.7 * gb))->id, QStringLiteral("standard")); // an "8 GB" laptop GPU
    QCOMPARE(pickStarterSet(sets, 16 * gb)->id, QStringLiteral("large"));
    const HubDownload d = starterDownload(*sets[1].file(QStringLiteral("control_net")));
    QCOMPARE(d.title(), QStringLiteral("xinsir-controlnet-openpose-sdxl-1.0.safetensors"));
    QCOMPARE(d.file.url.toString(), QStringLiteral("https://huggingface.co/xinsir/controlnet-openpose-sdxl-1.0/resolve/"
                                                   "23f966cd5cfdd3f7729c903e243d87152162d2b7/diffusion_pytorch_model.safetensors"));
}

void TestRuntime::hubSearch()
{
    MockHttpServer hf;
    MockHttpServer civ;
    QUrlQuery hfQuery;
    hf.route("GET", QStringLiteral("/api/models"), [&](const MockRequest &req) {
        hfQuery = req.query;
        QJsonArray list;
        list.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("Qwen/Qwen3-VL-8B-Instruct-GGUF")},
                                {QStringLiteral("downloads"), 418007},
                                {QStringLiteral("likes"), 166},
                                {QStringLiteral("gated"), false},
                                {QStringLiteral("pipeline_tag"), QStringLiteral("image-text-to-text")},
                                {QStringLiteral("tags"), QJsonArray{QStringLiteral("gguf"), QStringLiteral("license:apache-2.0")}}});
        list.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("black-forest-labs/FLUX.1-dev")}, {QStringLiteral("gated"), QStringLiteral("auto")}});
        MockResponse r;
        r.body = QJsonDocument(list).toJson();
        r.headers.append({"Link", "<" + hf.baseUrl().toString().toUtf8() + "/api/models?search=qwen&cursor=eyJuZXh0Ijo%3D>; rel=\"next\""});
        return r;
    });
    QUrlQuery civQuery;
    civ.route("GET", QStringLiteral("/api/v1/models"), [&](const MockRequest &req) {
        civQuery = req.query;
        auto model = [](int id, bool nsfw, const QString &file, const QString &format) {
            return QJsonObject{
                {QStringLiteral("id"), id},
                {QStringLiteral("name"), QStringLiteral("Model %1").arg(id)},
                {QStringLiteral("type"), QStringLiteral("LORA")},
                {QStringLiteral("nsfw"), nsfw},
                {QStringLiteral("allowCommercialUse"), QJsonArray{QStringLiteral("Image")}},
                {QStringLiteral("allowDerivatives"), true},
                {QStringLiteral("creator"), QJsonObject{{QStringLiteral("username"), QStringLiteral("someone")}}},
                {QStringLiteral("stats"), QJsonObject{{QStringLiteral("downloadCount"), 1234}}},
                {QStringLiteral("description"), QStringLiteral("<p>A <b>style</b> LoRA &amp; more.</p><p>Second</p>")},
                {QStringLiteral("modelVersions"),
                 QJsonArray{QJsonObject{
                     {QStringLiteral("id"), id * 10},
                     {QStringLiteral("name"), QStringLiteral("v1")},
                     {QStringLiteral("baseModel"), QStringLiteral("Illustrious")},
                     {QStringLiteral("trainedWords"), QJsonArray{QStringLiteral("stylexyz ")}},
                     {QStringLiteral("images"), QJsonArray{QJsonObject{{QStringLiteral("url"), QStringLiteral("https://image.civitai.com/a/original=true/1.jpeg")},
                                                                       {QStringLiteral("nsfwLevel"), 8}},
                                                           QJsonObject{{QStringLiteral("url"), QStringLiteral("https://image.civitai.com/a/original=true/2.jpeg")},
                                                                       {QStringLiteral("nsfwLevel"), 1}}}},
                     {QStringLiteral("files"),
                      QJsonArray{QJsonObject{{QStringLiteral("name"), file},
                                             {QStringLiteral("sizeKB"), 1024.5},
                                             {QStringLiteral("type"), QStringLiteral("Model")},
                                             {QStringLiteral("primary"), true},
                                             {QStringLiteral("metadata"), QJsonObject{{QStringLiteral("format"), format}, {QStringLiteral("fp"), QStringLiteral("fp16")}}},
                                             {QStringLiteral("hashes"), QJsonObject{{QStringLiteral("SHA256"), QStringLiteral("ABCDEF")}}},
                                             {QStringLiteral("downloadUrl"), QStringLiteral("https://civitai.com/api/download/models/%1").arg(id * 10)}},
                                 QJsonObject{{QStringLiteral("name"), QStringLiteral("training.zip")}, {QStringLiteral("type"), QStringLiteral("Training Data")}}}}}}}};
        };
        QJsonArray items{model(1, false, QStringLiteral("style.safetensors"), QStringLiteral("SafeTensor")),
                         model(2, true, QStringLiteral("adult.safetensors"), QStringLiteral("SafeTensor")),
                         model(3, false, QStringLiteral("old.pt"), QStringLiteral("PickleTensor"))};
        return MockResponse::json({{QStringLiteral("items"), items}, {QStringLiteral("metadata"), QJsonObject{{QStringLiteral("nextCursor"), QStringLiteral("2|100")}}}});
    });

    net::HttpClient http;
    JobQueue jobs;
    QTemporaryDir data;
    ModelLibrary lib(&http);
    lib.setDataDir(data.path());
    ModelHub hub(&http, &jobs, &lib);
    hub.setBaseUrls(hf.baseUrl().toString(), civ.baseUrl().toString());

    HubQuery q;
    q.text = QStringLiteral("qwen");
    q.category = QStringLiteral("llm");
    QFuture<HubPage> f = hub.search(HubSource::HuggingFace, q);
    QTRY_VERIFY_WITH_TIMEOUT(f.isFinished(), 10000);
    HubPage page = f.result();
    QCOMPARE(hfQuery.queryItemValue(QStringLiteral("filter")), QStringLiteral("gguf"));
    QCOMPARE(hfQuery.queryItemValue(QStringLiteral("search")), QStringLiteral("qwen"));
    QVERIFY(hfQuery.allQueryItemValues(QStringLiteral("expand[]")).contains(QStringLiteral("gated")));
    QCOMPARE(page.models.size(), 2);
    QCOMPARE(page.models[0].author, QStringLiteral("Qwen"));
    QCOMPARE(page.models[0].license, QStringLiteral("apache-2.0"));
    QCOMPARE(page.models[0].downloads, 418007);
    QVERIFY(page.models[0].gated.isEmpty());
    QCOMPARE(page.models[1].gated, QStringLiteral("auto"));
    QCOMPARE(page.nextCursor, QStringLiteral("eyJuZXh0Ijo="));

    q.text = QStringLiteral("style");
    q.category = QStringLiteral("lora");
    f = hub.search(HubSource::Civitai, q);
    QTRY_VERIFY_WITH_TIMEOUT(f.isFinished(), 10000);
    page = f.result();
    QCOMPARE(civQuery.queryItemValue(QStringLiteral("nsfw")), QStringLiteral("false"));
    QVERIFY(civQuery.allQueryItemValues(QStringLiteral("types")).contains(QStringLiteral("LORA")));
    QCOMPARE(page.models.size(), 2); // the adult model is left out
    const HubModel &m = page.models[0];
    QCOMPARE(m.id, QStringLiteral("1"));
    QCOMPARE(m.description, QStringLiteral("A style LoRA & more.\nSecond"));
    QCOMPARE(m.preview.toString(), QStringLiteral("https://image.civitai.com/a/width=320/2.jpeg")); // the safe image
    QCOMPARE(m.versions.size(), 1);
    QCOMPARE(m.versions[0].trainedWords, QStringList{QStringLiteral("stylexyz")});
    QCOMPARE(m.versions[0].files.size(), 1); // the training data is not a model
    QCOMPARE(m.versions[0].files[0].sha256, QStringLiteral("abcdef"));
    QCOMPARE(m.versions[0].files[0].kind, ModelKind::Lora);
    QCOMPARE(m.versions[0].files[0].precision, QStringLiteral("FP16"));
    QVERIFY(page.models[1].versions[0].files[0].pickle());
    QCOMPARE(page.nextCursor, QStringLiteral("2|100"));

    // Civitai has no language models.
    q.category = QStringLiteral("llm");
    f = hub.search(HubSource::Civitai, q);
    QVERIFY(f.isFinished());
    QVERIFY(f.result().models.isEmpty());
}

void TestRuntime::hubDownloads()
{
    MockHttpServer hf;
    MockHttpServer cdn;
    const QByteArray padding(2 * 1024 * 1024, '\0');
    // Named like a checkpoint, but the header says LoRA: it ends up in loras/.
    const QByteArray lora = makeSafetensors({{QStringLiteral("lora_unet_a.lora_down.weight"), {8, 640}}},
                                            {{QStringLiteral("ss_base_model_version"), QStringLiteral("sdxl_base_v1-0")}}) + padding;
    const QByteArray gguf = makeGguf(QStringLiteral("qwen3vl"), 15, {{QStringLiteral("blk.0.attn_q.weight"), {4096, 4096}}}) + padding;
    const QString sha = QStringLiteral("0123456789abcdef0123456789abcdef01234567");
    hf.route("GET", QStringLiteral("/api/models/org/repo"), [&](const MockRequest &) {
        return MockResponse::json({{QStringLiteral("id"), QStringLiteral("org/repo")},
                                   {QStringLiteral("sha"), sha},
                                   {QStringLiteral("pipeline_tag"), QStringLiteral("text-to-image")},
                                   {QStringLiteral("cardData"), QJsonObject{{QStringLiteral("license"), QStringLiteral("openrail++")}}}});
    });
    hf.route("GET", QStringLiteral("/api/models/org/repo/tree/") + sha, [&](const MockRequest &req) {
        QJsonArray tree;
        tree.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("file")}, {QStringLiteral("path"), QStringLiteral("README.md")}, {QStringLiteral("size"), 100}});
        tree.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("file")},
                                {QStringLiteral("path"), QStringLiteral("unet/model one.safetensors")},
                                {QStringLiteral("size"), lora.size()},
                                {QStringLiteral("lfs"), QJsonObject{{QStringLiteral("oid"), sha256(lora)}}}});
        tree.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("file")},
                                {QStringLiteral("path"), QStringLiteral("Model-Q4_K_M.gguf")},
                                {QStringLiteral("size"), gguf.size()},
                                {QStringLiteral("lfs"), QJsonObject{{QStringLiteral("oid"), sha256(gguf)}}}});
        tree.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("file")},
                                {QStringLiteral("path"), QStringLiteral("old.ckpt")},
                                {QStringLiteral("size"), 5000000},
                                {QStringLiteral("lfs"), QJsonObject{{QStringLiteral("oid"), QStringLiteral("aa")}}}});
        MockResponse r = MockResponse::json({});
        r.body = req.query.queryItemValue(QStringLiteral("recursive")) == QLatin1String("true") ? QJsonDocument(tree).toJson() : "[]";
        return r;
    });
    QByteArray auth = "unset";
    hf.route("GET", QStringLiteral("/org/repo/resolve/") + sha + QStringLiteral("/unet/model one.safetensors"), [&](const MockRequest &req) {
        auth = req.headers.value("authorization");
        MockResponse r;
        r.status = 302;
        r.headers.append({"Location", cdn.baseUrl().toString().toUtf8() + "/xet/lora"});
        return r;
    });
    serveFile(cdn, QStringLiteral("/xet/lora"), lora);
    serveFile(hf, QStringLiteral("/org/repo/resolve/") + sha + QStringLiteral("/Model-Q4_K_M.gguf"), gguf);

    net::HttpClient http;
    JobQueue jobs;
    QTemporaryDir data;
    ModelLibrary lib(&http);
    lib.setDataDir(data.path());
    lib.load();
    ModelHub hub(&http, &jobs, &lib);
    hub.setBaseUrls(hf.baseUrl().toString(), cdn.baseUrl().toString());
    hub.tokenProvider = [](HubSource s) { return s == HubSource::HuggingFace ? QStringLiteral("hf_test") : QString(); };

    HubModel start;
    start.id = QStringLiteral("org/repo");
    QFuture<HubModel> df = hub.details(start);
    QTRY_VERIFY_WITH_TIMEOUT(df.isFinished(), 10000);
    const HubModel m = df.result();
    QCOMPARE(m.license, QStringLiteral("openrail++"));
    QCOMPARE(m.versions.size(), 1);
    const QList<HubFile> files = m.versions[0].files;
    QCOMPARE(files.size(), 3); // README is not a model
    QCOMPARE(files[0].path, QStringLiteral("Model-Q4_K_M.gguf"));
    QCOMPARE(files[0].kind, ModelKind::Llm);
    QCOMPARE(files[0].precision, QStringLiteral("Q4_K_M"));
    QVERIFY(files[1].pickle());
    QCOMPARE(files[2].kind, ModelKind::Checkpoint); // a guess from the repository's pipeline
    QVERIFY(files[2].url.toString(QUrl::FullyEncoded).endsWith(QStringLiteral("/unet/model%20one.safetensors")));

    auto request = [&](const HubFile &f) {
        HubDownload d;
        d.source = HubSource::HuggingFace;
        d.modelId = m.id;
        d.modelName = m.name;
        d.versionId = sha;
        d.file = f;
        d.license = m.license;
        d.page = m.page;
        return d;
    };
    QSignalSpy downloaded(&hub, &ModelHub::downloaded);
    Job *j1 = hub.download(request(files[2]));
    Job *j2 = hub.download(request(files[0]));
    QTRY_VERIFY_WITH_TIMEOUT(j1->isFinished() && j2->isFinished(), 20000);
    QVERIFY2(j1->state() == Job::State::Succeeded, qPrintable(j1->errorString()));
    QVERIFY2(j2->state() == Job::State::Succeeded, qPrintable(j2->errorString()));
    QCOMPARE(auth, QByteArray("Bearer hf_test"));
    const QString loraPath = j1->result().toString();
    QCOMPARE(loraPath, QDir::cleanPath(lib.defaultFolder() + QStringLiteral("/loras/model one.safetensors")));
    QCOMPARE(readAll(loraPath), lora);
    const QJsonObject side = readSidecar(loraPath);
    QCOMPARE(side.value(QStringLiteral("source")).toString(), QStringLiteral("huggingface"));
    QCOMPARE(side.value(QStringLiteral("version")).toString(), sha);
    QCOMPARE(side.value(QStringLiteral("license")).toString(), QStringLiteral("openrail++"));
    QCOMPARE(downloaded.size(), 2);
    const auto entry = lib.entry(loraPath);
    QVERIFY(entry);
    QCOMPARE(entry->info.kind, ModelKind::Lora);
    QCOMPARE(entry->sha256, sha256(lora)); // verified while downloading, not hashed again
    QCOMPARE(entry->source.value(QStringLiteral("model")).toString(), QStringLiteral("org/repo"));
    QCOMPARE(lib.find(sha256(gguf))->path, QDir::cleanPath(lib.defaultFolder() + QStringLiteral("/llm/Model-Q4_K_M.gguf")));

    // The same file again: recognised on disk, not downloaded twice.
    Job *again = hub.download(request(files[0]));
    QTRY_VERIFY_WITH_TIMEOUT(again->isFinished(), 10000);
    QCOMPARE(again->state(), Job::State::Succeeded);
    QCOMPARE(again->result().toString(), lib.find(sha256(gguf))->path);
    // A different file of the same name gets another name; the existing one stays.
    HubFile other = files[0];
    other.sha256 = sha256(gguf + "x");
    other.size = gguf.size();
    Job *clash = hub.download(request(other));
    QTRY_VERIFY_WITH_TIMEOUT(clash->isFinished(), 10000);
    QCOMPARE(clash->state(), Job::State::Failed); // checksum mismatch, served bytes differ
    QCOMPARE(readAll(lib.defaultFolder() + QStringLiteral("/llm/Model-Q4_K_M.gguf")), gguf);

    // Pickle files need an explicit yes.
    Job *pickle = hub.download(request(files[1]));
    QTRY_VERIFY_WITH_TIMEOUT(pickle->isFinished(), 10000);
    QCOMPARE(pickle->state(), Job::State::Failed);
    QVERIFY(pickle->errorString().contains(QStringLiteral("pickle")));

    // Civitai without an API key: a clear message.
    cdn.route("GET", QStringLiteral("/api/download/models/5"), [](const MockRequest &) { return MockResponse::json({}, 401); });
    HubDownload civ;
    civ.source = HubSource::Civitai;
    civ.modelId = QStringLiteral("1");
    civ.versionId = QStringLiteral("5");
    civ.file.path = QStringLiteral("style.safetensors");
    civ.file.format = QStringLiteral("safetensors");
    civ.file.kind = ModelKind::Lora;
    civ.file.url = QUrl(cdn.baseUrl().toString() + QStringLiteral("/api/download/models/5"));
    Job *denied = hub.download(civ);
    QTRY_VERIFY_WITH_TIMEOUT(denied->isFinished(), 10000);
    QCOMPARE(denied->state(), Job::State::Failed);
    QVERIFY2(denied->errorString().contains(QStringLiteral("API key")), qPrintable(denied->errorString()));
}

void TestRuntime::inspectRealModels()
{
    const QStringList dirs = qEnvironmentVariable("LUMEN_TEST_MODEL_DIRS").split(QLatin1Char(';'), Qt::SkipEmptyParts);
    if (dirs.isEmpty())
        QSKIP("LUMEN_TEST_MODEL_DIRS is not set");
    for (const QString &dir : dirs) {
        QDirIterator it(dir, modelFileFilters(), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            QElapsedTimer t;
            t.start();
            const ModelInfo info = inspectModel(path);
            qInfo().noquote() << QStringLiteral("%1 | %2 | %3 %4 | %5 | %6M params | ctx %7 L%8 kv%9x%10 | %11 ms %12")
                                     .arg(QFileInfo(path).fileName(), modelKindKey(info.kind), info.family, info.variant, info.precision)
                                     .arg(info.parameters / 1000000)
                                     .arg(info.contextLength)
                                     .arg(info.layers)
                                     .arg(info.kvHeads)
                                     .arg(info.headDim)
                                     .arg(t.elapsed())
                                     .arg(info.error);
        }
    }
}

QTEST_MAIN(TestRuntime)
#include "tst_runtime.moc"
