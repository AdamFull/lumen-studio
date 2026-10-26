#include "MockHttpServer.h"

#include "core/Async.h"
#include "core/Job.h"
#include "core/JobQueue.h"
#include "core/Paths.h"
#include "net/HttpClient.h"
#include "runtime/Catalog.h"
#include "runtime/Download.h"
#include "runtime/Hardware.h"
#include "runtime/LoraTraining.h"
#include "runtime/ModelFormats.h"
#include "runtime/ModelHub.h"
#include "runtime/ModelPresets.h"
#include "runtime/ModelLibrary.h"
#include "runtime/ModelsFolder.h"
#include "runtime/CloudTraining.h"
#include "runtime/PodAgent.h"
#include "runtime/RunPod.h"
#include "runtime/RuntimeStore.h"
#include "runtime/ServerProfile.h"
#include "runtime/Trainer.h"
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
#include <QProcess>
#include <QRegularExpression>
#include <QUrlQuery>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QVersionNumber>

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
    void runPodClient();
    void podAgent();
    void cloudTraining();
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
    void modelsFolderDefaults();
    void modelsFolderMove();
    void modelsFolderCopies();
    void modelsFolderLinks();
    void legacyModelsFirstStart();
    void hubNamesAndStarters();
    void hubSearch();
    void hubDownloads();
    void hubCompanions();
    void animaCompanions();
    void modelPresets();
    void modelTypes();
    void presetProfiles();
    void trainerRecipes();
    void trainerAdopt();
    void trainerInstall();
    void loraTraining();

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

    // A profile the preflight check refuses: its message instead of a start.
    ServerProfile refused = fx.profile(QString());
    fx.supervisor.preflight = [&refused](const ServerProfile &p, const QString &) {
        return p.id == refused.id ? QStringLiteral("The model is in the wrong slot.") : QString();
    };
    QCOMPARE(await(fx.supervisor.start(refused.id)), QStringLiteral("The model is in the wrong slot."));
    QCOMPARE(fx.supervisor.state(refused.id), ServerState::Failed);
    fx.supervisor.preflight = nullptr;

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
    QCOMPARE(i.inspector, kModelInspectorVersion);
    // The Qwen-Image VAE (Wan layout, 3D convolutions, 16 latent channels) and Wan 2.2's (48).
    auto wanVae = [](int channels) {
        return makeSafetensors({{QStringLiteral("decoder.conv1.weight"), {384, channels, 3, 3, 3}},
                                {QStringLiteral("decoder.upsamples.0.residual.2.weight"), {384, 384, 3, 3, 3}},
                                {QStringLiteral("encoder.middle.0.residual.2.weight"), {384, 384, 3, 3, 3}}},
                               {}, QStringLiteral("BF16"));
    };
    i = inspect(QStringLiteral("qwen_image_vae.safetensors"), wanVae(16));
    QCOMPARE(i.kind, ModelKind::Vae);
    QCOMPARE(i.family, QStringLiteral("Qwen-Image / Wan"));
    QCOMPARE(inspect(QStringLiteral("wan2.2_vae.safetensors"), wanVae(48)).family, QStringLiteral("Wan 2.2"));
    // Language-model text encoders: Qwen3 by its q/k norms, the size by the width.
    auto qwen = [](int width, bool qwen3) {
        QList<QPair<QString, QList<int>>> t{{QStringLiteral("model.embed_tokens.weight"), {151936, width}},
                                            {QStringLiteral("model.layers.0.self_attn.q_proj.weight"), {width, width}}};
        if (qwen3)
            t.append({QStringLiteral("model.layers.0.self_attn.q_norm.weight"), {128}});
        return makeSafetensors(t, {}, QStringLiteral("BF16"));
    };
    i = inspect(QStringLiteral("text_encoders/qwen_3_06b_base.safetensors"), qwen(1024, true));
    QCOMPARE(i.kind, ModelKind::TextEncoder);
    QCOMPARE(i.family, QStringLiteral("LLM"));
    QCOMPARE(i.variant, QStringLiteral("Qwen3 0.6B"));
    QCOMPARE(inspect(QStringLiteral("q8b.safetensors"), qwen(4096, true)).variant, QStringLiteral("Qwen3 8B"));
    QCOMPARE(inspect(QStringLiteral("q25.safetensors"), qwen(3584, false)).variant, QStringLiteral("Qwen2.5 7B"));
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

void TestRuntime::modelsFolderDefaults()
{
    // With relocated data (LUMEN_DATA_DIR, the portable ZIP) or in Qt's test mode the standard
    // folder stays next to the data, which is also where earlier versions kept it.
    QStandardPaths::setTestModeEnabled(true);
    auto restore = qScopeGuard([] { QStandardPaths::setTestModeEnabled(false); });
    const QString dataModels = QDir::cleanPath(paths::appData() + QStringLiteral("/models"));
    QCOMPARE(QDir::cleanPath(paths::defaultModels()), dataModels);
    QCOMPARE(legacyModelsFolder(), dataModels);
    net::HttpClient http;
    {
        ModelLibrary lib(&http);
        QCOMPARE(lib.standardFolder(), dataModels);
        QCOMPARE(lib.defaultFolder(), dataModels);
        QVERIFY(legacyModelsToAsk(&lib).isEmpty()); // the old and the new default are one folder here
    }

    QTemporaryDir data;
    QTemporaryDir chosen;
    ModelLibrary lib(&http);
    lib.setDataDir(data.path());
    const QString standard = QDir::cleanPath(data.filePath(QStringLiteral("models")));
    QCOMPARE(lib.standardFolder(), standard);
    QCOMPARE(lib.defaultFolder(), standard);
    // ComfyUI's names, plus Lumen's own for language models and their vision projectors.
    QCOMPARE(ModelLibrary::subfolders(), (QStringList{QStringLiteral("checkpoints"), QStringLiteral("diffusion_models"), QStringLiteral("loras"),
                                                      QStringLiteral("controlnet"), QStringLiteral("ipadapter"), QStringLiteral("clip_vision"),
                                                      QStringLiteral("vae"), QStringLiteral("text_encoders"), QStringLiteral("upscale_models"),
                                                      QStringLiteral("embeddings"), QStringLiteral("llm")}));
    QCOMPARE(ModelLibrary::subfolderFor(ModelKind::Projector), QStringLiteral("llm"));

    // The chosen folder (the setting) is the library's own, with its sub-folders.
    QSignalSpy folderChanged(&lib, &ModelLibrary::folderChanged);
    lib.setFolder(QDir::toNativeSeparators(chosen.path()));
    QCOMPARE(folderChanged.size(), 1);
    lib.setFolder(chosen.path() + QStringLiteral("/"));
    QCOMPARE(folderChanged.size(), 1); // the same folder
    QCOMPARE(lib.chosenFolder(), QDir::cleanPath(chosen.path()));
    QCOMPARE(lib.defaultFolder(), QDir::cleanPath(chosen.path()));
    lib.load();
    QCOMPARE(lib.roots().size(), 1);
    QVERIFY(lib.roots().first().builtin);
    QCOMPARE(lib.roots().first().path, QDir::cleanPath(chosen.path()));
    for (const QString &sub : ModelLibrary::subfolders())
        QVERIFY2(QFileInfo(chosen.filePath(sub)).isDir(), qPrintable(sub));
    QVERIFY(!QFileInfo::exists(standard)); // not created while another folder is used

    // Back to the standard folder.
    lib.setFolder(QString());
    QCOMPARE(folderChanged.size(), 2);
    QVERIFY(lib.chosenFolder().isEmpty());
    QCOMPARE(lib.roots().first().path, standard);
    QVERIFY(QFileInfo(standard + QStringLiteral("/diffusion_models")).isDir());
    // An added folder that becomes Lumen's own is listed once.
    QTemporaryDir comfy;
    QVERIFY(lib.addRoot(comfy.path(), QStringLiteral("ComfyUI")));
    QCOMPARE(lib.roots().size(), 2);
    lib.setFolder(comfy.path());
    QCOMPARE(lib.roots().size(), 1);
    QCOMPARE(lib.roots().first().path, QDir::cleanPath(comfy.path()));
    QVERIFY(lib.roots().first().builtin);
    QTRY_VERIFY_WITH_TIMEOUT(!lib.isScanning(), 10000);
}

void TestRuntime::modelsFolderMove()
{
    FakeRuntime fx;
    QTemporaryDir data;
    QTemporaryDir elsewhere;
    ModelLibrary lib(&fx.http);
    lib.setDataDir(data.path());
    lib.load();
    const QString from = lib.defaultFolder();
    const QString to = QDir::cleanPath(elsewhere.filePath(QStringLiteral("Lumen/Models")));
    const QByteArray padding(2 * 1024 * 1024, '\0');
    const QByteArray ckpt = makeSafetensors({{QStringLiteral("model.diffusion_model.input_blocks.0.0.weight"), {320, 4, 3, 3}},
                                             {QStringLiteral("model.diffusion_model.label_emb.0.0.weight"), {1280, 2816}},
                                             {QStringLiteral("conditioner.embedders.1.model.ln_final.weight"), {1280}}}) + padding;
    const QByteArray lora = makeSafetensors({{QStringLiteral("lora_unet_a.lora_down.weight"), {8, 640}}},
                                            {{QStringLiteral("ss_base_model_version"), QStringLiteral("sdxl_base_v1-0")}}) + padding;
    const QByteArray gguf = makeGguf(QStringLiteral("qwen3"), 15, {{QStringLiteral("blk.0.attn_q.weight"), {4096, 4096}}}) + padding;
    QVERIFY(writeFile(from + QStringLiteral("/checkpoints/wai.safetensors"), ckpt));
    QVERIFY(writeFile(sidecarPath(from + QStringLiteral("/checkpoints/wai.safetensors")),
                      QJsonDocument(QJsonObject{{QStringLiteral("source"), QStringLiteral("huggingface")},
                                                {QStringLiteral("sha256"), sha256(ckpt)},
                                                {QStringLiteral("size"), ckpt.size()}})
                          .toJson()));
    QVERIFY(writeFile(from + QStringLiteral("/loras/style.safetensors"), lora));
    QVERIFY(writeFile(from + QStringLiteral("/llm/Model-Q4_K_M.gguf"), gguf));
    QVERIFY(writeFile(from + QStringLiteral("/loras/half.safetensors.part"), "an unfinished download"));
    QVERIFY(writeFile(from + QStringLiteral("/notes.txt"), "not a model: stays"));
    QVERIFY(writeFile(from + QStringLiteral("/llm/chat.jinja"), "a template a server uses: stays"));
    // A folder added to the library that lies inside Lumen's folder (ComfyUI's): it never moves.
    const QString comfy = from + QStringLiteral("/ComfyUI/models");
    QVERIFY(writeFile(comfy + QStringLiteral("/checkpoints/comfy.safetensors"), ckpt));
    QVERIFY(QDir().mkpath(comfy + QStringLiteral("/upscale_models")));
    QVERIFY(lib.addRoot(comfy, QStringLiteral("ComfyUI")));
    lib.rescan();
    QTRY_VERIFY_WITH_TIMEOUT(!lib.isScanning() && lib.entries().size() == 4, 10000);
    QFuture<QString> hash = lib.computeHash(from + QStringLiteral("/loras/style.safetensors"));
    QTRY_VERIFY_WITH_TIMEOUT(hash.isFinished(), 10000);

    // A running server and a stopped one, with paths into the folder in several forms.
    const QString nativeGguf = QDir::toNativeSeparators(from + QStringLiteral("/llm/Model-Q4_K_M.gguf"));
    const QString nativeLoras = QDir::toNativeSeparators(from + QStringLiteral("/loras"));
    ServerProfile running = fx.supervisor.createProfile(QStringLiteral("adopted/fake"), QStringLiteral("Running"));
    running.options = {{QStringLiteral("model"), nativeGguf},
                       {QStringLiteral("extra_args"), QStringLiteral("--fake-ready-ms 100 --lora-dir \"%1\"").arg(nativeLoras)}};
    running.startupTimeoutSec = 20;
    fx.supervisor.addProfile(running);
    ServerProfile stopped = fx.supervisor.createProfile(QStringLiteral("adopted/fake"), QStringLiteral("Stopped"));
    const QString nativeTemplate = QDir::toNativeSeparators(from + QStringLiteral("/llm/chat.jinja"));
    const QString siblings = QStringLiteral("--chat-template-file \"%1\" --lora-dir \"%2 backup\\loras\" --x %3/ComfyUI/models/checkpoints")
                                 .arg(nativeTemplate, QDir::toNativeSeparators(from), from);
    stopped.options = {{QStringLiteral("model"), fx.model}, // outside the folder
                       {QStringLiteral("lora_dir"), nativeLoras},
                       {QStringLiteral("vae"), from + QStringLiteral("2/vae.safetensors")}, // a folder with a similar name
                       {QStringLiteral("clip_vision"), from + QStringLiteral(" 2/clip.safetensors")},
                       {QStringLiteral("ip_adapter"), comfy + QStringLiteral("/checkpoints/comfy.safetensors")}, // in the added folder
                       {QStringLiteral("extra_args"), siblings},
                       {QStringLiteral("more_models"), QVariantList{QVariantMap{{QStringLiteral("name"), QStringLiteral("Second")},
                                                                                {QStringLiteral("model"), from + QStringLiteral("/llm/Model-Q4_K_M.gguf")},
                                                                                {QStringLiteral("mmproj"), QString()}}}}};
    fx.supervisor.addProfile(stopped);

    // Refused plans.
    const FolderMovePlan plan = planModelsFolderMove(&lib, to);
    QVERIFY2(plan.ok(), qPrintable(plan.error));
    QCOMPARE(plan.keep, QStringList{comfy});
    QVERIFY(plan.links.isEmpty());
    QCOMPARE(plan.models, 3);
    QVERIFY(plan.sameVolume);
    QStringList files = plan.files;
    files.sort();
    QCOMPARE(files, (QStringList{QStringLiteral("checkpoints/wai.safetensors"), QStringLiteral("checkpoints/wai.safetensors.lumen.json"),
                                 QStringLiteral("llm/Model-Q4_K_M.gguf"), QStringLiteral("loras/half.safetensors.part"),
                                 QStringLiteral("loras/style.safetensors")}));
    QVERIFY(!planModelsFolderMove(from, QString()).ok());
    QVERIFY(!planModelsFolderMove(from, QDir::toNativeSeparators(from)).ok());
    QVERIFY(!planModelsFolderMove(from, from + QStringLiteral("/loras/inside")).ok());
    QVERIFY(writeFile(elsewhere.filePath(QStringLiteral("file.txt")), "x"));
    QVERIFY(!planModelsFolderMove(from, elsewhere.filePath(QStringLiteral("file.txt"))).ok());
    QVERIFY(writeFile(elsewhere.filePath(QStringLiteral("taken/loras/style.safetensors")), "another file"));
    const FolderMovePlan taken = planModelsFolderMove(from, elsewhere.filePath(QStringLiteral("taken")));
    QVERIFY(!taken.ok());
    QCOMPARE(taken.conflicts, QStringList{QStringLiteral("loras/style.safetensors")});
    QVERIFY2(taken.error.contains(QStringLiteral("style.safetensors")), qPrintable(taken.error));

    auto moveTo = [&](const QString &target) {
        Job *job = moveModelsFolder(&fx.jobs, &lib, &fx.supervisor, target);
        QElapsedTimer t;
        t.start();
        while (!job->isFinished() && t.elapsed() < 30000)
            QTest::qWait(20);
        return job;
    };
    auto allInPlace = [&] {
        return readAll(from + QStringLiteral("/checkpoints/wai.safetensors")) == ckpt && readAll(from + QStringLiteral("/loras/style.safetensors")) == lora
               && readAll(from + QStringLiteral("/llm/Model-Q4_K_M.gguf")) == gguf
               && QFileInfo::exists(sidecarPath(from + QStringLiteral("/checkpoints/wai.safetensors")))
               && !QFileInfo::exists(to + QStringLiteral("/checkpoints/wai.safetensors"));
    };
    // Not while models are downloaded into the folder or a LoRA is trained (also a queued one, and in
    // the cloud: it is copied into the folder when it ends), nor while the GPU is reserved.
    Job *download = fx.jobs.submit(QStringLiteral("Download"), QStringLiteral("model-download"), [](Job *) {});
    QTRY_VERIFY(download->isActive());
    Job *refused = moveTo(to);
    QCOMPARE(refused->state(), Job::State::Failed);
    QVERIFY2(refused->errorString().contains(QStringLiteral("downloaded")), qPrintable(refused->errorString()));
    download->cancel();
    for (const QString &group : {QStringLiteral("lora-training"), QStringLiteral("cloud-training")}) {
        Job *training = fx.jobs.submit(QStringLiteral("Train"), group, [](Job *) {});
        refused = moveTo(to);
        QCOMPARE(refused->state(), Job::State::Failed);
        QVERIFY2(refused->errorString().contains(QStringLiteral("trained")), qPrintable(refused->errorString()));
        training->cancel();
    }
    QVERIFY(!lib.isMoving());
    fx.supervisor.reserveGpu(QStringLiteral("A LoRA is being trained."));
    refused = moveTo(to);
    QCOMPARE(refused->state(), Job::State::Failed);
    QVERIFY2(refused->errorString().contains(QStringLiteral("A LoRA is being trained.")), qPrintable(refused->errorString()));
    fx.supervisor.releaseGpu();
    QVERIFY(allInPlace());
    QCOMPARE(lib.defaultFolder(), from);

    QCOMPARE(await(fx.supervisor.start(running.id)), QString());
#ifdef Q_OS_WIN
    // A file in use cannot move: what moved already goes back, and the server runs again.
    {
        QFile busy(from + QStringLiteral("/loras/style.safetensors"));
        QVERIFY(busy.open(QIODevice::ReadOnly));
        Job *failed = moveTo(to);
        QCOMPARE(failed->state(), Job::State::Failed);
        QVERIFY2(failed->errorString().contains(QStringLiteral("style.safetensors")), qPrintable(failed->errorString()));
    }
    QVERIFY(allInPlace());
    QCOMPARE(lib.defaultFolder(), from);
    QCOMPARE(fx.supervisor.profile(running.id)->options.value(QStringLiteral("model")).toString(), nativeGguf);
    QTRY_COMPARE_WITH_TIMEOUT(fx.supervisor.state(running.id), ServerState::Ready, 20000);
#endif

    // The move: files with their sidecars, the profiles, the library.
    QSignalSpy folderChanged(&lib, &ModelLibrary::folderChanged);
    QSignalSpy relocated(&lib, &ModelLibrary::relocated);
    Job *job = moveTo(to);
    QVERIFY2(job->state() == Job::State::Succeeded, qPrintable(job->errorString()));
    QCOMPARE(job->result().toString(), to);
    QCOMPARE(readAll(to + QStringLiteral("/checkpoints/wai.safetensors")), ckpt);
    QCOMPARE(readSidecar(to + QStringLiteral("/checkpoints/wai.safetensors")).value(QStringLiteral("sha256")).toString(), sha256(ckpt));
    QCOMPARE(readAll(to + QStringLiteral("/loras/style.safetensors")), lora);
    QCOMPARE(readAll(to + QStringLiteral("/llm/Model-Q4_K_M.gguf")), gguf);
    QVERIFY(QFileInfo::exists(to + QStringLiteral("/loras/half.safetensors.part")));
    QVERIFY(!QFileInfo::exists(from + QStringLiteral("/checkpoints/wai.safetensors")));
    QVERIFY(!QFileInfo::exists(from + QStringLiteral("/checkpoints"))); // left empty: removed
    QCOMPARE(readAll(from + QStringLiteral("/notes.txt")), QByteArray("not a model: stays"));
    QVERIFY(QFileInfo::exists(from + QStringLiteral("/llm/chat.jinja")));
    // The added folder stays with its files and its empty sub-folders.
    QCOMPARE(readAll(comfy + QStringLiteral("/checkpoints/comfy.safetensors")), ckpt);
    QVERIFY(QFileInfo(comfy + QStringLiteral("/upscale_models")).isDir());
    QVERIFY(!QFileInfo::exists(to + QStringLiteral("/ComfyUI")));
    QVERIFY(!lib.isMoving());

    const QString movedGguf = QDir::toNativeSeparators(to + QStringLiteral("/llm/Model-Q4_K_M.gguf"));
    const QString movedLoras = QDir::toNativeSeparators(to + QStringLiteral("/loras"));
    const ServerProfile r = *fx.supervisor.profile(running.id);
    QCOMPARE(r.options.value(QStringLiteral("model")).toString(), movedGguf);
    QCOMPARE(r.options.value(QStringLiteral("extra_args")).toString(), QStringLiteral("--fake-ready-ms 100 --lora-dir \"%1\"").arg(movedLoras));
    const ServerProfile s = *fx.supervisor.profile(stopped.id);
    QCOMPARE(s.options.value(QStringLiteral("model")).toString(), fx.model);
    QCOMPARE(s.options.value(QStringLiteral("lora_dir")).toString(), movedLoras);
    QCOMPARE(s.options.value(QStringLiteral("vae")).toString(), from + QStringLiteral("2/vae.safetensors"));
    QCOMPARE(s.options.value(QStringLiteral("clip_vision")).toString(), from + QStringLiteral(" 2/clip.safetensors"));
    // What stayed keeps its path: the added folder's file, the template, a folder with a similar name.
    QCOMPARE(s.options.value(QStringLiteral("ip_adapter")).toString(), comfy + QStringLiteral("/checkpoints/comfy.safetensors"));
    QCOMPARE(s.options.value(QStringLiteral("extra_args")).toString(), siblings);
    QCOMPARE(s.options.value(QStringLiteral("more_models")).toList().value(0).toMap().value(QStringLiteral("model")).toString(),
             to + QStringLiteral("/llm/Model-Q4_K_M.gguf"));
    // The server that ran starts again with the new paths.
    QTRY_COMPARE_WITH_TIMEOUT(fx.supervisor.state(running.id), ServerState::Ready, 20000);
    const LaunchCommand launch = fx.supervisor.launchFor(running.id);
    QVERIFY2(launch.error.isEmpty(), qPrintable(launch.error));
    QVERIFY(launch.signature().contains((to + QStringLiteral("/llm/Model-Q4_K_M.gguf")).toUtf8()));
    QVERIFY(!fx.supervisor.needsRestart(running.id));
    QCOMPARE(fx.supervisor.state(stopped.id), ServerState::Stopped);

    QCOMPARE(folderChanged.size(), 1);
    QCOMPARE(relocated.size(), 1);
    QCOMPARE(relocated.first().at(0).toString(), from);
    QCOMPARE(relocated.first().at(1).toString(), to);
    QCOMPARE(lib.chosenFolder(), to);
    QCOMPARE(lib.roots().size(), 2);
    QCOMPARE(lib.roots().first().path, to);
    QVERIFY(lib.roots().first().builtin);
    QCOMPARE(lib.roots().last().path, comfy);
    QVERIFY(!lib.entry(from + QStringLiteral("/loras/style.safetensors")));
    QVERIFY(lib.entry(comfy + QStringLiteral("/checkpoints/comfy.safetensors")));
    QCOMPARE(lib.entry(to + QStringLiteral("/loras/style.safetensors"))->sha256, hash.result()); // not hashed again
    QTRY_VERIFY_WITH_TIMEOUT(!lib.isScanning(), 10000);
    QCOMPARE(lib.entries().size(), 4);
    QCOMPARE(lib.entry(to + QStringLiteral("/loras/style.safetensors"))->sha256, hash.result());
    QCOMPARE(lib.entry(to + QStringLiteral("/checkpoints/wai.safetensors"))->info.family, QStringLiteral("SDXL"));

    // Downloads land in the moved folder.
    MockHttpServer hf;
    const QByteArray lora2 = lora + "2";
    const QString revision = QStringLiteral("0123456789abcdef0123456789abcdef01234567");
    serveFile(hf, QStringLiteral("/org/repo/resolve/%1/second.safetensors").arg(revision), lora2);
    ModelHub hub(&fx.http, &fx.jobs, &lib);
    HubDownload d;
    d.source = HubSource::HuggingFace;
    d.modelId = QStringLiteral("org/repo");
    d.versionId = revision;
    d.file.path = QStringLiteral("second.safetensors");
    d.file.format = QStringLiteral("safetensors");
    d.file.kind = ModelKind::Lora;
    d.file.size = lora2.size();
    d.file.sha256 = sha256(lora2);
    d.file.url = QUrl(hf.baseUrl().toString() + QStringLiteral("/org/repo/resolve/%1/second.safetensors").arg(revision));
    QCOMPARE(hub.destinationFor(d), to + QStringLiteral("/loras/second.safetensors"));
    // Not while the folder moves.
    lib.beginMove();
    Job *waiting = hub.download(d);
    QTRY_VERIFY_WITH_TIMEOUT(waiting->isFinished(), 10000);
    QCOMPARE(waiting->state(), Job::State::Failed);
    QVERIFY2(waiting->errorString().contains(QStringLiteral("being moved")), qPrintable(waiting->errorString()));
    lib.endMove();
    QVERIFY(!QFileInfo::exists(to + QStringLiteral("/loras/second.safetensors")));
    Job *fetched = hub.download(d);
    QTRY_VERIFY_WITH_TIMEOUT(fetched->isFinished(), 20000);
    QVERIFY2(fetched->state() == Job::State::Succeeded, qPrintable(fetched->errorString()));
    QCOMPARE(fetched->result().toString(), to + QStringLiteral("/loras/second.safetensors"));
    QCOMPARE(readAll(to + QStringLiteral("/loras/second.safetensors")), lora2);

    // With nothing to move the running server is neither stopped nor waited for.
    {
        QTemporaryDir otherData;
        ModelLibrary empty(&fx.http);
        empty.setDataDir(otherData.path());
        empty.load();
        QTRY_COMPARE_WITH_TIMEOUT(fx.supervisor.state(running.id), ServerState::Ready, 20000);
        QElapsedTimer t;
        t.start();
        Job *quick = moveModelsFolder(&fx.jobs, &empty, &fx.supervisor, otherData.filePath(QStringLiteral("elsewhere")));
        QTRY_VERIFY_WITH_TIMEOUT(quick->isFinished(), 30000);
        QVERIFY2(quick->state() == Job::State::Succeeded, qPrintable(quick->errorString()));
        QVERIFY2(t.elapsed() < 5000, qPrintable(QString::number(t.elapsed())));
        QCOMPARE(fx.supervisor.state(running.id), ServerState::Ready);
        QTRY_VERIFY_WITH_TIMEOUT(!empty.isScanning(), 10000);
    }

    fx.supervisor.stop(running.id);
    QTRY_COMPARE_WITH_TIMEOUT(fx.supervisor.state(running.id), ServerState::Stopped, 10000);
}

void TestRuntime::modelsFolderCopies()
{
    // Across drives files are copied, then the originals deleted. Forced on one drive here.
    QTemporaryDir dir;
    const QString from = dir.filePath(QStringLiteral("old"));
    const QByteArray big(20 * 1024 * 1024, 'a'); // several chunks
    QVERIFY(writeFile(from + QStringLiteral("/checkpoints/big.safetensors"), big));
    QVERIFY(writeFile(sidecarPath(from + QStringLiteral("/checkpoints/big.safetensors")), "{}"));
    QVERIFY(writeFile(from + QStringLiteral("/vae/small.safetensors"), "vae"));
    const QDateTime modified = QFileInfo(from + QStringLiteral("/checkpoints/big.safetensors")).lastModified();
    QTest::qWait(1100); // a copy made now would get a newer time

    // Cancelled half-way: nothing is left in the target (neither files nor the folders the move
    // made), everything is still in place. A folder that was there already stays.
    QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("cancelled/mine"))));
    FolderMovePlan plan = planModelsFolderMove(from, dir.filePath(QStringLiteral("cancelled/models")));
    QVERIFY2(plan.ok(), qPrintable(plan.error));
    plan.sameVolume = false;
    const QString cancelled = moveModelFiles(plan, [](qint64 done, qint64) { return done < 10 * 1024 * 1024; });
    QVERIFY(!cancelled.isEmpty());
    QCOMPARE(readAll(from + QStringLiteral("/checkpoints/big.safetensors")), big);
    QVERIFY(!QFileInfo::exists(plan.to));
    QVERIFY(QFileInfo(dir.filePath(QStringLiteral("cancelled/mine"))).isDir());
    // A cancel after the last file still undoes the move: the originals are not gone yet.
    plan = planModelsFolderMove(from, dir.filePath(QStringLiteral("late")));
    QVERIFY2(plan.ok(), qPrintable(plan.error));
    QVERIFY(plan.sameVolume);
    const QString late = moveModelFiles(plan, [](qint64 done, qint64 total) { return done < total; });
    QVERIFY(!late.isEmpty());
    QCOMPARE(readAll(from + QStringLiteral("/checkpoints/big.safetensors")), big);
    QVERIFY(QFileInfo::exists(sidecarPath(from + QStringLiteral("/checkpoints/big.safetensors"))));
    QVERIFY(!QFileInfo::exists(plan.to));

    plan = planModelsFolderMove(from, dir.filePath(QStringLiteral("new")));
    QVERIFY2(plan.ok(), qPrintable(plan.error));
    plan.sameVolume = false;
    qint64 lastDone = -1;
    qint64 lastTotal = -1;
    int calls = 0;
    const QString error = moveModelFiles(plan, [&](qint64 done, qint64 total) {
        lastDone = done;
        lastTotal = total;
        ++calls;
        return true;
    });
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(readAll(plan.to + QStringLiteral("/checkpoints/big.safetensors")), big);
    QVERIFY(QFileInfo::exists(sidecarPath(plan.to + QStringLiteral("/checkpoints/big.safetensors"))));
    QCOMPARE(readAll(plan.to + QStringLiteral("/vae/small.safetensors")), QByteArray("vae"));
    // The same modification time: the library's index still knows the file.
    QCOMPARE(QFileInfo(plan.to + QStringLiteral("/checkpoints/big.safetensors")).lastModified(), modified);
    QVERIFY(!QFileInfo::exists(from)); // empty afterwards: removed
    QVERIFY(calls > 3);
    QCOMPARE(lastTotal, plan.bytes);
    QCOMPARE(lastDone, plan.bytes);
    QDirIterator temp(plan.to, {QStringLiteral("*.moving")}, QDir::Files, QDirIterator::Subdirectories);
    QVERIFY(!temp.hasNext());

    // Paths in options and text.
    const QString to = QStringLiteral("D:/Models");
    QCOMPARE(relocatePath(QStringLiteral("C:\\Old\\loras\\a.safetensors"), QStringLiteral("C:/Old"), to), QStringLiteral("D:\\Models\\loras\\a.safetensors"));
    QCOMPARE(relocatePath(QStringLiteral("C:/Old"), QStringLiteral("C:/Old"), to), to);
    QCOMPARE(relocatePath(QStringLiteral("C:/Older/a.gguf"), QStringLiteral("C:/Old"), to), QStringLiteral("C:/Older/a.gguf"));
#ifdef Q_OS_WIN
    QCOMPARE(relocatePath(QStringLiteral("c:/old/vae/x.sft"), QStringLiteral("C:/Old"), to), QStringLiteral("D:/Models/vae/x.sft"));
#endif
    QVariantMap options{{QStringLiteral("extra_args"), QStringLiteral("--a C:\\Old\\x.gguf --b=C:/Old/y --c C:/Olden --d C:/Old "
                                                                      "--e \"C:/Old/a b.gguf\",C:/Old/z --f \"C:\\Old backup\\loras\" --g C:/Old 2/x")},
                        {QStringLiteral("vae"), QStringLiteral("C:/Old 2/vae.safetensors")},
                        {QStringLiteral("threads"), 4}};
    QVERIFY(relocateOptions(options, QStringLiteral("C:/Old"), to));
    QCOMPARE(options.value(QStringLiteral("extra_args")).toString(),
             QStringLiteral("--a D:\\Models\\x.gguf --b=D:/Models/y --c C:/Olden --d D:/Models "
                            "--e \"D:/Models/a b.gguf\",D:/Models/z --f \"C:\\Old backup\\loras\" --g D:/Models 2/x"));
    // A plain option is one path: "C:/Old 2/…" is another folder.
    QCOMPARE(options.value(QStringLiteral("vae")).toString(), QStringLiteral("C:/Old 2/vae.safetensors"));
    QCOMPARE(options.value(QStringLiteral("threads")).toInt(), 4);
    QVERIFY(!relocateOptions(options, QStringLiteral("E:/Elsewhere"), to));
}

void TestRuntime::modelsFolderLinks()
{
    // Lumen's checkpoints are ComfyUI's, through a link (a junction on Windows, which needs no
    // rights). The link moves, ComfyUI's folder stays as it is.
    QTemporaryDir dir;
    const QString shared = dir.filePath(QStringLiteral("ComfyUI/models/checkpoints"));
    QVERIFY(writeFile(shared + QStringLiteral("/comfy.safetensors"), "comfy"));
    QVERIFY(QDir().mkpath(shared + QStringLiteral("/empty_sub")));
    const QString from = dir.filePath(QStringLiteral("Lumen"));
    QVERIFY(writeFile(from + QStringLiteral("/loras/a.safetensors"), "lora"));
    QVERIFY(writeFile(from + QStringLiteral("/llm/m.gguf"), "gguf"));
    QVERIFY(writeFile(from + QStringLiteral("/llm/chat.jinja"), "template"));
    const QString link = from + QStringLiteral("/checkpoints");
#ifdef Q_OS_WIN
    QCOMPARE(QProcess::execute(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
                                                             QDir::toNativeSeparators(link), QDir::toNativeSeparators(shared)}),
             0);
    QVERIFY(QFileInfo(link).isJunction());
#else
    QVERIFY(QFile::link(shared, link));
#endif
    QCOMPARE(readAll(link + QStringLiteral("/comfy.safetensors")), QByteArray("comfy"));

    const QString to = dir.filePath(QStringLiteral("New"));
    FolderMovePlan plan = planModelsFolderMove(from, to);
    QVERIFY2(plan.ok(), qPrintable(plan.error));
    QVERIFY(plan.sameVolume);
    QCOMPARE(plan.links, QStringList{QStringLiteral("checkpoints")});
    QStringList files = plan.files;
    files.sort();
    QCOMPARE(files, (QStringList{QStringLiteral("llm/m.gguf"), QStringLiteral("loras/a.safetensors")})); // nothing from inside the link
    QCOMPARE(plan.models, 3);

    // Across drives a link cannot be renamed; nothing moves.
    FolderMovePlan acrossDrives = plan;
    acrossDrives.sameVolume = false;
    const QString refused = moveModelFiles(acrossDrives);
    QVERIFY2(refused.contains(QStringLiteral("link")), qPrintable(refused));
    QCOMPARE(readAll(from + QStringLiteral("/loras/a.safetensors")), QByteArray("lora"));
    QVERIFY(!QFileInfo::exists(to));

    const QString error = moveModelFiles(plan);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QFileInfo moved(to + QStringLiteral("/checkpoints"));
    QVERIFY(moved.isJunction() || moved.isSymbolicLink());
    QVERIFY(!QFileInfo::exists(link));
    QCOMPARE(readAll(to + QStringLiteral("/checkpoints/comfy.safetensors")), QByteArray("comfy"));
    // ComfyUI's own folder: its file and its empty sub-folder are still there.
    QCOMPARE(readAll(shared + QStringLiteral("/comfy.safetensors")), QByteArray("comfy"));
    QVERIFY(QFileInfo(shared + QStringLiteral("/empty_sub")).isDir());
    QCOMPARE(readAll(to + QStringLiteral("/llm/m.gguf")), QByteArray("gguf"));
    QVERIFY(QFileInfo::exists(from + QStringLiteral("/llm/chat.jinja"))); // not a model: stays

    // Options follow what moved; a file that stayed keeps its path.
    QVariantMap options{{QStringLiteral("model"), QDir::toNativeSeparators(from + QStringLiteral("/checkpoints/comfy.safetensors"))},
                        {QStringLiteral("more_models"), QVariantList{QVariantMap{{QStringLiteral("model"), from + QStringLiteral("/llm/m.gguf")}}}},
                        {QStringLiteral("extra_args"), QStringLiteral("--chat-template-file \"%1\" --lora-dir %2")
                                                           .arg(QDir::toNativeSeparators(from + QStringLiteral("/llm/chat.jinja")), from + QStringLiteral("/loras"))}};
    QVERIFY(relocateOptions(options, from, to));
    QCOMPARE(options.value(QStringLiteral("model")).toString(), QDir::toNativeSeparators(to + QStringLiteral("/checkpoints/comfy.safetensors")));
    QCOMPARE(options.value(QStringLiteral("more_models")).toList().value(0).toMap().value(QStringLiteral("model")).toString(),
             to + QStringLiteral("/llm/m.gguf"));
    QCOMPARE(options.value(QStringLiteral("extra_args")).toString(),
             QStringLiteral("--chat-template-file \"%1\" --lora-dir %2")
                 .arg(QDir::toNativeSeparators(from + QStringLiteral("/llm/chat.jinja")), to + QStringLiteral("/loras")));
    QCOMPARE(movedPath(from + QStringLiteral("/llm/chat.jinja"), from, to), from + QStringLiteral("/llm/chat.jinja"));
    QCOMPARE(movedPath(from + QStringLiteral("/gone.safetensors"), from, to), to + QStringLiteral("/gone.safetensors")); // neither: follows
}

void TestRuntime::legacyModelsFirstStart()
{
    const QByteArray ckpt = makeSafetensors({{QStringLiteral("model.diffusion_model.input_blocks.0.0.weight"), {320, 4, 3, 3}},
                                             {QStringLiteral("cond_stage_model.transformer.text_model.x.weight"), {768}}})
                            + QByteArray(2 * 1024 * 1024, '\0');
    // An earlier version's folder: a model with its sidecar, and a note that is not a model.
    auto fillLegacy = [&](const QString &dir) {
        return writeFile(dir + QStringLiteral("/checkpoints/old.safetensors"), ckpt) && writeFile(sidecarPath(dir + QStringLiteral("/checkpoints/old.safetensors")), "{}")
               && writeFile(dir + QStringLiteral("/loras/put_loras_here"), "");
    };
    net::HttpClient http;
    JobQueue jobs;

    // Nothing to ask: no real model files, the folder is the standard one, or a folder is chosen.
    {
        QTemporaryDir data;
        QTemporaryDir legacy;
        ModelLibrary lib(&http);
        lib.setDataDir(data.path());
        lib.load();
        QVERIFY(writeFile(legacy.filePath(QStringLiteral("loras/placeholder.safetensors")), "x"));
        QVERIFY(legacyModelsToAsk(&lib, legacy.path()).isEmpty());
        QVERIFY(fillLegacy(legacy.path()));
        QCOMPARE(legacyModelsToAsk(&lib, legacy.path()), QDir::cleanPath(legacy.path()));
        QVERIFY(writeFile(lib.standardFolder() + QStringLiteral("/checkpoints/new.safetensors"), ckpt));
        QVERIFY(legacyModelsToAsk(&lib, lib.standardFolder()).isEmpty());
        lib.setFolder(data.filePath(QStringLiteral("mine")));
        QVERIFY(legacyModelsToAsk(&lib, legacy.path()).isEmpty());
        QVERIFY(!settleLegacyModels(&jobs, &lib, nullptr, QString(), [](const FolderMovePlan &) {
            [] { QFAIL("asked without a folder"); }();
            return true;
        }));
        QTRY_VERIFY_WITH_TIMEOUT(!lib.isScanning(), 10000);
    }

    // Keep: the old folder becomes the chosen one, so the question does not come again.
    {
        QTemporaryDir data;
        QTemporaryDir legacy;
        QVERIFY(fillLegacy(legacy.path()));
        ModelLibrary lib(&http);
        lib.setDataDir(data.path());
        lib.load();
        QSignalSpy chosen(&lib, &ModelLibrary::folderChanged);
        int asked = 0;
        FolderMovePlan seen;
        const QString ask = legacyModelsToAsk(&lib, legacy.path());
        QVERIFY(!settleLegacyModels(&jobs, &lib, nullptr, ask, [&](const FolderMovePlan &plan) {
            ++asked;
            seen = plan;
            return false;
        }));
        QCOMPARE(asked, 1);
        QVERIFY2(seen.ok(), qPrintable(seen.error));
        QCOMPARE(seen.from, QDir::cleanPath(legacy.path()));
        QCOMPARE(seen.to, lib.standardFolder());
        QCOMPARE(seen.models, 1);
        QCOMPARE(chosen.size(), 1);
        QCOMPARE(lib.chosenFolder(), QDir::cleanPath(legacy.path()));
        QVERIFY(legacyModelsToAsk(&lib, legacy.path()).isEmpty());
        QCOMPARE(readAll(legacy.filePath(QStringLiteral("checkpoints/old.safetensors"))), ckpt);
        QTRY_VERIFY_WITH_TIMEOUT(!lib.isScanning() && !lib.entries().isEmpty(), 10000);
        QCOMPARE(lib.entries().first().path, QDir::cleanPath(legacy.filePath(QStringLiteral("checkpoints/old.safetensors"))));
    }

    // Move: the files go to the standard folder, which then is the chosen one; profiles follow.
    {
        FakeRuntime fx;
        QTemporaryDir data;
        QTemporaryDir legacy;
        QVERIFY(fillLegacy(legacy.path()));
        ModelLibrary lib(&fx.http);
        lib.setDataDir(data.path());
        lib.load();
        ServerProfile p = fx.supervisor.createProfile(QStringLiteral("adopted/fake"), QStringLiteral("Images"));
        p.options = {{QStringLiteral("model"), QDir::toNativeSeparators(legacy.filePath(QStringLiteral("checkpoints/old.safetensors")))}};
        fx.supervisor.addProfile(p);
        QSignalSpy chosen(&lib, &ModelLibrary::folderChanged);
        Job *job = settleLegacyModels(&fx.jobs, &lib, &fx.supervisor, legacyModelsToAsk(&lib, legacy.path()), [](const FolderMovePlan &) { return true; });
        QVERIFY(job);
        QTRY_VERIFY_WITH_TIMEOUT(job->isFinished(), 30000);
        QVERIFY2(job->state() == Job::State::Succeeded, qPrintable(job->errorString()));
        QCOMPARE(chosen.size(), 2); // the old folder while asking, then the standard one again
        QVERIFY(lib.chosenFolder().isEmpty());
        QCOMPARE(lib.defaultFolder(), lib.standardFolder());
        const QString moved = lib.standardFolder() + QStringLiteral("/checkpoints/old.safetensors");
        QCOMPARE(readAll(moved), ckpt);
        QVERIFY(QFileInfo::exists(sidecarPath(moved)));
        QVERIFY(!QFileInfo::exists(legacy.filePath(QStringLiteral("checkpoints/old.safetensors"))));
        QVERIFY(QFileInfo::exists(legacy.filePath(QStringLiteral("loras/put_loras_here")))); // not a model: stays
        QCOMPARE(fx.supervisor.profile(p.id)->options.value(QStringLiteral("model")).toString(), QDir::toNativeSeparators(moved));
        QTRY_VERIFY_WITH_TIMEOUT(!lib.isScanning(), 10000);
        QCOMPARE(lib.entries().size(), 1);
        QCOMPARE(lib.entries().first().path, moved);
    }

    // A move that cannot be done (a file of that name in the new folder) keeps the old folder.
    {
        QTemporaryDir data;
        QTemporaryDir legacy;
        QVERIFY(fillLegacy(legacy.path()));
        ModelLibrary lib(&http);
        lib.setDataDir(data.path());
        lib.load();
        QVERIFY(writeFile(lib.standardFolder() + QStringLiteral("/checkpoints/old.safetensors"), "another"));
        QString why;
        QVERIFY(!settleLegacyModels(&jobs, &lib, nullptr, legacyModelsToAsk(&lib, legacy.path()), [&](const FolderMovePlan &plan) {
            why = plan.error;
            return true;
        }));
        QVERIFY2(why.contains(QStringLiteral("old.safetensors")), qPrintable(why));
        QCOMPARE(lib.chosenFolder(), QDir::cleanPath(legacy.path()));
        QCOMPARE(readAll(legacy.filePath(QStringLiteral("checkpoints/old.safetensors"))), ckpt);
        QTRY_VERIFY_WITH_TIMEOUT(!lib.isScanning(), 10000);
    }
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

void TestRuntime::hubCompanions()
{
    // A Civitai version like MiaoMiao 3D Harem (Anima): the model plus its text encoder (marked
    // required) and VAE as separate files.
    const QByteArray padding(2 * 1024 * 1024, '\0'); // the scan skips files under 1 MB
    const QByteArray model = makeSafetensors({{QStringLiteral("model.diffusion_model.input_blocks.0.0.weight"), {320, 4, 3, 3}},
                                              {QStringLiteral("model.diffusion_model.label_emb.0.0.weight"), {1280, 2816}}}) + padding;
    const QByteArray encoder = makeSafetensors({{QStringLiteral("model.layers.0.self_attn.q_proj.weight"), {1024, 1024}}}) + padding;
    const QByteArray vae = makeSafetensors({{QStringLiteral("decoder.conv_in.weight"), {512, 16, 3, 3}},
                                            {QStringLiteral("encoder.down.0.block.0.conv1.weight"), {128, 128, 3, 3}}}) + padding;
    MockHttpServer civ;
    serveFile(civ, QStringLiteral("/api/download/models/30"), model);
    serveFile(civ, QStringLiteral("/api/download/models/30/te"), encoder);
    serveFile(civ, QStringLiteral("/api/download/models/30/vae"), vae);
    auto file = [&](const char *name, const char *type, const QByteArray &bytes, const QString &url, const QJsonObject &meta) {
        return QJsonObject{{QStringLiteral("name"), QLatin1String(name)},
                           {QStringLiteral("type"), QLatin1String(type)},
                           {QStringLiteral("sizeKB"), bytes.size() / 1024.0},
                           {QStringLiteral("metadata"), meta},
                           {QStringLiteral("hashes"), QJsonObject{{QStringLiteral("SHA256"), sha256(bytes).toUpper()}}},
                           {QStringLiteral("downloadUrl"), civ.baseUrl().toString() + url}};
    };
    const QJsonObject safe{{QStringLiteral("format"), QStringLiteral("SafeTensor")}, {QStringLiteral("fp"), QStringLiteral("bf16")}};
    QJsonObject requiredMeta = safe;
    requiredMeta.insert(QStringLiteral("isRequired"), true);
    const QJsonArray versionFiles{file("harem_txt.safetensors", "Text Encoder", encoder, QStringLiteral("/api/download/models/30/te"), requiredMeta),
                                  file("harem.safetensors", "Model", model, QStringLiteral("/api/download/models/30"), safe),
                                  file("qwen_image_vae.safetensors", "VAE", vae, QStringLiteral("/api/download/models/30/vae"), safe),
                                  file("qwen_image_vae_copy.safetensors", "VAE", vae, QStringLiteral("/api/download/models/30/vae"), safe)};
    QList<HubFile> files;
    for (const QJsonValue &v : versionFiles)
        files << civitaiFile(v.toObject(), QStringLiteral("Checkpoint"));
    QCOMPARE(files[0].kind, ModelKind::TextEncoder);
    QCOMPARE(files[0].hubType, QStringLiteral("Text Encoder"));
    QVERIFY(files[0].required);
    QCOMPARE(files[1].kind, ModelKind::Checkpoint);
    QVERIFY(!files[1].required);
    QCOMPARE(files[2].kind, ModelKind::Vae);

    // The model's companions: the encoder and one VAE (the copy has the same checksum); a
    // companion has none, nor does a Hugging Face file.
    const QList<HubFile> companions = companionFiles(files, files[1]);
    QCOMPARE(companions.size(), 2);
    QCOMPARE(companions[0].path, QStringLiteral("harem_txt.safetensors"));
    QCOMPARE(companions[1].path, QStringLiteral("qwen_image_vae.safetensors"));
    QVERIFY(companionFiles(files, files[2]).isEmpty());
    HubFile hf = files[1];
    hf.hubType.clear();
    QVERIFY(companionFiles(files, hf).isEmpty());
    const HubFile roundTrip = companionFromJson(companionToJson(companions[0]));
    QCOMPARE(roundTrip.path, companions[0].path);
    QCOMPARE(roundTrip.kind, ModelKind::TextEncoder);
    QCOMPARE(roundTrip.sha256, sha256(encoder));
    QCOMPARE(roundTrip.size, companions[0].size);
    QVERIFY(roundTrip.required);

    net::HttpClient http;
    JobQueue jobs;
    QTemporaryDir data;
    ModelLibrary lib(&http);
    lib.setDataDir(data.path());
    lib.load();
    lib.setCivitaiBase(civ.baseUrl().toString());
    ModelHub hub(&http, &jobs, &lib);
    hub.setBaseUrls(civ.baseUrl().toString(), civ.baseUrl().toString());
    hub.tokenProvider = [](HubSource) { return QStringLiteral("civ_test"); };
    auto request = [&](const HubFile &f) {
        HubDownload d;
        d.source = HubSource::Civitai;
        d.modelId = QStringLiteral("3");
        d.modelName = QStringLiteral("Harem");
        d.versionId = QStringLiteral("30");
        d.versionName = QStringLiteral("v1");
        d.baseModel = QStringLiteral("Anima");
        d.license = QStringLiteral("civitai");
        d.page = QUrl(QStringLiteral("https://civitai.com/models/3"));
        d.file = f;
        return d;
    };

    // The model alone first: its companions are recorded but missing.
    HubDownload main = request(files[1]);
    main.companions = companions;
    Job *mj = hub.download(main);
    QTRY_VERIFY_WITH_TIMEOUT(mj->isFinished(), 20000);
    QVERIFY2(mj->state() == Job::State::Succeeded, qPrintable(mj->errorString()));
    const QString modelPath = mj->result().toString();
    QList<ModelCompanion> recorded = companionsOf(&lib, modelPath);
    QCOMPARE(recorded.size(), 2);
    QVERIFY(recorded[0].missing() && recorded[1].missing());
    QVERIFY(recorded[0].file.required);
    QVERIFY(companionServerOptions(&lib, modelPath).isEmpty());

    // Missing ones are fetched with the model's Civitai data; then the server options follow.
    const ModelEntry entry = *lib.entry(modelPath);
    QList<Job *> fetches;
    for (const ModelCompanion &c : recorded) {
        const HubDownload d = companionDownload(entry, c.file);
        QCOMPARE(d.source, HubSource::Civitai);
        QCOMPARE(d.modelId, QStringLiteral("3"));
        QCOMPARE(d.versionId, QStringLiteral("30"));
        QCOMPARE(d.license, QStringLiteral("civitai"));
        fetches << hub.download(d);
    }
    QTRY_VERIFY_WITH_TIMEOUT(fetches[0]->isFinished() && fetches[1]->isFinished(), 20000);
    QVERIFY2(fetches[0]->state() == Job::State::Succeeded, qPrintable(fetches[0]->errorString()));
    QVERIFY2(fetches[1]->state() == Job::State::Succeeded, qPrintable(fetches[1]->errorString()));
    QCOMPARE(QFileInfo(fetches[0]->result().toString()).absolutePath(), QDir::cleanPath(lib.defaultFolder() + QStringLiteral("/text_encoders")));
    QCOMPARE(QFileInfo(fetches[1]->result().toString()).absolutePath(), QDir::cleanPath(lib.defaultFolder() + QStringLiteral("/vae")));
    QCOMPARE(lib.entry(fetches[0]->result().toString())->info.family, QStringLiteral("LLM"));
    recorded = companionsOf(&lib, modelPath);
    QVERIFY(!recorded[0].missing() && !recorded[1].missing());
    const QVariantMap options = companionServerOptions(&lib, modelPath);
    QCOMPARE(options.value(QStringLiteral("llm")).toString(), QDir::toNativeSeparators(fetches[0]->result().toString()));
    QCOMPARE(options.value(QStringLiteral("vae")).toString(), QDir::toNativeSeparators(fetches[1]->result().toString()));

    // A model downloaded before companions were known learns them later.
    QVERIFY(recordCompanions(fetches[1]->result().toString(), {companions[0]}));
    lib.addFile(fetches[1]->result().toString());
    QTRY_COMPARE(companionsOf(&lib, fetches[1]->result().toString()).size(), 1);

    // A file that came from elsewhere (a ComfyUI folder): the Civitai lookup by checksum
    // records its version's companions; the required mark of an earlier record stays.
    QTemporaryDir comfy;
    const QString local = comfy.filePath(QStringLiteral("harem_local.safetensors"));
    {
        QFile f(local);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(model);
    }
    civ.route("GET", QStringLiteral("/api/v1/model-versions/by-hash/") + sha256(model).toUpper(), [&](const MockRequest &) {
        QJsonArray plain;
        for (const QJsonValue &v : versionFiles) {
            QJsonObject o = v.toObject();
            QJsonObject meta = o.value(QStringLiteral("metadata")).toObject();
            meta.remove(QStringLiteral("isRequired")); // this endpoint does not say
            o.insert(QStringLiteral("metadata"), meta);
            plain.append(o);
        }
        return MockResponse::json({{QStringLiteral("id"), 30}, {QStringLiteral("modelId"), 3}, {QStringLiteral("name"), QStringLiteral("v1")},
                                   {QStringLiteral("baseModel"), QStringLiteral("Anima")},
                                   {QStringLiteral("model"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Harem")}, {QStringLiteral("type"), QStringLiteral("Checkpoint")}}},
                                   {QStringLiteral("files"), plain}});
    });
    QVERIFY(lib.addRoot(comfy.path()));
    QTRY_VERIFY_WITH_TIMEOUT(lib.entry(local).has_value() && !lib.isScanning(), 20000);
    auto looked = lib.lookupCivitai(local);
    QTRY_VERIFY_WITH_TIMEOUT(looked.isFinished(), 20000);
    QCOMPARE(looked.result().value(QStringLiteral("companions")).toArray().size(), 2);
    const QList<ModelCompanion> found = companionsOf(&lib, local);
    QCOMPARE(found.size(), 2);
    QVERIFY(!found[0].file.required); // not known from this endpoint
    QVERIFY(!found[0].missing() && !found[1].missing()); // downloaded above
}

void TestRuntime::animaCompanions()
{
    // An Anima model from elsewhere, and VAEs / text encoders under other names: Lumen finds the
    // ones that fit by their headers.
    const QByteArray pad(2 * 1024 * 1024, '\0'); // the scan skips files under 1 MB
    QTemporaryDir comfy;
    auto put = [&](const QString &rel, const QByteArray &bytes) {
        const QString path = QDir::cleanPath(comfy.filePath(rel));
        writeFile(path, bytes + pad);
        return path;
    };
    const QString anima = put(QStringLiteral("checkpoints/miaomiaoHarem_29BBETA11.safetensors"),
                              makeSafetensors({{QStringLiteral("net.blocks.0.self_attn.q_proj.weight"), {2048, 2048}},
                                               {QStringLiteral("net.llm_adapter.out_proj.weight"), {1024, 1024}}}));
    const QString sdxl = put(QStringLiteral("checkpoints/wai.safetensors"),
                             makeSafetensors({{QStringLiteral("model.diffusion_model.input_blocks.0.0.weight"), {320, 4, 3, 3}},
                                              {QStringLiteral("model.diffusion_model.label_emb.0.0.weight"), {1280, 2816}},
                                              {QStringLiteral("conditioner.embedders.1.model.ln_final.weight"), {1280}}}));
    auto wanVae = makeSafetensors({{QStringLiteral("decoder.conv1.weight"), {384, 16, 3, 3, 3}},
                                   {QStringLiteral("decoder.upsamples.0.residual.2.weight"), {384, 384, 3, 3, 3}}});
    put(QStringLiteral("vae/wan_2.1_vae.safetensors"), wanVae + "a");
    const QString qwenVae = put(QStringLiteral("vae/qwen_image_vae_fp8.safetensors"), wanVae + "b");
    put(QStringLiteral("text_encoders/qwen_3_8b.safetensors"),
        makeSafetensors({{QStringLiteral("model.embed_tokens.weight"), {151936, 4096}}, {QStringLiteral("model.layers.0.self_attn.q_norm.weight"), {128}}}));

    net::HttpClient http;
    JobQueue jobs;
    QTemporaryDir data;
    ModelLibrary lib(&http);
    lib.setDataDir(data.path());
    lib.load();
    QSignalSpy scanned(&lib, &ModelLibrary::scanFinished);
    QVERIFY(lib.addRoot(comfy.path()));
    QTRY_VERIFY_WITH_TIMEOUT(scanned.size() >= 1 && !lib.isScanning(), 20000);
    QCOMPARE(lib.entry(anima)->info.family, QStringLiteral("Anima"));

    // Needed: the VAE (the Qwen-named one of the two that fit) and Qwen3 0.6B (missing: the 8B does not fit).
    QList<ModelCompanion> needs = companionsOf(&lib, anima);
    QCOMPARE(needs.size(), 2);
    QCOMPARE(needs[0].file.kind, ModelKind::Vae);
    QCOMPARE(needs[0].path, qwenVae);
    QCOMPARE(needs[1].file.kind, ModelKind::TextEncoder);
    QVERIFY(needs[1].missing());
    QVERIFY(needs[1].file.required);
    QVERIFY(companionsOf(&lib, sdxl).isEmpty());
    QVariantMap options = companionServerOptions(&lib, anima);
    QCOMPARE(options, (QVariantMap{{QStringLiteral("vae"), QDir::toNativeSeparators(qwenVae)}}));

    // The missing one comes from the pinned Hugging Face repository.
    const HubDownload d = companionDownload(*lib.entry(anima), needs[1].file);
    QCOMPARE(d.source, HubSource::HuggingFace);
    QCOMPARE(d.modelId, QStringLiteral("circlestone-labs/Anima"));
    QCOMPARE(d.versionId.size(), 40);
    QVERIFY(d.file.url.toString().startsWith(QStringLiteral("https://huggingface.co/circlestone-labs/Anima/resolve/") + d.versionId));
    QCOMPARE(d.file.sha256.size(), 64);
    QCOMPARE(d.license, QStringLiteral("circlestone-labs-non-commercial-license"));

    // Once a Qwen3 0.6B is there, both fill the server.
    const QString encoder = put(QStringLiteral("text_encoders/qwen3-0.6b.safetensors"),
                                makeSafetensors({{QStringLiteral("model.embed_tokens.weight"), {151936, 1024}},
                                                 {QStringLiteral("model.layers.0.self_attn.q_norm.weight"), {128}}}));
    lib.rescan();
    QTRY_VERIFY_WITH_TIMEOUT(scanned.size() >= 2 && !lib.isScanning(), 20000);
    options = companionServerOptions(&lib, anima);
    QCOMPARE(options.value(QStringLiteral("llm")).toString(), QDir::toNativeSeparators(encoder));
    QCOMPARE(options.value(QStringLiteral("vae")).toString(), QDir::toNativeSeparators(qwenVae));

    // An index written by an older inspector is read again (its hash stays).
    {
        QFile f(data.filePath(QStringLiteral("models.json")));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        QJsonArray models = root.value(QStringLiteral("models")).toArray();
        for (int k = 0; k < models.size(); ++k) {
            QJsonObject m = models[k].toObject();
            if (m.value(QStringLiteral("path")).toString() == encoder) {
                QJsonObject info = m.value(QStringLiteral("info")).toObject();
                info.remove(QStringLiteral("inspector"));
                info.insert(QStringLiteral("variant"), QString());
                m.insert(QStringLiteral("info"), info);
                m.insert(QStringLiteral("sha256"), QStringLiteral("feed"));
                models[k] = m;
            }
        }
        root.insert(QStringLiteral("models"), models);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(root).toJson());
    }
    ModelLibrary again(&http);
    again.setDataDir(data.path());
    again.load();
    QCOMPARE(again.entry(encoder)->info.variant, QString());
    QSignalSpy rescanned(&again, &ModelLibrary::scanFinished);
    again.rescan();
    QTRY_VERIFY_WITH_TIMEOUT(rescanned.size() >= 1 && !again.isScanning(), 20000);
    QCOMPARE(again.entry(encoder)->info.variant, QStringLiteral("Qwen3 0.6B"));
    QCOMPARE(again.entry(encoder)->sha256, QStringLiteral("feed"));
}

void TestRuntime::trainerRecipes()
{
    const TrainerCatalog &catalog = TrainerCatalog::instance();
    QCOMPARE(catalog.uvVersion, QStringLiteral("0.12.18"));
    QVERIFY(QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(catalog.uv.sha256).hasMatch());
    const TrainerRecipe *kohya = catalog.trainer(QStringLiteral("kohya"));
    QVERIFY(kohya);
    QVERIFY(QRegularExpression(QStringLiteral("^[0-9a-f]{40}$")).match(kohya->commit).hasMatch());
    QVERIFY(kohya->source.url.toString().contains(kohya->commit));
    QVERIFY(QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(kohya->source.sha256).hasMatch());
    QCOMPARE(kohya->variants.size(), 2);
    // Cloud pods (Linux): uv's Linux build and the cu128 lock compiled for Linux, same PyTorch.
    QVERIFY(catalog.uvLinux.url.toString().endsWith(QStringLiteral("uv-x86_64-unknown-linux-gnu.tar.gz")));
    QVERIFY(QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(catalog.uvLinux.sha256).hasMatch());
    const QByteArray linuxLock = trainerLinuxLock(*kohya->variant(QStringLiteral("cu128")));
    QVERIFY(linuxLock.contains("torch==2.8.0+cu128 \\"));
    QVERIFY(linuxLock.contains("triton=="));
    QVERIFY(!linuxLock.contains("colorama=="));
    QVERIFY(trainerLinuxLock(*kohya->variant(QStringLiteral("cu124"))).isEmpty());
    // Every locked package carries hashes, and PyTorch is the CUDA build of the variant.
    for (const TrainerVariant &v : kohya->variants) {
        const QString lock = QString::fromUtf8(trainerLock(v));
        QVERIFY2(!lock.isEmpty(), qPrintable(v.lock));
        QVERIFY(lock.contains(QStringLiteral("torch==%1+%2").arg(v.torch, v.id)));
        QVERIFY(lock.contains(QStringLiteral("--extra-index-url https://download.pytorch.org/whl/") + v.id));
        const QStringList lines = lock.split(QLatin1Char('\n'));
        int packages = 0;
        for (int i = 0; i < lines.size(); ++i) {
            if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9_.\\-\\[\\]]+==\\S+")).match(lines[i]).hasMatch())
                continue;
            ++packages;
            QVERIFY2(lines[i].endsWith(QLatin1Char('\\')) && lines.value(i + 1).trimmed().startsWith(QLatin1String("--hash=sha256:")), qPrintable(lines[i]));
        }
        QVERIFY(packages > 50);
    }
    // The newest CUDA build the driver supports.
    HardwareInfo hw;
    hw.nvidiaDriver = QStringLiteral("610.62");
    hw.cudaVersion = QVersionNumber(13, 3);
    QCOMPARE(recommendedTrainerVariant(*kohya, hw), QStringLiteral("cu128"));
    hw.cudaVersion = QVersionNumber(12, 6);
    QCOMPARE(recommendedTrainerVariant(*kohya, hw), QStringLiteral("cu124"));
    QString why;
    hw.cudaVersion = QVersionNumber(12, 2);
    QVERIFY(recommendedTrainerVariant(*kohya, hw, &why).isEmpty());
    QVERIFY(why.contains(QStringLiteral("12.4")));
    QVERIFY(recommendedTrainerVariant(*kohya, HardwareInfo(), &why).isEmpty());
    QVERIFY(isTrainerKind(QStringLiteral("kohya")));
    QVERIFY(!isTrainerKind(QStringLiteral("sd.cpp")));
    QCOMPARE(Catalog::instance().runtime(QStringLiteral("kohya"))->name, QStringLiteral("kohya sd-scripts"));
}

void TestRuntime::trainerAdopt()
{
    QTemporaryDir dir;
    // kohya_ss: the GUI's folder with sd-scripts inside and the venv at the top.
    QVERIFY(writeFile(dir.filePath(QStringLiteral("kohya_ss/sd-scripts/sdxl_train_network.py")), "# trainer"));
    QString error;
    QVERIFY(!detectTrainer(dir.filePath(QStringLiteral("kohya_ss")), &error));
    QVERIFY(error.contains(QStringLiteral("venv")));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("kohya_ss/venv/Scripts/python.exe")), "MZ"));
    const auto rt = detectTrainer(dir.filePath(QStringLiteral("kohya_ss")), &error);
    QVERIFY2(rt, qPrintable(error));
    QCOMPARE(rt->kind, QStringLiteral("kohya"));
    QCOMPARE(rt->settings.value(QStringLiteral("scripts")).toString(), QDir::cleanPath(dir.filePath(QStringLiteral("kohya_ss/sd-scripts"))));
    QCOMPARE(rt->executable, QDir::cleanPath(dir.filePath(QStringLiteral("kohya_ss/venv/Scripts/python.exe"))));
    QVERIFY(!detectTrainer(dir.path(), &error)); // not a trainer folder

    net::HttpClient http;
    JobQueue jobs;
    RuntimeStore store(&http, &jobs);
    store.setRoot(dir.filePath(QStringLiteral("runtimes")));
    QFuture<InstalledRuntime> adopted = store.adopt(dir.filePath(QStringLiteral("kohya_ss/sd-scripts/sdxl_train_network.py")));
    QTRY_VERIFY(adopted.isFinished());
    QCOMPARE(adopted.result().kind, QStringLiteral("kohya"));
    QCOMPARE(store.runtimes(QStringLiteral("kohya")).size(), 1);
    // The same folder again is not added twice.
    QFuture<InstalledRuntime> again = store.adopt(dir.filePath(QStringLiteral("kohya_ss")));
    QTRY_VERIFY(again.isFinished());
    QCOMPARE(store.runtimes(QStringLiteral("kohya")).size(), 1);
}

void TestRuntime::trainerInstall()
{
    MockHttpServer server;
    QTemporaryDir dir;
    const QByteArray uvZip = makeZip({{QStringLiteral("uv.exe"), "uv"}, {QStringLiteral("uvx.exe"), "uvx"}});
    const QByteArray sourceZip = makeZip({{QStringLiteral("sd-scripts-abc/sdxl_train_network.py"), "# train"},
                                          {QStringLiteral("sd-scripts-abc/library/train_util.py"), "# util"}});
    serveFile(server, QStringLiteral("/uv.zip"), uvZip);
    serveFile(server, QStringLiteral("/src.zip"), sourceZip);
    const QByteArray json = QJsonDocument(QJsonObject{
        {QStringLiteral("uv"), QJsonObject{{QStringLiteral("version"), QStringLiteral("9.9.9")},
                                          {QStringLiteral("url"), server.baseUrl().toString() + QStringLiteral("/uv.zip")},
                                          {QStringLiteral("size"), uvZip.size()},
                                          {QStringLiteral("sha256"), sha256(uvZip)}}},
        {QStringLiteral("trainers"),
         QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("kohya")},
                                {QStringLiteral("name"), QStringLiteral("kohya sd-scripts")},
                                {QStringLiteral("version"), QStringLiteral("v0.12.0")},
                                {QStringLiteral("commit"), QStringLiteral("abc")},
                                {QStringLiteral("python"), QStringLiteral("3.10.21")},
                                {QStringLiteral("source"), QJsonObject{{QStringLiteral("url"), server.baseUrl().toString() + QStringLiteral("/src.zip")},
                                                                       {QStringLiteral("size"), sourceZip.size()},
                                                                       {QStringLiteral("sha256"), sha256(sourceZip)},
                                                                       {QStringLiteral("root"), QStringLiteral("sd-scripts-abc")}}},
                                {QStringLiteral("variants"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("cu128")},
                                                                                    {QStringLiteral("label"), QStringLiteral("CUDA 12.8")},
                                                                                    {QStringLiteral("minCuda"), QStringLiteral("12.8")},
                                                                                    {QStringLiteral("torch"), QStringLiteral("2.8.0")},
                                                                                    {QStringLiteral("lock"), QStringLiteral("kohya-v0.12.0-cu128.txt")},
                                                                                    {QStringLiteral("downloadSize"), 1000}}}}}}}}).toJson();
    net::HttpClient http;
    JobQueue jobs;
    RuntimeStore store(&http, &jobs);
    store.setRoot(dir.filePath(QStringLiteral("runtimes")));
    store.setTrainerCatalog(TrainerCatalog::fromJson(json));
    // uv and Python are simulated: each command is recorded and leaves what the real one would.
    QStringList commands;
    QString gpu = QStringLiteral("True");
    store.processRunner = [&](const QString &program, const QStringList &args, const QProcessEnvironment &env, const QString &workDir,
                              std::function<void(const QString &)> line, const async::CancelToken &) {
        commands << QFileInfo(program).fileName() + QLatin1Char(' ') + args.join(QLatin1Char(' '));
        ProcessResult r;
        r.started = true;
        r.exitCode = 0;
        if (env.value(QStringLiteral("UV_PYTHON_INSTALL_DIR")).isEmpty())
            r.exitCode = 99; // uv must use Lumen's own Python folder
        if (args.value(0) == QLatin1String("venv")) {
            writeFile(args.last() + QStringLiteral("/Scripts/python.exe"), "MZ");
        } else if (args.value(0) == QLatin1String("pip")) {
            const QString lock = args.last();
            r.exitCode = QFile::exists(lock) && args.contains(QStringLiteral("--require-hashes")) ? 0 : 2;
            if (line) {
                line(QStringLiteral("Downloading torch (2.3GiB)"));
                line(QStringLiteral("Installed 68 packages in 41s"));
            }
        } else if (args.value(0) == QLatin1String("-c")) {
            // The check runs in the trainer's folder so that its library imports.
            r.exitCode = QFile::exists(workDir + QStringLiteral("/library/train_util.py")) ? 0 : 1;
            r.output = QStringLiteral("LUMEN 2.8.0+cu128 %1 12.8\n").arg(gpu);
        }
        return QtFuture::makeReadyValueFuture(r);
    };

    Job *job = store.installTrainer(QStringLiteral("kohya"), QStringLiteral("cu128"));
    QTRY_VERIFY_WITH_TIMEOUT(job->isFinished(), 20000);
    QVERIFY2(job->state() == Job::State::Succeeded, qPrintable(job->errorString()));
    QCOMPARE(job->result().toString(), QStringLiteral("kohya/v0.12.0-cu128"));
    QCOMPARE(commands.size(), 4);
    QVERIFY(commands[0].startsWith(QStringLiteral("uv.exe python install 3.10.21")));
    QVERIFY(commands[1].startsWith(QStringLiteral("uv.exe venv --python 3.10.21")));
    QVERIFY(commands[2].contains(QStringLiteral("pip install")) && commands[2].contains(QStringLiteral("--require-hashes"))
            && commands[2].contains(QStringLiteral("--no-deps")));
    QVERIFY(commands[3].startsWith(QStringLiteral("python.exe -c")));
    const auto rt = store.runtime(QStringLiteral("kohya/v0.12.0-cu128"));
    QVERIFY(rt);
    QVERIFY(rt->managed);
    QCOMPARE(rt->kind, QStringLiteral("kohya"));
    QVERIFY(QFileInfo::exists(rt->settings.value(QStringLiteral("scripts")).toString() + QStringLiteral("/sdxl_train_network.py")));
    QVERIFY(QFileInfo::exists(rt->executable));
    QVERIFY(rt->versionInfo.contains(QStringLiteral("PyTorch 2.8.0+cu128")));
    QVERIFY(QFileInfo::exists(rt->directory + QStringLiteral("/requirements.lock.txt")));
    QVERIFY(!QFileInfo::exists(rt->directory + QStringLiteral(".staging")));
    QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("runtimes/tools/uv-9.9.9/uv.exe"))));

    // A second install reuses uv; without a GPU in PyTorch the job fails with a clear message.
    commands.clear();
    gpu = QStringLiteral("False");
    Job *noGpu = store.installTrainer(QStringLiteral("kohya"), QStringLiteral("cu128"));
    QTRY_VERIFY_WITH_TIMEOUT(noGpu->isFinished(), 20000);
    QCOMPARE(noGpu->state(), Job::State::Failed);
    QVERIFY2(noGpu->errorString().contains(QStringLiteral("NVIDIA driver")), qPrintable(noGpu->errorString()));
    // Removing the runtime deletes its folder.
    QVERIFY(store.remove(QStringLiteral("kohya/v0.12.0-cu128")));
    QVERIFY(!QFileInfo::exists(rt->directory));
}

void TestRuntime::loraTraining()
{
    QCOMPARE(repeatsFor(25), 8);
    QCOMPARE(repeatsFor(8), 25);
    QCOMPARE(repeatsFor(400), 1);
    QCOMPARE(totalSteps(25, 8, 10), 2000);
    QCOMPARE(totalSteps(25, 8, 10, 2), 1000);

    InstalledRuntime trainer;
    trainer.kind = QStringLiteral("kohya");
    trainer.directory = QStringLiteral("C:/rt/kohya/v0.12.0-cu128");
    trainer.settings.insert(QStringLiteral("scripts"), QStringLiteral("C:/rt/kohya/v0.12.0-cu128/sd-scripts"));
    TrainingOptions o;
    o.checkpoint = QStringLiteral("C:/models/wai.safetensors");
    o.family = QStringLiteral("SDXL");
    o.imagesDir = QStringLiteral("C:/run/img");
    o.workDir = QStringLiteral("C:/run");
    o.outputName = QStringLiteral("lmn_mira");
    o.trigger = QStringLiteral("lmn_mira");
    o.epochs = 10;
    o.tier = VramTier::Small;
    QStringList a = trainingArguments(o, trainer);
    auto value = [&a](const char *option) { return a.value(a.indexOf(QLatin1String(option)) + 1); };
    QVERIFY(a.first().endsWith(QStringLiteral("sdxl_train_network.py")));
    QCOMPARE(value("--pretrained_model_name_or_path"), QDir::toNativeSeparators(o.checkpoint));
    QCOMPARE(value("--network_dim"), QStringLiteral("16"));
    QCOMPARE(value("--optimizer_type"), QStringLiteral("Adafactor"));
    QCOMPARE(value("--resolution"), QStringLiteral("1024,1024"));
    QCOMPARE(value("--max_train_epochs"), QStringLiteral("10"));
    for (const char *flag : {"--network_train_unet_only", "--cache_text_encoder_outputs", "--gradient_checkpointing", "--cache_latents",
                             "--full_bf16", "--save_state", "--sdpa"})
        QVERIFY2(a.contains(QLatin1String(flag)), flag);
    // Cached text-encoder outputs need fixed captions (the trainer refuses shuffling then).
    QVERIFY(!a.contains(QStringLiteral("--shuffle_caption")));
    QVERIFY(!a.contains(QStringLiteral("--resume")));
    o.tier = VramTier::Large;
    o.resumeState = QStringLiteral("C:/run/output/lmn_mira-000004-state");
    a = trainingArguments(o, trainer);
    QVERIFY(!a.contains(QStringLiteral("--network_train_unet_only")));
    QVERIFY(a.contains(QStringLiteral("--shuffle_caption")));
    QCOMPARE(value("--optimizer_type"), QStringLiteral("AdamW8bit"));
    QCOMPARE(value("--train_batch_size"), QStringLiteral("2"));
    QCOMPARE(value("--resume"), QDir::toNativeSeparators(o.resumeState));
    o.family = QStringLiteral("SD 1.5");
    a = trainingArguments(o, trainer);
    QVERIFY(a.first().endsWith(QStringLiteral("train_network.py")) && !a.first().endsWith(QStringLiteral("sdxl_train_network.py")));
    QCOMPARE(value("--resolution"), QStringLiteral("512,512"));

    // The trainer's output (tqdm with carriage returns arrives line by line).
    TrainingProgress p;
    QVERIFY(parseTrainingLine(QStringLiteral("caching latents..."), &p));
    QCOMPARE(p.phase, QStringLiteral("caching"));
    QVERIFY(parseTrainingLine(QStringLiteral("epoch 3/10"), &p));
    QCOMPARE(p.epoch, 3);
    QCOMPARE(p.epochs, 10);
    QVERIFY(parseTrainingLine(QStringLiteral("steps:  12%|█▏        | 240/2000 [12:20<1:30:28,  3.08s/it, avr_loss=0.0912]"), &p));
    QCOMPARE(p.step, 240);
    QCOMPARE(p.totalSteps, 2000);
    QCOMPARE(p.remaining, QStringLiteral("1:30:28"));
    QCOMPARE(p.secondsPerStep, 3.08);
    QCOMPARE(p.loss, 0.0912);
    QVERIFY(parseTrainingLine(QStringLiteral("steps:  50%|█████     | 8/16 [00:20<00:20,  2.50it/s]"), &p));
    QCOMPARE(p.secondsPerStep, 0.4);
    QVERIFY(parseTrainingLine(QStringLiteral("saving checkpoint: C:\\run\\output\\lmn_mira-000003.safetensors"), &p));
    QCOMPARE(p.savedFile, QStringLiteral("C:\\run\\output\\lmn_mira-000003.safetensors"));
    QVERIFY(parseTrainingLine(QStringLiteral("torch.OutOfMemoryError: CUDA out of memory. Tried to allocate 20.00 MiB"), &p));
    QVERIFY(p.error.contains(QStringLiteral("memory")));
    QVERIFY(!parseTrainingLine(QStringLiteral("INFO     loading model"), &p));

    // Outputs: one file per epoch plus the final one; the newest state for resuming.
    QTemporaryDir dir;
    for (const char *f : {"output/lmn_mira-000001.safetensors", "output/lmn_mira-000002.safetensors", "output/lmn_mira.safetensors",
                          "output/other.safetensors", "output/lmn_mira-000001-state/x", "output/lmn_mira-000002-state/x"})
        QVERIFY(writeFile(dir.filePath(QLatin1String(f)), "x"));
    const QStringList files = trainingOutputs(dir.path(), QStringLiteral("lmn_mira"));
    QCOMPARE(files.size(), 3);
    QVERIFY(files.last().endsWith(QStringLiteral("/lmn_mira.safetensors")));
    QVERIFY(latestTrainingState(dir.path()).endsWith(QStringLiteral("lmn_mira-000002-state")));

    // While training holds the GPU, servers do not start and say why.
    net::HttpClient http;
    JobQueue jobs;
    RuntimeStore store(&http, &jobs);
    store.setRoot(dir.filePath(QStringLiteral("runtimes")));
    ServerSupervisor servers(&store, &http);
    servers.setConfigPath(dir.filePath(QStringLiteral("servers.json")));
    QSignalSpy reservation(&servers, &ServerSupervisor::gpuReservationChanged);
    QVERIFY(servers.reserveGpu(QStringLiteral("training")).isEmpty());
    QCOMPARE(servers.gpuReservation(), QStringLiteral("training"));
    QFuture<QString> refused = servers.start(QStringLiteral("anything"));
    QVERIFY(refused.isFinished());
    QCOMPARE(refused.result(), QStringLiteral("training"));
    // Nor does a second training (or one during a move of the models folder): the reservation
    // belongs to the first, whose release would start the servers under the second.
    Job *second = startTraining(&store, &servers, &jobs, o);
    QTRY_VERIFY(second->isFinished());
    QCOMPARE(second->state(), Job::State::Failed);
    QVERIFY2(second->errorString().contains(QStringLiteral("cannot be trained now: training")), qPrintable(second->errorString()));
    QCOMPARE(servers.gpuReservation(), QStringLiteral("training"));
    servers.releaseGpu();
    QVERIFY(servers.gpuReservation().isEmpty());
    QCOMPARE(reservation.size(), 2);
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

void TestRuntime::runPodClient()
{
    MockHttpServer server;
    QJsonObject created;
    QByteArray auth;
    QStringList deleted;
    server.route("GET", QStringLiteral("/v2/catalog/gpus"), [&](const MockRequest &req) {
        auth = req.headers.value("authorization");
        const QJsonArray gpus{
            QJsonObject{{QStringLiteral("id"), QStringLiteral("NVIDIA GeForce RTX 4090")}, {QStringLiteral("name"), QStringLiteral("RTX 4090")},
                        {QStringLiteral("memory"), 24},
                        {QStringLiteral("price"), QJsonObject{{QStringLiteral("secure"), 0.69}, {QStringLiteral("community"), 0.34}}},
                        {QStringLiteral("maxCount"), QJsonObject{{QStringLiteral("secure"), 8}, {QStringLiteral("community"), 8}}}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("NVIDIA RTX A4000")}, {QStringLiteral("name"), QStringLiteral("RTX A4000")},
                        {QStringLiteral("memory"), 16},
                        {QStringLiteral("price"), QJsonObject{{QStringLiteral("secure"), 0.17}, {QStringLiteral("community"), 0.0}}},
                        {QStringLiteral("maxCount"), QJsonObject{{QStringLiteral("secure"), 2}, {QStringLiteral("community"), 0}}}}};
        return MockResponse::json({{QStringLiteral("gpus"), gpus}});
    });
    server.route("POST", QStringLiteral("/v2/pods"), [&](const MockRequest &req) {
        created = req.json();
        if (created.value(QStringLiteral("gpu")).toObject().value(QStringLiteral("id")).toString().isEmpty())
            return MockResponse::json({{QStringLiteral("title"), QStringLiteral("Unprocessable Entity")}, {QStringLiteral("status"), 422},
                                       {QStringLiteral("detail"), QStringLiteral("invalid request")},
                                       {QStringLiteral("errors"), QJsonArray{QStringLiteral("gpu.id is required")}}},
                                      422);
        return MockResponse::json({{QStringLiteral("id"), QStringLiteral("pod1")}, {QStringLiteral("name"), created.value(QStringLiteral("name"))},
                                   {QStringLiteral("status"), QStringLiteral("PROVISIONING")}, {QStringLiteral("cost"), 0.69}},
                                  201);
    });
    server.route("GET", QStringLiteral("/v2/pods/pod1"), [](const MockRequest &) {
        return MockResponse::json({{QStringLiteral("id"), QStringLiteral("pod1")}, {QStringLiteral("name"), QStringLiteral("lumen-train-aki")},
                                   {QStringLiteral("status"), QStringLiteral("RUNNING")}, {QStringLiteral("cost"), 0.69}});
    });
    server.route("GET", QStringLiteral("/v2/pods"), [](const MockRequest &) {
        return MockResponse::json({{QStringLiteral("pods"),
                                    QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("pod1")}, {QStringLiteral("name"), QStringLiteral("lumen-train-aki")},
                                                           {QStringLiteral("status"), QStringLiteral("RUNNING")}},
                                               QJsonObject{{QStringLiteral("id"), QStringLiteral("x")}, {QStringLiteral("name"), QStringLiteral("my-notebook")},
                                                           {QStringLiteral("status"), QStringLiteral("RUNNING")}}}}});
    });
    server.route("DELETE", QStringLiteral("/v2/pods/pod1"), [&](const MockRequest &) {
        deleted << QStringLiteral("pod1");
        MockResponse r;
        r.status = 204;
        return r;
    });
    server.route("DELETE", QStringLiteral("/v2/pods/gone"), [](const MockRequest &) {
        return MockResponse::json({{QStringLiteral("title"), QStringLiteral("Not Found")}, {QStringLiteral("status"), 404},
                                   {QStringLiteral("detail"), QStringLiteral("pod not found")}},
                                  404);
    });

    net::HttpClient http;
    RunPodClient client(&http);
    client.setBaseUrl(server.baseUrl().toString() + QStringLiteral("/v2"));
    client.keyProvider = [] { return QStringLiteral("rp_test_key"); };
    QVERIFY(client.hasKey());

    // The catalogue, cheapest Secure price first.
    auto gpus = client.gpus();
    QTRY_VERIFY(gpus.isFinished());
    QCOMPARE(auth, QByteArray("Bearer rp_test_key"));
    QCOMPARE(gpus.result().size(), 2);
    QCOMPARE(gpus.result()[0].name, QStringLiteral("RTX A4000"));
    const RunPodGpu rtx = gpus.result()[1];
    QCOMPARE(rtx.memoryGb, 24);
    QCOMPARE(rtx.price(true), 0.34);
    QVERIFY(rtx.offered(true) && !gpus.result()[0].offered(true));

    // A pod: the request body, then its status and proxy address.
    RunPodPodSpec spec;
    spec.name = QStringLiteral("lumen-train-aki");
    spec.image = QStringLiteral("runpod/pytorch:2.8.0-py3.11-cuda12.8.1-devel-ubuntu22.04");
    spec.gpuId = rtx.id;
    spec.minCudaVersion = QStringLiteral("12.8");
    spec.env.insert(QStringLiteral("LUMEN_TOKEN"), QStringLiteral("secret"));
    spec.ports = {QStringLiteral("8000/http")};
    spec.cmd = {QStringLiteral("bash"), QStringLiteral("-c"), QStringLiteral("python3 /agent.py")};
    auto pod = client.createPod(spec);
    QTRY_VERIFY(pod.isFinished());
    QCOMPARE(pod.result().id, QStringLiteral("pod1"));
    QCOMPARE(created.value(QStringLiteral("gpu")).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("NVIDIA GeForce RTX 4090"));
    QCOMPARE(created.value(QStringLiteral("gpu")).toObject().value(QStringLiteral("minCudaVersion")).toString(), QStringLiteral("12.8"));
    QCOMPARE(created.value(QStringLiteral("cloud")).toString(), QStringLiteral("SECURE"));
    QCOMPARE(created.value(QStringLiteral("ports")).toArray(), QJsonArray{QStringLiteral("8000/http")});
    QCOMPARE(created.value(QStringLiteral("env")).toObject().value(QStringLiteral("LUMEN_TOKEN")).toString(), QStringLiteral("secret"));
    QCOMPARE(created.value(QStringLiteral("cmd")).toArray().size(), 3);
    auto running = client.pod(QStringLiteral("pod1"));
    QTRY_VERIFY(running.isFinished());
    QVERIFY(running.result().isRunning());
    QCOMPARE(running.result().proxyUrl(8000), QStringLiteral("https://pod1-8000.proxy.runpod.net"));

    // Lumen's own pods only.
    auto mine = client.pods(QLatin1String(RunPodClient::kTrainingPrefix));
    QTRY_VERIFY(mine.isFinished());
    QCOMPARE(mine.result().size(), 1);
    QCOMPARE(mine.result()[0].id, QStringLiteral("pod1"));

    // Terminating; a pod that is already gone counts as terminated.
    auto t1 = client.terminate(QStringLiteral("pod1"));
    auto t2 = client.terminate(QStringLiteral("gone"));
    QTRY_VERIFY(t1.isFinished() && t2.isFinished());
    QVERIFY(!t1.isCanceled() && !t2.isCanceled());
    QCOMPARE(deleted, QStringList{QStringLiteral("pod1")});

    // RunPod's own messages: a validation problem, a refused key, no key.
    auto errorOf = [](auto future) {
        if (!QTest::qWaitFor([&] { return future.isFinished(); }, 10000))
            return QStringLiteral("timeout");
        try {
            future.waitForFinished();
        } catch (const std::exception &e) {
            return QString::fromUtf8(e.what());
        }
        return QString();
    };
    RunPodPodSpec bad = spec;
    bad.gpuId.clear();
    QCOMPARE(errorOf(client.createPod(bad)), QStringLiteral("RunPod: invalid request; gpu.id is required"));
    server.route("GET", QStringLiteral("/v2/catalog/gpus"), [](const MockRequest &) {
        return MockResponse::json({{QStringLiteral("title"), QStringLiteral("Unauthorized")}, {QStringLiteral("status"), 401}}, 401);
    });
    QVERIFY(errorOf(client.gpus()).contains(QStringLiteral("refused the API key")));
    client.keyProvider = [] { return QString(); };
    QVERIFY(!client.hasKey());
    QVERIFY(errorOf(client.gpus()).contains(QStringLiteral("No RunPod API key")));
}

namespace {

// A Python that runs scripts (not the Windows Store stub), or empty.
QString findPython()
{
    for (const QString &name : {QStringLiteral("python3"), QStringLiteral("python")}) {
        const QString exe = QStandardPaths::findExecutable(name);
        if (exe.isEmpty() || exe.contains(QLatin1String("WindowsApps"), Qt::CaseInsensitive))
            continue;
        QProcess p;
        p.start(exe, {QStringLiteral("--version")});
        if (p.waitForFinished(10000) && (p.readAllStandardOutput() + p.readAllStandardError()).startsWith("Python 3."))
            return exe;
    }
    return {};
}

// Waits for a future and returns its error message ("" when it succeeded).
template<typename F>
QString errorOf(F future)
{
    if (!QTest::qWaitFor([&] { return future.isFinished(); }, 30000))
        return QStringLiteral("timeout");
    try {
        future.waitForFinished();
    } catch (const std::exception &e) {
        return QString::fromUtf8(e.what());
    }
    return {};
}

// Stands in for sd-scripts: checks the image folder, prints like kohya (epochs, tqdm), writes a
// LoRA per epoch; "--sleep" waits to be cancelled.
const char *kFakeTrainer = R"(import os, sys, time
args = sys.argv[1:]
out = args[args.index('--output_dir') + 1]
img = args[args.index('--train_data_dir') + 1]
if '--sleep' in args:
    time.sleep(120)
print('found %d image folder' % len(os.listdir(img)), flush=True)
os.makedirs(out, exist_ok=True)
for e in (1, 2):
    print('epoch %d/2' % e, flush=True)
    print('steps: %d%%|#####     | %d/2 [00:01<00:01,  1.00s/it, avr_loss=0.0%d]' % (e * 50, e, e), flush=True)
    name = 'lmn-%06d.safetensors' % e if e < 2 else 'lmn.safetensors'
    with open(os.path.join(out, name), 'wb') as f:
        f.write(bytes([e]) * (150000 + e))
)";

} // namespace

void TestRuntime::podAgent()
{
    const QString python = findPython();
    if (python.isEmpty())
        QSKIP("no Python 3 on PATH");
    const QByteArray script = PodAgentClient::agentScript();
    QVERIFY(script.contains("Lumen Studio's agent"));
    QTemporaryDir dir;
    const QString agentPath = dir.filePath(QStringLiteral("pod_agent.py"));
    {
        QFile f(agentPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(script);
    }
    int port = 0;
    {
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        port = probe.serverPort();
    }
    QProcess agent;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LUMEN_TOKEN"), QStringLiteral("tok-123"));
    env.insert(QStringLiteral("LUMEN_PORT"), QString::number(port));
    env.insert(QStringLiteral("LUMEN_HOST"), QStringLiteral("127.0.0.1"));
    env.insert(QStringLiteral("LUMEN_WORK"), dir.filePath(QStringLiteral("work")));
    agent.setProcessEnvironment(env);
    agent.setProcessChannelMode(QProcess::MergedChannels);
    agent.start(python, {agentPath});
    QVERIFY(agent.waitForStarted(10000));
    auto stop = qScopeGuard([&] {
        if (agent.state() != QProcess::NotRunning) {
            agent.kill();
            agent.waitForFinished(5000);
        }
    });
    net::HttpClient http;
    const QString base = QStringLiteral("http://127.0.0.1:%1").arg(port);
    PodAgentClient client(&http, base, QStringLiteral("tok-123"));
    client.chunkSize = 64 * 1024;
    PodAgentStatus st;
    QVERIFY2(QTest::qWaitFor([&] {
                 auto f = client.status();
                 if (!errorOf(f).isEmpty())
                     return false;
                 st = f.result();
                 return true;
             }, 20000),
             agent.readAll().constData());
    QCOMPARE(st.setupState, QStringLiteral("waiting"));
    QCOMPARE(st.trainingState, QStringLiteral("idle"));
    // Without the pod's token nothing works.
    PodAgentClient stranger(&http, base, QStringLiteral("guess"));
    QVERIFY(errorOf(stranger.status()).contains(QStringLiteral("401")));

    // A chunked upload (300 KB in 64 KB chunks), checked by its SHA-256.
    QByteArray payload(300 * 1024, Qt::Uninitialized);
    for (int i = 0; i < payload.size(); ++i)
        payload[i] = char((i * 7919) % 251);
    const QString payloadPath = dir.filePath(QStringLiteral("payload.bin"));
    {
        QFile f(payloadPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(payload);
    }
    int chunks = 0;
    QCOMPARE(errorOf(client.upload(QStringLiteral("payload.bin"), payloadPath, [&](qint64, qint64) { ++chunks; })), QString());
    QCOMPARE(chunks, 5);
    const QString sha = QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());
    auto checked = client.check(QStringLiteral("payload.bin"), sha);
    QCOMPARE(errorOf(checked), QString());
    QVERIFY(checked.result());
    auto wrong = client.check(QStringLiteral("payload.bin"), QStringLiteral("00"));
    QCOMPARE(errorOf(wrong), QString());
    QVERIFY(!wrong.result());
    auto afterUpload = client.status();
    QCOMPARE(errorOf(afterUpload), QString());
    QCOMPARE(afterUpload.result().files.value(QStringLiteral("payload.bin")), qint64(payload.size()));

    // A file fetched by the pod from a URL, checked against its SHA-256; a wrong one fails.
    MockHttpServer web;
    serveFile(web, QStringLiteral("/model.safetensors"), payload);
    QCOMPARE(errorOf(client.fetch(QStringLiteral("model.safetensors"), web.baseUrl().toString() + QStringLiteral("/model.safetensors"), sha)),
             QString());
    QCOMPARE(errorOf(client.fetch(QStringLiteral("bad.safetensors"), web.baseUrl().toString() + QStringLiteral("/model.safetensors"),
                                  QStringLiteral("deadbeef"))),
             QString());
    QVERIFY(QTest::qWaitFor([&] {
        auto f = client.status();
        if (!errorOf(f).isEmpty())
            return false;
        const QJsonObject fetch = f.result().fetch;
        return fetch.value(QStringLiteral("model.safetensors")).toObject().value(QStringLiteral("state")).toString() == QLatin1String("done")
               && fetch.value(QStringLiteral("bad.safetensors")).toObject().value(QStringLiteral("state")).toString() == QLatin1String("failed");
    }, 20000));

    // The training set as a zip, extracted on the pod.
    const QString setDir = dir.filePath(QStringLiteral("set"));
    QDir().mkpath(setDir + QStringLiteral("/10_lmn"));
    for (const char *name : {"10_lmn/001.png", "10_lmn/001.txt"}) {
        QFile f(setDir + QLatin1Char('/') + QLatin1String(name));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
    }
    const QString zip = dir.filePath(QStringLiteral("dataset.zip"));
    QCOMPARE(writeZip(zip, setDir), QString());
    QList<ZipEntry> entries;
    QCOMPARE(listZip(zip, &entries), QString());
    QCOMPARE(entries.size(), 2);
    QCOMPARE(errorOf(client.upload(QStringLiteral("dataset.zip"), zip)), QString());
    QCOMPARE(errorOf(client.unzip(QStringLiteral("dataset.zip"), QStringLiteral("img"))), QString());

    // Training before setup is refused; setup (skipped in tests), then the fake trainer.
    const QString trainerPath = dir.filePath(QStringLiteral("fake_train.py"));
    {
        QFile f(trainerPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(kFakeTrainer);
    }
    QCOMPARE(errorOf(client.upload(QStringLiteral("fake_train.py"), trainerPath)), QString());
    const QStringList args{QStringLiteral("{work}/files/fake_train.py"), QStringLiteral("--output_dir"), QStringLiteral("{work}\\output"),
                           QStringLiteral("--train_data_dir"), QStringLiteral("{work}/img")};
    QVERIFY(errorOf(client.train(args)).contains(QStringLiteral("not set up")));
    QCOMPARE(errorOf(client.setup({{QStringLiteral("skip"), true}})), QString());
    QVERIFY(QTest::qWaitFor([&] {
        auto f = client.status();
        return errorOf(f).isEmpty() && f.result().setupState == QLatin1String("ready");
    }, 10000));
    QCOMPARE(errorOf(client.train(args)), QString());
    QVERIFY(QTest::qWaitFor([&] {
        auto f = client.status();
        return errorOf(f).isEmpty() && f.result().trainingState == QLatin1String("done");
    }, 30000));
    auto logged = client.log(0);
    QCOMPARE(errorOf(logged), QString());
    const QString log = QString::fromUtf8(logged.result());
    QVERIFY2(log.contains(QStringLiteral("found 1 image folder")), qPrintable(log));
    QVERIFY(log.contains(QStringLiteral("epoch 2/2")));
    QVERIFY(log.contains(QStringLiteral("avr_loss=0.02")));
    auto tail = client.log(logged.result().size());
    QCOMPARE(errorOf(tail), QString());
    QVERIFY(tail.result().isEmpty());

    // The epochs come back in chunks, checked by their SHA-256.
    auto outputs = client.outputs();
    QCOMPARE(errorOf(outputs), QString());
    QCOMPARE(outputs.result().size(), 2);
    for (const PodOutput &o : outputs.result()) {
        const QString local = dir.filePath(QStringLiteral("down/") + QFileInfo(o.path).fileName());
        QCOMPARE(errorOf(client.download(o, local)), QString());
        QFile f(local);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray got = f.readAll();
        QCOMPARE(qint64(got.size()), o.size);
        QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(got, QCryptographicHash::Sha256).toHex()), o.sha256);
    }
    PodOutput tampered = outputs.result().first();
    tampered.sha256 = QStringLiteral("00");
    QVERIFY(errorOf(client.download(tampered, dir.filePath(QStringLiteral("down/bad.safetensors")))).contains(QStringLiteral("checksum")));
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("down/bad.safetensors"))));
    // Paths outside the work folder are refused.
    PodOutput escape{QStringLiteral("../pod_agent.py"), 10, QString()};
    QVERIFY(!errorOf(client.download(escape, dir.filePath(QStringLiteral("down/x")))).isEmpty());

    // A running training can be stopped.
    QCOMPARE(errorOf(client.train(args + QStringList{QStringLiteral("--sleep")})), QString());
    QVERIFY(QTest::qWaitFor([&] {
        auto f = client.status();
        return errorOf(f).isEmpty() && f.result().trainingState == QLatin1String("running");
    }, 10000));
    QCOMPARE(errorOf(client.cancelTraining()), QString());
    QVERIFY(QTest::qWaitFor([&] {
        auto f = client.status();
        return errorOf(f).isEmpty() && f.result().trainingState == QLatin1String("cancelled");
    }, 10000));

    // Shutdown ends the agent.
    QCOMPARE(errorOf(client.shutdown()), QString());
    QVERIFY(agent.waitForFinished(10000));
}

namespace {

// Stands in for sdxl_train_network.py on the "pod": checks its inputs, prints like kohya and
// writes one LoRA per epoch. FAKE_SLEEP makes it wait (to be cancelled).
const char *kFakeSdScripts = R"(import os, sys, time
a = sys.argv[1:]
def arg(name):
    return a[a.index(name) + 1]
model = arg('--pretrained_model_name_or_path')
img = arg('--train_data_dir')
out = arg('--output_dir')
name = arg('--output_name')
epochs = int(arg('--max_train_epochs'))
print('model %d bytes, %d image folder(s)' % (os.path.getsize(model), len(os.listdir(img))), flush=True)
if os.environ.get('FAKE_SLEEP'):
    print('epoch 1/%d' % epochs, flush=True)
    time.sleep(120)
os.makedirs(out, exist_ok=True)
for e in range(1, epochs + 1):
    print('epoch %d/%d' % (e, epochs), flush=True)
    print('steps: %d%%|#####     | %d/%d [00:01<00:01,  2.00s/it, avr_loss=0.0%d]' % (100 * e // epochs, e, epochs, e), flush=True)
    path = os.path.join(out, '%s-%06d.safetensors' % (name, e) if e < epochs else name + '.safetensors')
    with open(path, 'wb') as f:
        f.write(bytes([e]) * 4096)
    print('saving checkpoint: ' + path, flush=True)
)";

// A fake RunPod: creating a pod starts the real agent here, with the token and script the
// request carries.
struct FakeRunPod
{
    MockHttpServer api;
    QString python;
    QTemporaryDir dir;
    QProcess agent;
    int port = 0;
    QJsonObject created;
    int polls = 0;
    QStringList deleted;
    bool slowTrainer = false;

    FakeRunPod(const QString &py)
        : python(py)
    {
        api.route("POST", QStringLiteral("/v2/pods"), [this](const MockRequest &req) {
            created = req.json();
            const QJsonObject env = created.value(QStringLiteral("env")).toObject();
            const QString script = dir.filePath(QStringLiteral("agent.py"));
            QFile f(script);
            if (f.open(QIODevice::WriteOnly))
                f.write(QByteArray::fromBase64(env.value(QStringLiteral("LUMEN_AGENT")).toString().toLatin1()));
            f.close();
            QTcpServer probe;
            probe.listen(QHostAddress::LocalHost, 0);
            port = probe.serverPort();
            probe.close();
            QProcessEnvironment pe = QProcessEnvironment::systemEnvironment();
            pe.insert(QStringLiteral("LUMEN_TOKEN"), env.value(QStringLiteral("LUMEN_TOKEN")).toString());
            pe.insert(QStringLiteral("LUMEN_PORT"), QString::number(port));
            pe.insert(QStringLiteral("LUMEN_HOST"), QStringLiteral("127.0.0.1"));
            pe.insert(QStringLiteral("LUMEN_WORK"), dir.filePath(QStringLiteral("work%1").arg(port)));
            if (slowTrainer)
                pe.insert(QStringLiteral("FAKE_SLEEP"), QStringLiteral("1"));
            agent.setProcessEnvironment(pe);
            agent.setProcessChannelMode(QProcess::MergedChannels);
            agent.start(python, {script});
            agent.waitForStarted(10000);
            polls = 0;
            return MockResponse::json({{QStringLiteral("id"), QStringLiteral("pod9")}, {QStringLiteral("name"), created.value(QStringLiteral("name"))},
                                       {QStringLiteral("status"), QStringLiteral("PROVISIONING")}, {QStringLiteral("cost"), 0.69}},
                                      201);
        });
        api.route("GET", QStringLiteral("/v2/pods/pod9"), [this](const MockRequest &) {
            return MockResponse::json({{QStringLiteral("id"), QStringLiteral("pod9")},
                                       {QStringLiteral("status"), ++polls < 2 ? QStringLiteral("STARTING") : QStringLiteral("RUNNING")},
                                       {QStringLiteral("cost"), 0.69}});
        });
        api.route("DELETE", QStringLiteral("/v2/pods/pod9"), [this](const MockRequest &) {
            deleted << QStringLiteral("pod9");
            agent.kill();
            MockResponse r;
            r.status = 204;
            return r;
        });
    }
    ~FakeRunPod()
    {
        if (agent.state() != QProcess::NotRunning) {
            agent.kill();
            agent.waitForFinished(5000);
        }
    }
};

} // namespace

void TestRuntime::cloudTraining()
{
    const QString python = findPython();
    if (python.isEmpty())
        QSKIP("no Python 3 on PATH");
    QTemporaryDir dir;
    // The training set ("<repeats>_<trigger>"), a base model, the fake trainer.
    QDir().mkpath(dir.filePath(QStringLiteral("img/10_lmn")));
    for (const char *name : {"img/10_lmn/001.png", "img/10_lmn/001.txt", "img/10_lmn/002.png", "img/10_lmn/002.txt"}) {
        QFile f(dir.filePath(QLatin1String(name)));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("data");
    }
    const QByteArray model(300 * 1024, 'm');
    const QString modelPath = dir.filePath(QStringLiteral("xl.safetensors"));
    {
        QFile f(modelPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(model);
    }
    const QString trainer = dir.filePath(QStringLiteral("fake_sd_scripts.py"));
    {
        QFile f(trainer);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(kFakeSdScripts);
    }
    net::HttpClient http;
    JobQueue jobs;
    FakeRunPod runpod(python);
    RunPodClient client(&http);
    client.setBaseUrl(runpod.api.baseUrl().toString() + QStringLiteral("/v2"));
    client.keyProvider = [] { return QStringLiteral("rp_test"); };

    auto options = [&](const QString &work) {
        CloudTrainingOptions o;
        o.training.checkpoint = modelPath;
        o.training.family = QStringLiteral("SDXL");
        o.training.imagesDir = dir.filePath(QStringLiteral("img"));
        o.training.workDir = dir.filePath(work);
        o.training.outputName = QStringLiteral("lmn");
        o.training.trigger = QStringLiteral("lmn");
        o.training.epochs = 3;
        o.training.tier = VramTier::Large;
        o.gpuId = QStringLiteral("NVIDIA GeForce RTX 4090");
        o.testTrainer = trainer;
        o.pollMs = 100;
        o.startTimeoutSec = 30;
        o.agentUrl = [&runpod](const RunPodPod &) { return QStringLiteral("http://127.0.0.1:%1").arg(runpod.port); };
        return o;
    };

    // A whole run: the pod gets the agent, trains, the epochs come home, the pod is terminated.
    Job *job = startCloudTraining(&client, &http, &jobs, options(QStringLiteral("run1")));
    QStringList stages;
    connect(job, &Job::progressChanged, job, [&stages](double, const QString &s) {
        if (stages.isEmpty() || stages.last() != s)
            stages << s;
    });
    QTRY_VERIFY_WITH_TIMEOUT(job->isFinished(), 60000);
    QVERIFY2(job->state() == Job::State::Succeeded, qPrintable(job->errorString() + QStringLiteral("\n") + QString::fromUtf8(runpod.agent.readAll())));
    // The pod: Lumen's name, the GPU, a token and the agent in its environment, port 8000.
    QVERIFY(runpod.created.value(QStringLiteral("name")).toString().startsWith(QStringLiteral("lumen-train-lmn-")));
    QCOMPARE(runpod.created.value(QStringLiteral("gpu")).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("NVIDIA GeForce RTX 4090"));
    QCOMPARE(runpod.created.value(QStringLiteral("ports")).toArray(), QJsonArray{QStringLiteral("8000/http")});
    QCOMPARE(runpod.created.value(QStringLiteral("env")).toObject().value(QStringLiteral("LUMEN_TOKEN")).toString().size(), 48);
    QCOMPARE(runpod.deleted, QStringList{QStringLiteral("pod9")});
    QVERIFY(activeCloudPods().isEmpty());
    const QVariantMap result = job->result().toMap();
    QCOMPARE(result.value(QStringLiteral("pod")).toString(), QStringLiteral("pod9"));
    QCOMPARE(result.value(QStringLiteral("files")).toStringList().size(), 3);
    const QString final = result.value(QStringLiteral("final")).toString();
    QVERIFY(final.endsWith(QStringLiteral("run1/output/lmn.safetensors")));
    QFile finalFile(final);
    QVERIFY(finalFile.open(QIODevice::ReadOnly));
    QCOMPARE(finalFile.readAll(), QByteArray(4096, char(3)));
    // The trainer saw the uploaded model and the unpacked set; the log came along.
    QFile log(result.value(QStringLiteral("log")).toString());
    QVERIFY(log.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(log.readAll());
    QVERIFY2(text.contains(QStringLiteral("model 307200 bytes, 1 image folder(s)")), qPrintable(text));
    QVERIFY(text.contains(QStringLiteral("epoch 3/3")));
    QCOMPARE(job->metadata().value(QStringLiteral("totalSteps")).toInt(), 3);
    QVERIFY(stages.contains(QStringLiteral("Renting the cloud GPU…")));
    QVERIFY(std::any_of(stages.begin(), stages.end(), [](const QString &s) { return s.startsWith(QStringLiteral("Epoch 3 of 3")); }));

    // Cancelled while training: the pod is terminated, nothing is kept running.
    runpod.deleted.clear();
    runpod.slowTrainer = true;
    Job *cancelled = startCloudTraining(&client, &http, &jobs, options(QStringLiteral("run2")));
    QTRY_VERIFY_WITH_TIMEOUT(cancelled->metadata().value(QStringLiteral("stage")).toString().startsWith(QStringLiteral("Starting the trainer")), 60000);
    QTest::qWait(500);
    cancelled->cancel();
    QTRY_VERIFY_WITH_TIMEOUT(cancelled->isFinished(), 30000);
    QCOMPARE(cancelled->state(), Job::State::Cancelled);
    QCOMPARE(runpod.deleted, QStringList{QStringLiteral("pod9")});
    QVERIFY(activeCloudPods().isEmpty());

    // A base model the pod cannot get (wrong checksum): a clear failure, and the pod is gone.
    runpod.deleted.clear();
    runpod.slowTrainer = false;
    MockHttpServer web;
    serveFile(web, QStringLiteral("/xl.safetensors"), model);
    CloudTrainingOptions bad = options(QStringLiteral("run3"));
    bad.checkpointUrl = web.baseUrl().toString() + QStringLiteral("/xl.safetensors");
    bad.checkpointSha256 = QStringLiteral("0000");
    Job *failed = startCloudTraining(&client, &http, &jobs, bad);
    QTRY_VERIFY_WITH_TIMEOUT(failed->isFinished(), 60000);
    QCOMPARE(failed->state(), Job::State::Failed);
    QVERIFY2(failed->errorString().contains(QStringLiteral("could not download the base model")), qPrintable(failed->errorString()));
    QCOMPARE(runpod.deleted, QStringList{QStringLiteral("pod9")});
    // The same with the right checksum works, fetched by the pod instead of uploaded.
    runpod.deleted.clear();
    CloudTrainingOptions good = bad;
    good.training.workDir = dir.filePath(QStringLiteral("run4"));
    good.checkpointSha256 = QString::fromLatin1(QCryptographicHash::hash(model, QCryptographicHash::Sha256).toHex());
    Job *fetched = startCloudTraining(&client, &http, &jobs, good);
    QTRY_VERIFY_WITH_TIMEOUT(fetched->isFinished(), 60000);
    QVERIFY2(fetched->state() == Job::State::Succeeded, qPrintable(fetched->errorString()));
    QCOMPARE(runpod.deleted, QStringList{QStringLiteral("pod9")});

    // Leftovers: Lumen's pods nothing here trains on (a stopped one too: it keeps its disk);
    // other pods of the account are not Lumen's business.
    runpod.api.route("GET", QStringLiteral("/v2/pods"), [](const MockRequest &) {
        auto pod = [](const char *id, const char *name, const char *status) {
            return QJsonObject{{QStringLiteral("id"), QLatin1String(id)}, {QStringLiteral("name"), QLatin1String(name)},
                               {QStringLiteral("status"), QLatin1String(status)}};
        };
        return MockResponse::json({{QStringLiteral("pods"), QJsonArray{pod("a", "lumen-train-aki-20261011-1200", "RUNNING"),
                                                                       pod("b", "lumen-train-bo-20261010-0900", "EXITED"),
                                                                       pod("c", "lumen-train-old", "TERMINATED"),
                                                                       pod("d", "my-notebook", "RUNNING")}}});
    });
    auto leftovers = leftoverCloudPods(&client);
    QTRY_VERIFY(leftovers.isFinished());
    QCOMPARE(leftovers.result().size(), 2);
    QCOMPARE(leftovers.result()[0].id, QStringLiteral("a"));
    QCOMPARE(leftovers.result()[1].id, QStringLiteral("b"));
}

namespace {

// Headers of the model families the presets know, as small safetensors files.
QByteArray flux2Klein(int textWidth)
{
    return makeSafetensors({{QStringLiteral("img_in.weight"), {3072, 128}},
                            {QStringLiteral("txt_in.weight"), {3072, textWidth}},
                            {QStringLiteral("double_stream_modulation_img.lin.weight"), {18432, 3072}},
                            {QStringLiteral("double_blocks.0.img_attn.qkv.weight"), {9216, 3072}},
                            {QStringLiteral("single_blocks.0.linear1.weight"), {27648, 3072}}});
}

QByteArray flux1(bool dev)
{
    QList<QPair<QString, QList<int>>> t = {{QStringLiteral("img_in.weight"), {3072, 64}},
                                           {QStringLiteral("txt_in.weight"), {3072, 4096}},
                                           {QStringLiteral("double_blocks.0.img_attn.qkv.weight"), {9216, 3072}}};
    if (dev)
        t.append({QStringLiteral("guidance_in.in_layer.weight"), {3072, 256}});
    return makeSafetensors(t);
}

QByteArray vae(int channels)
{
    return makeSafetensors({{QStringLiteral("encoder.down_blocks.0.resnets.0.conv1.weight"), {128, 128, 3, 3}},
                            {QStringLiteral("decoder.conv_in.weight"), {512, channels, 3, 3}},
                            {QStringLiteral("decoder.up_blocks.0.resnets.0.conv1.weight"), {512, 512, 3, 3}}});
}

QByteArray qwen3(int width)
{
    return makeSafetensors({{QStringLiteral("model.embed_tokens.weight"), {151936, width}}, {QStringLiteral("model.layers.0.self_attn.q_norm.weight"), {128}}});
}

QByteArray clip(int width)
{
    return makeSafetensors({{QStringLiteral("text_model.embeddings.token_embedding.weight"), {49408, width}},
                            {QStringLiteral("text_model.encoder.layers.0.mlp.fc1.weight"), {width * 4, width}}});
}

QByteArray t5()
{
    return makeSafetensors({{QStringLiteral("encoder.block.0.layer.0.SelfAttention.q.weight"), {4096, 4096}}, {QStringLiteral("shared.weight"), {32128, 4096}}});
}

QByteArray sdxlCheckpoint()
{
    return makeSafetensors({{QStringLiteral("model.diffusion_model.input_blocks.0.0.weight"), {320, 4, 3, 3}},
                            {QStringLiteral("model.diffusion_model.label_emb.0.0.weight"), {1280, 2816}},
                            {QStringLiteral("conditioner.embedders.1.model.ln_final.weight"), {1280}}});
}

QByteArray sdxlControlNet()
{
    return makeSafetensors({{QStringLiteral("control_model.input_blocks.0.0.weight"), {320, 4, 3, 3}},
                            {QStringLiteral("control_model.label_emb.0.0.weight"), {1280, 2816}},
                            {QStringLiteral("control_model.input_hint_block.0.weight"), {16, 3, 3, 3}}});
}

QByteArray sdxlIpAdapter()
{
    return makeSafetensors({{QStringLiteral("image_proj.proj.weight"), {8192, 1024}}, {QStringLiteral("ip_adapter.1.to_k_ip.weight"), {640, 2048}}});
}

QByteArray anima()
{
    return makeSafetensors({{QStringLiteral("net.blocks.0.self_attn.q_proj.weight"), {2048, 2048}},
                            {QStringLiteral("net.llm_adapter.out_proj.weight"), {1024, 1024}}});
}

QStringList messages(const QList<ProfileProblem> &problems)
{
    QStringList out;
    for (const ProfileProblem &p : problems)
        out << QStringLiteral("%1: %2").arg(p.option, p.message);
    return out;
}

} // namespace

// The bundled presets load, every file is pinned, and model headers pick the right preset.
void TestRuntime::modelPresets()
{
    // Every family file parses without a problem.
    QDirIterator it(QStringLiteral(":/presets/models"), {QStringLiteral("*.json")}, QDir::Files);
    int files = 0;
    while (it.hasNext()) {
        QFile f(it.next());
        QVERIFY(f.open(QIODevice::ReadOnly));
        QString error;
        QVERIFY2(!parseModelPresets(f.readAll(), QFileInfo(f.fileName()).completeBaseName(), &error).isEmpty() && error.isEmpty(),
                 qPrintable(f.fileName() + QStringLiteral(": ") + error));
        ++files;
    }
    QCOMPARE(files, 5);
    const QList<ModelPreset> &presets = lumen::runtime::modelPresets();
    QStringList ids;
    for (const ModelPreset &preset : presets) {
        QVERIFY2(!ids.contains(preset.id), qPrintable(preset.id));
        ids << preset.id;
        QVERIFY(preset.docs.toString().startsWith(QLatin1String("https://github.com/leejet/stable-diffusion.cpp/blob/")));
        for (const PresetSlot &slot : preset.components) {
            const HubFile f = slot.hubFile();
            QVERIFY2(f.url.toString().startsWith(QStringLiteral("https://huggingface.co/%1/resolve/%2/").arg(slot.file.repo, slot.file.revision)),
                     qPrintable(f.url.toString()));
            QCOMPARE(f.sha256.size(), 64);
            QVERIFY(f.size > 0);
            QVERIFY(f.required);
        }
    }
    for (const QString &id : {QStringLiteral("flux2-klein-4b"), QStringLiteral("flux2-klein-base-4b"), QStringLiteral("flux2-klein-9b"),
                              QStringLiteral("flux1-dev"), QStringLiteral("flux1-schnell"), QStringLiteral("flux1-kontext-dev"),
                              QStringLiteral("qwen-image-edit"), QStringLiteral("anima"), QStringLiteral("illustrious"), QStringLiteral("sdxl"),
                              QStringLiteral("sd15")})
        QVERIFY2(ids.contains(id), qPrintable(id));

    // A broken file names its problem.
    QString error;
    QVERIFY(parseModelPresets(R"({"version": 1, "files": {"x": {"repo": "a/b"}}, "presets": []})", QStringLiteral("x"), &error).isEmpty());
    QVERIFY2(error.contains(QLatin1String("revision")), qPrintable(error));

    auto inspect = [this](const QString &name, const QByteArray &bytes) {
        const QString path = m_tmp.filePath(QStringLiteral("presets/") + name);
        writeFile(path, bytes);
        return std::pair{inspectModel(path), path};
    };
    auto presetOf = [&](const QString &name, const QByteArray &bytes) {
        const auto [info, path] = inspect(name, bytes);
        const ModelPreset *p = presetFor(info, path);
        return p ? p->id : QString();
    };

    // FLUX.2: 128 input channels; the text width tells klein 4B from 9B.
    auto [klein, kleinPath] = inspect(QStringLiteral("flux-2-klein-4b-fp8.safetensors"), flux2Klein(7680));
    QCOMPARE(klein.kind, ModelKind::DiffusionModel);
    QCOMPARE(klein.family, QStringLiteral("FLUX.2"));
    QCOMPARE(klein.variant, QStringLiteral("Klein 4B"));
    QCOMPARE(presetOf(QStringLiteral("flux-2-klein-4b-fp8.safetensors"), flux2Klein(7680)), QStringLiteral("flux2-klein-4b"));
    QCOMPARE(presetOf(QStringLiteral("flux-2-klein-base-4b.safetensors"), flux2Klein(7680)), QStringLiteral("flux2-klein-base-4b"));
    QCOMPARE(presetOf(QStringLiteral("flux-2-klein-9b.safetensors"), flux2Klein(12288)), QStringLiteral("flux2-klein-9b"));
    // The same from a GGUF file, whose shapes are stored innermost first.
    auto [gguf, ggufPath] = inspect(QStringLiteral("unslothFLUX2Klein4BGGUF_q80.gguf"),
                                    makeGguf(QString(), 8,
                                             {{QStringLiteral("img_in.weight"), {128, 3072}},
                                              {QStringLiteral("txt_in.weight"), {7680, 3072}},
                                              {QStringLiteral("double_blocks.0.img_attn.qkv.weight"), {3072, 9216}}},
                                             false));
    QCOMPARE(gguf.family, QStringLiteral("FLUX.2"));
    QCOMPARE(gguf.variant, QStringLiteral("Klein 4B"));
    QCOMPARE(presetFor(gguf, ggufPath)->id, QStringLiteral("flux2-klein-4b"));
    const ModelPreset *kleinPreset = presetFor(klein, kleinPath);
    QCOMPARE(kleinPreset->cfg, 1.0);
    QCOMPARE(kleinPreset->steps, 4);
    QVERIFY(kleinPreset->edit);
    QVERIFY(!kleinPreset->negativePrompt);
    QVERIFY(kleinPreset->controlNetFamilies.isEmpty());

    // FLUX.1: dev has a guidance embedding, schnell has not; Kontext by its name.
    QCOMPARE(inspect(QStringLiteral("flux1-dev.safetensors"), flux1(true)).first.variant, QStringLiteral("Dev"));
    QCOMPARE(presetOf(QStringLiteral("flux1-dev.safetensors"), flux1(true)), QStringLiteral("flux1-dev"));
    QCOMPARE(presetOf(QStringLiteral("flux1-schnell.safetensors"), flux1(false)), QStringLiteral("flux1-schnell"));
    QCOMPARE(presetOf(QStringLiteral("flux1-kontext-dev.safetensors"), flux1(true)), QStringLiteral("flux1-kontext-dev"));
    // Text encoders: CLIP-L and CLIP-G by width.
    QCOMPARE(inspect(QStringLiteral("clip_l.safetensors"), clip(768)).first.variant, QStringLiteral("CLIP-L"));
    QCOMPARE(inspect(QStringLiteral("clip_g.safetensors"), clip(1280)).first.variant, QStringLiteral("CLIP-G"));
    // SDXL fine-tunes keep their name hints; plain files are SDXL.
    QCOMPARE(presetOf(QStringLiteral("waiIllustriousSDXL_v170.safetensors"), sdxlCheckpoint()), QStringLiteral("illustrious"));
    QCOMPARE(presetOf(QStringLiteral("juggernautXL.safetensors"), sdxlCheckpoint()), QStringLiteral("sdxl"));
    QCOMPARE(presetOf(QStringLiteral("miaomiao3DHarem_animaLH3D10.safetensors"), anima()), QStringLiteral("anima"));
}

// The model types of the server dialog (plan §17.5): the presets grouped by the fields and files
// they need, named in the family files, sorted by name.
void TestRuntime::modelTypes()
{
    QStringList names;
    for (const ModelType &t : lumen::runtime::modelTypes())
        names << t.name;
    QCOMPARE(names, (QStringList{QStringLiteral("Anima"), QStringLiteral("FLUX.1 (dev, schnell, Kontext; separate files)"),
                                 QStringLiteral("FLUX.1 (single file)"), QStringLiteral("FLUX.2 [klein] 4B"), QStringLiteral("FLUX.2 [klein] 9B"),
                                 QStringLiteral("Qwen-Image / Qwen-Image-Edit"), QStringLiteral("SDXL (also Illustrious, NoobAI, Pony)"),
                                 QStringLiteral("Stable Diffusion 1.5")}));
    // Every preset has a type, and the presets of one type need the same files and take the same
    // add-ons.
    for (const ModelPreset &p : lumen::runtime::modelPresets()) {
        const ModelType *t = modelTypeOf(p);
        QVERIFY2(t && t->contains(p), qPrintable(p.id));
        QCOMPARE(p.components.size(), t->components.size());
        for (int i = 0; i < p.components.size(); ++i) {
            QCOMPARE(p.components[i].option, t->components[i].option);
            QCOMPARE(p.components[i].kind, t->components[i].kind);
            QCOMPARE(p.components[i].family, t->components[i].family);
            QCOMPARE(p.components[i].variant, t->components[i].variant);
        }
        QCOMPARE(p.controlNetFamilies, t->controlNetFamilies);
        QCOMPARE(p.ipAdapterFamilies, t->ipAdapterFamilies);
        QCOMPARE(p.optionalVae, t->optionalVae);
    }
    auto typeOf = [](const char *preset) { return modelTypeOf(*presetById(QLatin1String(preset)))->id; };
    QCOMPARE(typeOf("flux2-klein-base-4b"), QStringLiteral("flux2-klein-4b"));
    QCOMPARE(typeOf("flux2-klein-base-9b"), QStringLiteral("flux2-klein-9b"));
    QCOMPARE(typeOf("flux1-kontext-dev"), QStringLiteral("flux1"));
    QCOMPARE(typeOf("flux1-schnell"), QStringLiteral("flux1"));
    QCOMPARE(typeOf("flux1-checkpoint"), QStringLiteral("flux1-checkpoint"));
    QCOMPARE(typeOf("qwen-image-edit"), QStringLiteral("qwen-image"));
    QCOMPARE(typeOf("pony"), QStringLiteral("sdxl"));
    QCOMPARE(typeOf("illustrious"), QStringLiteral("sdxl"));
    QCOMPARE(typeOf("sd15"), QStringLiteral("sd15"));

    // The fields of each type: the model in its slot, its components, the add-ons it can use.
    auto fields = [](const char *type) {
        const ModelType *t = modelTypeById(QLatin1String(type));
        return t ? t->fileOptions(t->modelOption()).join(QLatin1Char(' ')) : QString();
    };
    QCOMPARE(fields("flux1"), QStringLiteral("diffusion_model vae clip_l t5xxl lora_dir upscalers_dir"));
    QCOMPARE(fields("flux1-checkpoint"), QStringLiteral("model vae lora_dir upscalers_dir"));
    QCOMPARE(fields("flux2-klein-4b"), QStringLiteral("diffusion_model vae llm lora_dir upscalers_dir"));
    QCOMPARE(fields("flux2-klein-9b"), QStringLiteral("diffusion_model vae llm lora_dir upscalers_dir"));
    QCOMPARE(fields("qwen-image"), QStringLiteral("diffusion_model vae llm lora_dir upscalers_dir"));
    QCOMPARE(fields("anima"), QStringLiteral("diffusion_model vae llm lora_dir upscalers_dir"));
    QCOMPARE(fields("sdxl"), QStringLiteral("model vae control_net ip_adapter clip_vision lora_dir upscalers_dir"));
    QCOMPARE(fields("sd15"), QStringLiteral("model vae control_net ip_adapter clip_vision lora_dir upscalers_dir"));
    // A klein file that is a complete checkpoint stays one.
    ModelInfo kleinCheckpoint;
    kleinCheckpoint.kind = ModelKind::Checkpoint;
    QCOMPARE(modelTypeById(QStringLiteral("flux2-klein-4b"))->modelOption(kleinCheckpoint), QStringLiteral("model"));
    QCOMPARE(modelTypeById(QStringLiteral("flux1"))->modelOption(kleinCheckpoint), QStringLiteral("diffusion_model"));
    QCOMPARE(sdFileOptions().size(), 12);

    // Each field says what it expects.
    QCOMPARE(fileFieldLabel(QStringLiteral("llm"), modelTypeById(QStringLiteral("flux2-klein-4b"))), QStringLiteral("Text encoder (Qwen3 4B)"));
    QCOMPARE(fileFieldLabel(QStringLiteral("llm"), modelTypeById(QStringLiteral("flux2-klein-9b"))), QStringLiteral("Text encoder (Qwen3 8B)"));
    QCOMPARE(fileFieldLabel(QStringLiteral("llm"), modelTypeById(QStringLiteral("anima"))), QStringLiteral("Text encoder (Qwen3 0.6B)"));
    QCOMPARE(fileFieldLabel(QStringLiteral("clip_l"), modelTypeById(QStringLiteral("flux1"))), QStringLiteral("Text encoder (CLIP-L)"));
    QCOMPARE(fileFieldLabel(QStringLiteral("t5xxl"), modelTypeById(QStringLiteral("flux1"))), QStringLiteral("Text encoder (T5-XXL)"));
    QCOMPARE(fileFieldLabel(QStringLiteral("vae"), modelTypeById(QStringLiteral("flux1"))), QStringLiteral("VAE"));
    QCOMPARE(fileFieldLabel(QStringLiteral("vae"), modelTypeById(QStringLiteral("sdxl"))), QStringLiteral("VAE (optional)"));
    QCOMPARE(fileFieldLabel(QStringLiteral("llm"), nullptr), QStringLiteral("Text encoder (language model)"));
    QCOMPARE(fileFieldLabel(QStringLiteral("diffusion_model"), modelTypeById(QStringLiteral("flux1"))), QStringLiteral("Diffusion model"));
    QCOMPARE(fileFieldLabel(QStringLiteral("model"), modelTypeById(QStringLiteral("sdxl"))), QStringLiteral("Checkpoint"));

    // A preset of a type its file does not name is refused; one without a type is its own.
    const QByteArray noType = R"({"version": 1, "presets": [{"id": "x", "name": "X model", "match": {"family": "X", "kinds": ["checkpoint"]}}]})";
    QString error;
    const QList<ModelPreset> own = parseModelPresets(noType, QStringLiteral("x"), &error);
    QVERIFY2(own.size() == 1 && error.isEmpty(), qPrintable(error));
    QCOMPARE(own.first().type, QStringLiteral("x"));
    QCOMPARE(own.first().typeName, QStringLiteral("X model"));
    const QByteArray unknownType =
        R"({"version": 1, "presets": [{"id": "x", "name": "X", "type": "nope", "match": {"family": "X", "kinds": ["checkpoint"]}}]})";
    QVERIFY(parseModelPresets(unknownType, QStringLiteral("x"), &error).isEmpty());
    QVERIFY2(error.contains(QLatin1String("unknown type nope")), qPrintable(error));
    const QByteArray namedType = R"({"version": 1, "types": {"xs": "X models"}, "presets": [
        {"id": "x1", "name": "X one", "type": "xs", "match": {"family": "X", "kinds": ["checkpoint"]}}]})";
    const QList<ModelPreset> named = parseModelPresets(namedType, QStringLiteral("x"), &error);
    QVERIFY2(named.size() == 1 && error.isEmpty(), qPrintable(error));
    QCOMPARE(named.first().type, QStringLiteral("xs"));
    QCOMPARE(named.first().typeName, QStringLiteral("X models"));
}

// The two failed starts of a real session (plan §17.1), and what presets make of them.
void TestRuntime::presetProfiles()
{
    const QByteArray pad(2 * 1024 * 1024, '\0'); // the scan skips files under 1 MB
    QTemporaryDir comfy;
    auto put = [&](const QString &rel, const QByteArray &bytes) {
        const QString path = QDir::cleanPath(comfy.filePath(rel));
        writeFile(path, bytes + pad);
        return path;
    };
    // A ComfyUI folder as the user had it: FLUX.2 klein under checkpoints, its VAE and both Qwen3s.
    const QString klein = put(QStringLiteral("checkpoints/flux-2-klein-4b-fp8.safetensors"), flux2Klein(7680));
    const QString flux2Vae = put(QStringLiteral("vae/flux2-vae.safetensors"), vae(32));
    const QString qwen4b = put(QStringLiteral("text_encoders/qwen_3_4b.safetensors"), qwen3(2560));
    const QString qwen8b = put(QStringLiteral("text_encoders/qwen_3_8b_fp8mixed.safetensors"), qwen3(4096));
    const QString animaModel = put(QStringLiteral("checkpoints/miaomiao3DHarem_animaLH3D10.safetensors"), anima());
    const QString controlNet = put(QStringLiteral("controlnet/xinsir-controlnet-openpose-sdxl-1.0.safetensors"), sdxlControlNet());
    const QString ipAdapter = put(QStringLiteral("ipadapter/ip-adapter_sdxl_vit-h.safetensors"), sdxlIpAdapter());
    const QString clipVision = put(QStringLiteral("clip_vision/CLIP-ViT-H-14.safetensors"),
                                   makeSafetensors({{QStringLiteral("vision_model.encoder.layers.0.mlp.fc1.weight"), {5120, 1280}}}));
    const QString wai = put(QStringLiteral("checkpoints/waiIllustriousSDXL_v170.safetensors"), sdxlCheckpoint());
    const QString fluxDev = put(QStringLiteral("diffusion_models/flux1-dev.safetensors"), flux1(true));
    const QString clipL = put(QStringLiteral("clip/clip_l.safetensors"), clip(768));
    put(QStringLiteral("clip/clip_g.safetensors"), clip(1280));
    const QString t5xxl = put(QStringLiteral("text_encoders/t5xxl_fp8_e4m3fn.safetensors"), t5());

    net::HttpClient http;
    QTemporaryDir data;
    ModelLibrary lib(&http);
    lib.setDataDir(data.path());
    lib.load();
    QSignalSpy scanned(&lib, &ModelLibrary::scanFinished);
    QVERIFY(lib.addRoot(comfy.path()));
    QTRY_VERIFY_WITH_TIMEOUT(scanned.size() >= 1 && !lib.isScanning(), 20000);
    auto native = [](const QString &path) { return QDir::toNativeSeparators(path); };

    // 1. FLUX.2 klein chosen as the checkpoint: refused with what to do, not "exited (code 1)".
    ServerProfile failed;
    failed.options = {{QStringLiteral("model"), native(klein)}};
    QStringList problems = messages(checkSdProfile(&lib, failed));
    QCOMPARE(problems.size(), 1);
    QVERIFY2(problems.first().startsWith(QLatin1String("model: flux-2-klein-4b-fp8.safetensors is a diffusion model (FLUX.2 Klein 4B)")),
             qPrintable(problems.first()));
    QVERIFY(problems.first().contains(QLatin1String("Diffusion model")));
    // Choosing it through the preset puts it in its slot with its components: the 4B encoder, not the 8B.
    QVariantMap options = modelServerOptions(&lib, klein, VramTier::Small);
    QCOMPARE(options.value(QStringLiteral("diffusion_model")).toString(), native(klein));
    QCOMPARE(options.value(QStringLiteral("model")).toString(), QString());
    QCOMPARE(options.value(QStringLiteral("vae")).toString(), native(flux2Vae));
    QCOMPARE(options.value(QStringLiteral("llm")).toString(), native(qwen4b));
    ServerProfile fixed;
    fixed.port = 7861;
    for (auto it = options.begin(); it != options.end(); ++it) {
        if (!it.value().toString().isEmpty() || it.value().typeId() != QMetaType::QString)
            fixed.options.insert(it.key(), it.value());
    }
    QVERIFY2(checkSdProfile(&lib, fixed).isEmpty(), qPrintable(messages(checkSdProfile(&lib, fixed)).join(QLatin1Char('\n'))));
    // The server dialog's type: klein's, and none for the profile of the log (the model in the other
    // slot) or with a file the type has no place for.
    QCOMPARE(profileModelType(&lib, fixed)->id, QStringLiteral("flux2-klein-4b"));
    QVERIFY(!profileModelType(&lib, failed));
    ServerProfile withT5 = fixed;
    withT5.options.insert(QStringLiteral("t5xxl"), native(t5xxl));
    QVERIFY(!profileModelType(&lib, withT5));
    QVERIFY(!profileModelType(&lib, ServerProfile()));
    InstalledRuntime rt;
    rt.kind = QStringLiteral("sd.cpp");
    rt.executable = m_tmp.filePath(QStringLiteral("sd-server.exe"));
    const LaunchCommand cmd = buildLaunch(fixed, rt);
    QVERIFY2(cmd.error.isEmpty(), qPrintable(cmd.error));
    QVERIFY(!cmd.arguments.contains(QStringLiteral("-m")));
    QCOMPARE(cmd.arguments.value(cmd.arguments.indexOf(QStringLiteral("--diffusion-model")) + 1), native(klein));
    QCOMPARE(cmd.arguments.value(cmd.arguments.indexOf(QStringLiteral("--vae")) + 1), native(flux2Vae));
    QCOMPARE(cmd.arguments.value(cmd.arguments.indexOf(QStringLiteral("--llm")) + 1), native(qwen4b));
    // The wrong encoder is named, with the one it needs.
    fixed.options.insert(QStringLiteral("llm"), native(qwen8b));
    problems = messages(checkSdProfile(&lib, fixed));
    QCOMPARE(problems.size(), 1);
    QVERIFY2(problems.first().startsWith(QLatin1String("llm: qwen_3_8b_fp8mixed.safetensors")) && problems.first().contains(QLatin1String("Qwen3 4B")),
             qPrintable(problems.first()));
    QVERIFY2(problems.first().contains(QLatin1String("under \"Text encoder (Qwen3 4B)\"")), qPrintable(problems.first()));
    // In a few words, for next to the field.
    QCOMPARE(checkSdProfile(&lib, fixed).first().brief,
             QStringLiteral("This is a Qwen3 8B text encoder; FLUX.2 [klein] 4B needs a Qwen3 4B text encoder."));
    fixed.options.insert(QStringLiteral("llm"), native(t5xxl));
    QCOMPARE(checkSdProfile(&lib, fixed).first().brief, QStringLiteral("This is a T5 text encoder; FLUX.2 [klein] 4B needs a Qwen3 4B text encoder."));
    // No encoder at all: what to get and how big it is.
    fixed.options.remove(QStringLiteral("llm"));
    problems = messages(checkSdProfile(&lib, fixed));
    QCOMPARE(problems.size(), 1);
    QVERIFY2(problems.first().contains(QLatin1String("qwen_3_4b.safetensors")) && problems.first().contains(QLatin1String("GB")), qPrintable(problems.first()));

    // 2. Anima with an SDXL ControlNet and IP-Adapter: each named; none are offered for Anima or FLUX.
    ServerProfile animaProfile;
    animaProfile.options = {{QStringLiteral("model"), native(animaModel)},
                            {QStringLiteral("control_net"), native(controlNet)},
                            {QStringLiteral("ip_adapter"), native(ipAdapter)},
                            {QStringLiteral("clip_vision"), native(clipVision)}};
    problems = messages(checkSdProfile(&lib, animaProfile));
    // The slot (whose message asks for the VAE and encoder too), the ControlNet and the IP-Adapter.
    QVERIFY2(problems.size() == 3, qPrintable(problems.join(QLatin1Char('\n'))));
    QVERIFY(problems.first().startsWith(QLatin1String("model: ")));
    QVERIFY(std::any_of(problems.cbegin(), problems.cend(), [](const QString &m) {
        return m.startsWith(QLatin1String("control_net: xinsir-controlnet-openpose-sdxl-1.0.safetensors is for SDXL models"))
               && m.contains(QLatin1String("Remove it"));
    }));
    QVERIFY(std::any_of(problems.cbegin(), problems.cend(), [](const QString &m) { return m.startsWith(QLatin1String("ip_adapter: ")); }));
    const ModelPreset *animaPreset = presetFor(*modelInfoOf(&lib, animaModel), animaModel);
    QVERIFY(!addonFits(*animaPreset, *modelInfoOf(&lib, controlNet)));
    QVERIFY(!addonFits(*presetFor(*modelInfoOf(&lib, klein), klein), *modelInfoOf(&lib, controlNet)));
    // The same ControlNet and IP-Adapter fit an SDXL checkpoint.
    ServerProfile sdxl;
    sdxl.options = {{QStringLiteral("model"), native(wai)},
                    {QStringLiteral("control_net"), native(controlNet)},
                    {QStringLiteral("ip_adapter"), native(ipAdapter)},
                    {QStringLiteral("clip_vision"), native(clipVision)}};
    QVERIFY2(checkSdProfile(&lib, sdxl).isEmpty(), qPrintable(messages(checkSdProfile(&lib, sdxl)).join(QLatin1Char('\n'))));
    QVERIFY(addonFits(*presetFor(*modelInfoOf(&lib, wai), wai), *modelInfoOf(&lib, controlNet)));
    QCOMPARE(profileModelType(&lib, sdxl)->id, QStringLiteral("sdxl"));
    QVERIFY(!profileModelType(&lib, animaProfile));
    // A VAE in place of the checkpoint's own must be one for SD / SDXL.
    sdxl.options.insert(QStringLiteral("vae"), native(flux2Vae));
    problems = messages(checkSdProfile(&lib, sdxl));
    QVERIFY2(problems.size() == 1 && problems.first().startsWith(QLatin1String("vae: flux2-vae.safetensors (FLUX.2 VAE) does not fit")),
             qPrintable(problems.join(QLatin1Char('\n'))));
    QCOMPARE(checkSdProfile(&lib, sdxl).first().brief, QStringLiteral("This is a FLUX.2 VAE; Illustrious / NoobAI (SDXL) takes only a VAE for SD / SDXL."));
    sdxl.options.remove(QStringLiteral("vae"));
    // A checkpoint needs no components.
    options = modelServerOptions(&lib, wai);
    QCOMPARE(options, (QVariantMap{{QStringLiteral("model"), native(wai)}, {QStringLiteral("diffusion_model"), QString()}}));

    // 3. FLUX.1 dev gets CLIP-L (not CLIP-G) and T5-XXL; on 8 GB, q4_K weights and encoders on the CPU.
    options = modelServerOptions(&lib, fluxDev, VramTier::Small);
    QCOMPARE(options.value(QStringLiteral("clip_l")).toString(), native(clipL));
    QCOMPARE(options.value(QStringLiteral("t5xxl")).toString(), native(t5xxl));
    QVERIFY(!options.contains(QStringLiteral("clip_g")));
    QVERIFY(!options.contains(QStringLiteral("vae"))); // no FLUX.1 VAE in the library
    QCOMPARE(options.value(QStringLiteral("weight_type")).toString(), QStringLiteral("q4_K"));
    QVERIFY(options.value(QStringLiteral("clip_on_cpu")).toBool());
    const QList<ModelCompanion> devNeeds = companionsOf(&lib, fluxDev);
    QCOMPARE(devNeeds.size(), 3);
    QCOMPARE(devNeeds[0].option, QStringLiteral("vae"));
    QVERIFY(devNeeds[0].missing());
    // The missing VAE comes from the ungated copy, with its licence.
    const HubDownload d = companionDownload(*lib.entry(fluxDev), devNeeds[0].file);
    QCOMPARE(d.modelId, QStringLiteral("Comfy-Org/z_image_turbo"));
    QCOMPARE(d.license, QStringLiteral("apache-2.0"));
    QCOMPARE(d.file.fileName(), QStringLiteral("ae.safetensors"));

    // 4. A file that is gone, and a model that is not an image model.
    ServerProfile gone;
    gone.options = {{QStringLiteral("model"), native(comfy.filePath(QStringLiteral("missing.safetensors")))}};
    QVERIFY(messages(checkSdProfile(&lib, gone)).first().contains(QLatin1String("does not exist")));
    ServerProfile vaeAsModel;
    vaeAsModel.options = {{QStringLiteral("model"), native(flux2Vae)}};
    QVERIFY(messages(checkSdProfile(&lib, vaeAsModel)).first().contains(QLatin1String("not an image model")));
}

QTEST_MAIN(TestRuntime)
#include "tst_runtime.moc"
