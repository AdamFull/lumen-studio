#include "runtime/Supervisor.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "net/HttpClient.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QUuid>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace lumen::runtime {

namespace {

class Text
{
    Q_DECLARE_TR_FUNCTIONS(lumen::runtime::ServerSupervisor)
};

constexpr int kLogLines = 1500;

#ifdef Q_OS_WIN
// A job object that kills its processes when its last handle closes (also when Lumen crashes).
void *createKillJob()
{
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job)
        return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
    return job;
}

bool assignToJob(void *job, qint64 pid)
{
    HANDLE proc = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(pid));
    if (!proc)
        return false;
    const bool ok = AssignProcessToJobObject(static_cast<HANDLE>(job), proc);
    CloseHandle(proc);
    return ok;
}
#endif

} // namespace

QString serverStateLabel(ServerState s)
{
    switch (s) {
    case ServerState::Stopped:
        return Text::tr("Stopped");
    case ServerState::Starting:
        return Text::tr("Starting");
    case ServerState::Ready:
        return Text::tr("Running");
    case ServerState::Stopping:
        return Text::tr("Stopping");
    case ServerState::Failed:
        return Text::tr("Failed");
    }
    return {};
}

ServerSupervisor::ServerSupervisor(RuntimeStore *store, net::HttpClient *http, QObject *parent)
    : QObject(parent)
    , m_store(store)
    , m_http(http)
{
}

ServerSupervisor::~ServerSupervisor()
{
    for (Server *s : std::as_const(m_servers)) {
        s->stopping = true;
        if (s->process) {
            s->process->disconnect(this);
            killProcess(s);
            s->process->waitForFinished(3000);
            delete s->process;
        }
#ifdef Q_OS_WIN
        if (s->job)
            CloseHandle(static_cast<HANDLE>(s->job));
#endif
        delete s;
    }
}

QString ServerSupervisor::configPath() const
{
    return m_configPath.isEmpty() ? QDir(paths::appData()).filePath(QStringLiteral("servers.json")) : m_configPath;
}

void ServerSupervisor::load()
{
    m_profiles.clear();
    QFile f(configPath());
    if (f.open(QIODevice::ReadOnly)) {
        for (const QJsonValue &v : QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("servers")).toArray()) {
            ServerProfile p = ServerProfile::fromJson(v.toObject());
            if (!p.id.isEmpty())
                m_profiles << p;
        }
    }
    emit profilesChanged();
}

void ServerSupervisor::save() const
{
    QJsonArray arr;
    for (const ServerProfile &p : m_profiles)
        arr.append(p.toJson());
    QDir().mkpath(QFileInfo(configPath()).absolutePath());
    QFile f(configPath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(QJsonObject{{QStringLiteral("servers"), arr}}).toJson());
}

std::optional<ServerProfile> ServerSupervisor::profile(const QString &id) const
{
    for (const ServerProfile &p : m_profiles) {
        if (p.id == id)
            return p;
    }
    return std::nullopt;
}

QString ServerSupervisor::kindOf(const QString &profileId) const
{
    const auto p = profile(profileId);
    if (!p)
        return {};
    const auto rt = m_store->runtime(p->runtimeId);
    return rt ? rt->kind : QString();
}

ServerProfile ServerSupervisor::createProfile(const QString &runtimeId, const QString &name) const
{
    ServerProfile p;
    p.id = QUuid::createUuid().toString(QUuid::Id128).left(12);
    p.runtimeId = runtimeId;
    const auto rt = m_store->runtime(runtimeId);
    const RuntimeSpec *spec = rt ? Catalog::instance().runtime(rt->kind) : nullptr;
    p.name = !name.isEmpty() ? name : spec ? spec->name : Text::tr("Server");
    QList<int> taken;
    for (const ServerProfile &other : m_profiles)
        taken << other.port;
    p.port = choosePort(spec && spec->defaultPort ? spec->defaultPort : 18000, taken);
    return p;
}

void ServerSupervisor::addProfile(const ServerProfile &profile)
{
    m_profiles << profile;
    save();
    emit profilesChanged();
}

void ServerSupervisor::updateProfile(const ServerProfile &profile)
{
    for (ServerProfile &p : m_profiles) {
        if (p.id == profile.id) {
            p = profile;
            save();
            emit profilesChanged();
            emit stateChanged(profile.id); // needsRestart() may have changed
            return;
        }
    }
}

void ServerSupervisor::removeProfile(const QString &id)
{
    stop(id);
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles[i].id == id) {
            m_profiles.removeAt(i);
            break;
        }
    }
    save();
    emit profilesChanged();
}

LaunchCommand ServerSupervisor::launchFor(const QString &profileId) const
{
    const auto p = profile(profileId);
    if (!p) {
        LaunchCommand c;
        c.error = Text::tr("Unknown server.");
        return c;
    }
    const auto rt = m_store->runtime(p->runtimeId);
    if (!rt) {
        LaunchCommand c;
        c.error = Text::tr("The runtime of %1 is no longer installed.").arg(p->name);
        return c;
    }
    return buildLaunch(*p, *rt, filesDir());
}

QString ServerSupervisor::filesDir() const
{
    return QDir(QFileInfo(configPath()).absolutePath()).filePath(QStringLiteral("servers"));
}

ServerSupervisor::Server *ServerSupervisor::server(const QString &id)
{
    Server *&s = m_servers[id];
    if (!s) {
        s = new Server;
        s->probeTimer.setInterval(healthIntervalMs);
        connect(&s->probeTimer, &QTimer::timeout, this, [this, id] { probeHealth(id); });
        s->idleTimer.setSingleShot(true);
        connect(&s->idleTimer, &QTimer::timeout, this, [this, id] {
            const Server *srv = m_servers.value(id);
            if (srv && srv->users == 0 && srv->state == ServerState::Ready) {
                qCInfo(lcApp) << "stopping idle server" << id;
                stop(id);
            }
        });
    }
    return s;
}

const ServerSupervisor::Server *ServerSupervisor::server(const QString &id) const
{
    return m_servers.value(id);
}

ServerState ServerSupervisor::state(const QString &id) const
{
    const Server *s = server(id);
    return s ? s->state : ServerState::Stopped;
}

QString ServerSupervisor::statusText(const QString &id) const
{
    const Server *s = server(id);
    if (!s)
        return serverStateLabel(ServerState::Stopped);
    return s->status.isEmpty() ? serverStateLabel(s->state) : s->status;
}

QString ServerSupervisor::lastError(const QString &id) const
{
    const Server *s = server(id);
    return s ? s->error : QString();
}

QStringList ServerSupervisor::log(const QString &id) const
{
    const Server *s = server(id);
    return s ? s->log : QStringList();
}

QString ServerSupervisor::logPath(const QString &id) const
{
    const QString dir = m_logDir.isEmpty() ? QDir(paths::logs()).filePath(QStringLiteral("servers")) : m_logDir;
    return QDir(dir).filePath(id + QStringLiteral(".log"));
}

bool ServerSupervisor::needsRestart(const QString &id) const
{
    const Server *s = server(id);
    if (!s || (s->state != ServerState::Ready && s->state != ServerState::Starting))
        return false;
    return launchFor(id).signature() != s->signature;
}

int ServerSupervisor::users(const QString &id) const
{
    const Server *s = server(id);
    return s ? s->users : 0;
}

void ServerSupervisor::setState(const QString &id, ServerState state, const QString &status)
{
    Server *s = server(id);
    s->state = state;
    s->status = status;
    emit stateChanged(id);
}

void ServerSupervisor::settleWaiters(Server *s, const QString &error)
{
    const auto waiters = std::exchange(s->waiters, {});
    for (const auto &w : waiters) {
        w->addResult(error);
        w->finish();
    }
}

QStringList ServerSupervisor::reserveGpu(const QString &reason)
{
    QStringList running;
    for (const ServerProfile &p : std::as_const(m_profiles)) {
        const ServerState st = state(p.id);
        if (st == ServerState::Ready || st == ServerState::Starting) {
            running << p.id;
            stop(p.id);
        }
    }
    m_gpuReservation = reason.isEmpty() ? QStringLiteral("reserved") : reason;
    emit gpuReservationChanged();
    return running;
}

void ServerSupervisor::releaseGpu(const QStringList &restart)
{
    if (m_gpuReservation.isEmpty())
        return;
    m_gpuReservation.clear();
    emit gpuReservationChanged();
    for (const QString &id : restart) {
        if (profile(id))
            start(id);
    }
}

QFuture<QString> ServerSupervisor::start(const QString &id)
{
    if (!m_gpuReservation.isEmpty())
        return QtFuture::makeReadyValueFuture(m_gpuReservation);
    Server *s = server(id);
    if (s->state == ServerState::Ready)
        return QtFuture::makeReadyValueFuture(QString());
    auto promise = std::make_shared<QPromise<QString>>();
    promise->start();
    QFuture<QString> future = promise->future();
    s->waiters << promise;
    if (s->state == ServerState::Starting)
        return future;
    if (s->state == ServerState::Stopping) {
        // Start again once the old process is gone (see onExited).
        return future;
    }
    launch(id);
    return future;
}

void ServerSupervisor::launch(const QString &id)
{
    Server *s = server(id);
    const auto p = profile(id);
    const LaunchCommand cmd = launchFor(id);
    auto failNow = [this, id, s](const QString &error) {
        s->error = error;
        setState(id, ServerState::Failed, error);
        settleWaiters(s, error);
    };
    if (!p)
        return failNow(Text::tr("Unknown server."));
    if (!cmd.error.isEmpty())
        return failNow(cmd.error);
    if (preflight) {
        if (const QString problem = preflight(*p, kindOf(id)); !problem.isEmpty())
            return failNow(problem);
    }
    if (!QFileInfo::exists(cmd.program))
        return failNow(Text::tr("%1 is missing. Reinstall the runtime.").arg(QDir::toNativeSeparators(cmd.program)));
    if (!portIsFree(p->port))
        return failNow(Text::tr("Port %1 is already used by another program. Stop it or choose another port for %2.").arg(p->port).arg(p->name));
    const auto rt = m_store->runtime(p->runtimeId);

    s->error.clear();
    s->stopping = false;
    s->partialLine.clear();
    if (!s->log.isEmpty())
        s->log << QString(); // the in-memory log keeps earlier runs (crash restarts) for the viewer
    s->commandLine = cmd.commandLine();
    s->signature = cmd.signature();
    for (auto it = cmd.files.cbegin(); it != cmd.files.cend(); ++it) {
        QDir().mkpath(QFileInfo(it.key()).absolutePath());
        QFile f(it.key());
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(it.value()) != it.value().size())
            return failNow(Text::tr("Cannot write %1.").arg(QDir::toNativeSeparators(it.key())));
    }
    s->startTimeoutSec = std::max(10, p->startupTimeoutSec);

    // Log file: the previous run is kept as <id>.1.log.
    const QString logFile = logPath(id);
    QDir().mkpath(QFileInfo(logFile).absolutePath());
    QFile::remove(logFile + QStringLiteral(".1"));
    QFile::rename(logFile, logFile + QStringLiteral(".1"));
    s->logFile = std::make_unique<QFile>(logFile);
    if (!s->logFile->open(QIODevice::WriteOnly | QIODevice::Text))
        s->logFile.reset();
    appendLog(id, s, QStringLiteral("> ") + s->commandLine);

#ifdef Q_OS_WIN
    if (!s->job)
        s->job = createKillJob();
#endif
    auto *proc = new QProcess(this);
    s->process = proc;
    proc->setProgram(cmd.program);
    proc->setArguments(cmd.arguments);
    proc->setWorkingDirectory(cmd.workingDirectory);
    QProcessEnvironment env = runtimeEnvironment(*rt);
    for (auto it = cmd.environment.cbegin(); it != cmd.environment.cend(); ++it)
        env.insert(it.key(), it.value());
    proc->setProcessEnvironment(env);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(proc, &QProcess::readyRead, this, [this, id] { onOutput(id); });
    connect(proc, &QProcess::errorOccurred, this, [this, id, proc](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart && server(id)->process == proc)
            onExited(id, -1, true);
    });
    connect(proc, &QProcess::finished, this, [this, id, proc](int code, QProcess::ExitStatus status) {
        if (server(id)->process == proc)
            onExited(id, code, status == QProcess::CrashExit);
    });
    setState(id, ServerState::Starting, Text::tr("Starting…"));
    s->started.start();
    proc->start();
#ifdef Q_OS_WIN
    if (s->job && proc->processId() > 0 && !assignToJob(s->job, proc->processId()))
        qCWarning(lcApp) << "could not put server" << id << "into a job object";
#endif
    s->probeTimer.setInterval(healthIntervalMs);
    s->probeTimer.start();
    qCInfo(lcApp) << "starting server" << p->name << s->commandLine;
}

void ServerSupervisor::onOutput(const QString &id)
{
    Server *s = server(id);
    if (!s->process)
        return;
    s->partialLine += QString::fromLocal8Bit(s->process->readAll());
    qsizetype nl;
    while ((nl = s->partialLine.indexOf(QLatin1Char('\n'))) >= 0) {
        QString line = s->partialLine.left(nl);
        s->partialLine.remove(0, nl + 1);
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        appendLog(id, s, line);
    }
}

void ServerSupervisor::appendLog(const QString &id, Server *s, const QString &line)
{
    s->log << line;
    if (s->log.size() > kLogLines)
        s->log.remove(0, s->log.size() - kLogLines);
    if (s->logFile) {
        s->logFile->write(line.toUtf8());
        s->logFile->write("\n");
        s->logFile->flush();
    }
    if (s->state == ServerState::Starting && !line.trimmed().isEmpty() && !line.startsWith(QLatin1String("> "))) {
        // Show loading progress ("load_tensors: …") while waiting for the health check.
        s->status = Text::tr("Starting: %1").arg(line.trimmed().left(90));
        emit stateChanged(id);
    }
    emit logAppended(id, line);
}

void ServerSupervisor::probeHealth(const QString &id)
{
    Server *s = server(id);
    if (s->state != ServerState::Starting || s->probe)
        return;
    if (s->started.elapsed() > qint64(s->startTimeoutSec) * 1000) {
        const QString error = Text::tr("The server did not become ready within %1 s. See its log.").arg(s->startTimeoutSec);
        s->error = error;
        s->stopping = true;
        killProcess(s);
        setState(id, ServerState::Failed, error);
        settleWaiters(s, error);
        return;
    }
    const auto p = profile(id);
    const QString kind = kindOf(id);
    const RuntimeSpec *spec = Catalog::instance().runtime(kind);
    if (!p || !spec)
        return;
    QUrl url(QStringLiteral("http://127.0.0.1:%1").arg(p->port));
    url.setPath(spec->healthPath);
    net::CallOptions opts;
    opts.maxRetries = 0;
    opts.quiet = true;
    opts.timeoutMs = 3000;
    s->probe = m_http->get(net::HttpClient::makeRequest(url), opts);
    connect(s->probe, &net::HttpCall::finished, this, [this, id, call = s->probe.data()] {
        Server *srv = server(id);
        if (srv->state != ServerState::Starting)
            return;
        if (call->status() == 200) {
            srv->probeTimer.stop();
            appendLog(id, srv, QStringLiteral("[lumen] ready after %1 s").arg(srv->started.elapsed() / 1000.0, 0, 'f', 1));
            setState(id, ServerState::Ready);
            settleWaiters(srv, {});
            armIdle(id);
        }
    });
}

void ServerSupervisor::onExited(const QString &id, int code, bool crashed)
{
    Server *s = server(id);
    s->probeTimer.stop();
    s->idleTimer.stop();
    if (s->process) {
        onOutput(id);
        if (!s->partialLine.isEmpty())
            appendLog(id, s, std::exchange(s->partialLine, {}));
        const QString startError = s->process->error() == QProcess::FailedToStart ? s->process->errorString() : QString();
        s->process->deleteLater();
        s->process = nullptr;
        if (!startError.isEmpty())
            s->error = Text::tr("Cannot start %1: %2").arg(launchFor(id).program, startError);
    }
    s->logFile.reset();
    const ServerState previous = s->state;
    if (s->stopping) {
        s->stopping = false;
        if (previous == ServerState::Stopping) {
            setState(id, ServerState::Stopped);
            // stop() settled the earlier waiters; these came later (restart(), or start() while
            // stopping) and want a fresh process.
            if (!s->waiters.isEmpty())
                launch(id);
        }
        return;
    }
    if (s->error.isEmpty()) {
        // The last lines usually say why (CUDA out of memory, model file not found, ...).
        QStringList tail;
        for (auto it = s->log.crbegin(); it != s->log.crend() && tail.size() < 3; ++it) {
            if (!it->trimmed().isEmpty() && !it->startsWith(QLatin1String("[lumen]")))
                tail.prepend(it->trimmed());
        }
        s->error = (previous == ServerState::Starting ? Text::tr("The server exited while starting (code %1).")
                                                      : Text::tr("The server stopped unexpectedly (code %1)."))
                       .arg(crashed ? QStringLiteral("0x%1").arg(quint32(code), 8, 16, QLatin1Char('0')) : QString::number(code));
        if (!tail.isEmpty())
            s->error += QLatin1Char('\n') + tail.join(QLatin1Char('\n'));
    }
    appendLog(id, s, QStringLiteral("[lumen] ") + s->error.section(QLatin1Char('\n'), 0, 0));
    setState(id, ServerState::Failed, s->error.section(QLatin1Char('\n'), 0, 0));
    if (previous == ServerState::Starting) {
        settleWaiters(s, s->error);
        return;
    }
    // Crashed while running: restart when someone needs it (or it should always run).
    const auto p = profile(id);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    s->crashTimes.removeIf([now](qint64 t) { return now - t > 10 * 60 * 1000; });
    s->crashTimes << now;
    const bool wanted = s->users > 0 || (p && p->startPolicy == StartPolicy::AtStartup);
    if (wanted && s->crashTimes.size() <= maxRestarts) {
        const int delay = 2000 * s->crashTimes.size();
        appendLog(id, s, QStringLiteral("[lumen] restarting in %1 s").arg(delay / 1000));
        QTimer::singleShot(delay, this, [this, id] {
            Server *srv = server(id);
            if (srv->state == ServerState::Failed && !srv->process)
                launch(id);
        });
    }
}

void ServerSupervisor::killProcess(Server *s)
{
#ifdef Q_OS_WIN
    if (s->job && s->process && s->process->state() != QProcess::NotRunning) {
        // Ends the whole tree (ComfyUI's Python workers too).
        TerminateJobObject(static_cast<HANDLE>(s->job), 1);
        return;
    }
#endif
    if (s->process)
        s->process->kill();
}

void ServerSupervisor::stop(const QString &id)
{
    Server *s = m_servers.value(id);
    if (!s)
        return;
    s->idleTimer.stop();
    s->probeTimer.stop();
    if (!s->process || s->process->state() == QProcess::NotRunning) {
        if (s->state != ServerState::Stopped) {
            s->error.clear();
            setState(id, ServerState::Stopped);
        }
        settleWaiters(s, Text::tr("The server was stopped."));
        return;
    }
    s->stopping = true;
    setState(id, ServerState::Stopping, Text::tr("Stopping…"));
    settleWaiters(s, Text::tr("The server was stopped."));
    killProcess(s);
}

QFuture<QString> ServerSupervisor::restart(const QString &id)
{
    Server *s = server(id);
    if (s->process && s->process->state() != QProcess::NotRunning) {
        stop(id);
        auto promise = std::make_shared<QPromise<QString>>();
        promise->start();
        QFuture<QString> f = promise->future();
        s->waiters << promise; // launched again from onExited()
        return f;
    }
    return start(id);
}

QFuture<QString> ServerSupervisor::acquire(const QString &id)
{
    Server *s = server(id);
    ++s->users;
    s->idleTimer.stop();
    return start(id);
}

void ServerSupervisor::release(const QString &id)
{
    Server *s = m_servers.value(id);
    if (!s || s->users == 0)
        return;
    --s->users;
    armIdle(id);
}

void ServerSupervisor::armIdle(const QString &id)
{
    Server *s = server(id);
    const auto p = profile(id);
    if (!p || s->users > 0 || p->idleStopMinutes <= 0 || s->state != ServerState::Ready)
        return;
    s->idleTimer.start(p->idleStopMinutes * 60 * 1000);
}

void ServerSupervisor::startAutoStartServers()
{
    for (const ServerProfile &p : std::as_const(m_profiles)) {
        if (p.startPolicy == StartPolicy::AtStartup)
            start(p.id);
    }
}

QFuture<void> ServerSupervisor::unloadModels(const QString &id)
{
    const auto p = profile(id);
    const auto rt = p ? m_store->runtime(p->runtimeId) : std::nullopt;
    if (!p || !rt || rt->kind != QLatin1String("llama.cpp") || !llamaRouter(*rt) || state(id) != ServerState::Ready)
        return QtFuture::makeReadyVoidFuture();
    auto promise = std::make_shared<QPromise<void>>();
    promise->start();
    QFuture<void> future = promise->future();
    const QString base = QStringLiteral("http://127.0.0.1:%1").arg(p->port);
    net::CallOptions o;
    o.maxRetries = 0;
    o.quiet = true;
    o.timeoutMs = 15000;
    QPointer<ServerSupervisor> self(this);
    // Models still in memory ("sleeping" ones already gave their memory back).
    auto loadedModels = [](const QJsonObject &list) {
        QStringList out;
        for (const QJsonValue &v : list.value(QStringLiteral("data")).toArray()) {
            const QJsonObject m = v.toObject();
            const QString st = m.value(QStringLiteral("status")).toObject().value(QStringLiteral("value")).toString();
            if (st == QLatin1String("loaded") || st == QLatin1String("loading"))
                out << m.value(QStringLiteral("id")).toString();
        }
        return out;
    };
    net::HttpCall *list = m_http->get(net::HttpClient::makeRequest(QUrl(base + QStringLiteral("/models"))), o);
    connect(list, &net::HttpCall::finished, this, [self, list, promise, base, o, loadedModels, id] {
        const QStringList loaded = list->ok() ? loadedModels(list->json()) : QStringList();
        if (!self || loaded.isEmpty()) {
            promise->finish();
            return;
        }
        qCInfo(lcApp) << "unloading" << loaded << "from server" << id << "to free the GPU";
        auto pending = std::make_shared<int>(int(loaded.size()));
        for (const QString &model : loaded) {
            net::HttpCall *call = self->m_http->postJson(net::HttpClient::makeRequest(QUrl(base + QStringLiteral("/models/unload"))),
                                                         QJsonObject{{QStringLiteral("model"), model}}, o);
            connect(call, &net::HttpCall::finished, self, [self, pending, promise, base, o, loadedModels] {
                if (--*pending > 0)
                    return;
                // The unload is asynchronous: wait until the instances are gone (at most ~15 s).
                auto tries = std::make_shared<int>(0);
                auto poll = std::make_shared<std::function<void()>>();
                *poll = [self, promise, base, o, loadedModels, tries, poll] {
                    if (!self || ++*tries > 60) {
                        promise->finish();
                        *poll = nullptr;
                        return;
                    }
                    net::HttpCall *check = self->m_http->get(net::HttpClient::makeRequest(QUrl(base + QStringLiteral("/models"))), o);
                    connect(check, &net::HttpCall::finished, self, [check, promise, loadedModels, poll, self] {
                        if (!check->ok() || loadedModels(check->json()).isEmpty()) {
                            promise->finish();
                            *poll = nullptr;
                            return;
                        }
                        QTimer::singleShot(250, self, [poll] {
                            if (*poll)
                                (*poll)();
                        });
                    });
                };
                (*poll)();
            });
        }
    });
    return future;
}

void ServerSupervisor::stopAll()
{
    for (auto it = m_servers.cbegin(); it != m_servers.cend(); ++it)
        stop(it.key());
}

} // namespace lumen::runtime
