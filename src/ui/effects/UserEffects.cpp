#include "ui/effects/UserEffects.h"

#include "core/Async.h"
#include "core/Paths.h"
#include "filters/EffectNodes.h"
#include "filters/Registry.h"
#include "gpu/Effects.h"
#include "runtime/Components.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPromise>
#include <QSaveFile>

namespace lumen::ui {

namespace {

const QString kComponent = QStringLiteral("slang");

QString cacheBase(const QString &source)
{
    const QByteArray hash = QCryptographicHash::hash(source.toUtf8(), QCryptographicHash::Sha256).toHex().left(32);
    return paths::cache() + QStringLiteral("/effects/") + QString::fromLatin1(hash);
}

QString readSource(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

} // namespace

UserEffects::UserEffects(runtime::Components *components, QObject *parent)
    : QObject(parent)
    , m_components(components)
{
    useCompiler();
    if (m_components) {
        connect(m_components, &runtime::Components::installed, this, [this](const QString &id) {
            if (id != kComponent)
                return;
            useCompiler();
            emit compilerInstalled();
            reload(); // the ones that waited for it
        });
    }
}

void UserEffects::useCompiler()
{
    if (m_components && m_components->isInstalled(kComponent))
        gpu::Compiler::setProgram(m_components->path(kComponent, QStringLiteral("slangc.exe")));
}

QString UserEffects::folder()
{
    return paths::defaultEffects();
}

QString UserEffects::effectIdOf(const QString &name)
{
    return QStringLiteral("user:") + name;
}

bool UserEffects::isValidName(const QString &name)
{
    static const QString forbidden = QStringLiteral("\\/:*?\"<>|");
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || trimmed != name || trimmed.size() > 80 || trimmed.endsWith(u'.'))
        return false;
    for (const QChar c : trimmed) {
        if (forbidden.contains(c) || c.unicode() < 32)
            return false;
    }
    return true;
}

QString UserEffects::freeName(const QString &wanted) const
{
    const QString base = isValidName(wanted) ? wanted : tr("My effect");
    QString name = base;
    for (int i = 2; m_items.contains(name) || QFileInfo::exists(QDir(folder()).filePath(name + QStringLiteral(".slang"))); ++i)
        name = QStringLiteral("%1 %2").arg(base).arg(i);
    return name;
}

QList<UserEffects::Item> UserEffects::items() const
{
    QList<Item> out = m_items.values();
    std::sort(out.begin(), out.end(), [](const Item &a, const Item &b) { return a.name.localeAwareCompare(b.name) < 0; });
    return out;
}

UserEffects::Item UserEffects::item(const QString &name) const
{
    return m_items.value(name);
}

gpu::Compiled UserEffects::cached(const QString &source)
{
    const QString base = cacheBase(source);
    QFile spv(base + QStringLiteral(".spv"));
    QFile json(base + QStringLiteral(".json"));
    gpu::Compiled out;
    if (spv.open(QIODevice::ReadOnly) && json.open(QIODevice::ReadOnly)) {
        out.spirv = spv.readAll();
        out.reflection = json.readAll();
    }
    return out;
}

void UserEffects::store(const QString &source, const gpu::Compiled &compiled)
{
    if (!compiled.ok())
        return;
    const QString base = cacheBase(source);
    QDir().mkpath(QFileInfo(base).path());
    for (const auto &[suffix, bytes] : {std::pair{QStringLiteral(".spv"), compiled.spirv}, std::pair{QStringLiteral(".json"), compiled.reflection}}) {
        QSaveFile f(base + suffix);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(bytes);
            f.commit();
        }
    }
}

void UserEffects::add(const QString &name, const QString &path, const gpu::Compiled &compiled)
{
    Item item{name, path, effectIdOf(name)};
    filters::NodeRegistry &registry = filters::NodeRegistry::global();
    if (!compiled.ok()) {
        item.problem = compiled.error;
        gpu::Effect::removeCompiled(item.effectId);
        filters::effects::removeYourNode(registry, name);
    } else {
        QString why;
        if (gpu::Effect::addCompiled(item.effectId, compiled.spirv, compiled.reflection, &why).isNull()) {
            item.problem = why;
            gpu::Effect::removeCompiled(item.effectId);
            filters::effects::removeYourNode(registry, name);
        } else {
            item.ready = true;
            filters::effects::registerYourNode(registry, item.effectId, name);
        }
    }
    m_items.insert(name, item);
}

void UserEffects::reload()
{
    const int generation = ++m_generation;
    const QStringList before = m_items.keys();
    m_items.clear();
    const QFileInfoList files = QDir(folder()).entryInfoList({QStringLiteral("*.slang")}, QDir::Files, QDir::Name | QDir::IgnoreCase);
    QStringList now;
    for (const QFileInfo &file : files) {
        const QString name = file.completeBaseName();
        if (!isValidName(name))
            continue;
        now << name;
        const QString source = readSource(file.filePath());
        const gpu::Compiled known = cached(source);
        if (known.ok()) {
            add(name, file.filePath(), known);
            continue;
        }
        if (!gpu::Compiler::isAvailable()) {
            m_items.insert(name, Item{name, file.filePath(), effectIdOf(name), false,
                                      tr("It is compiled when the Slang compiler is installed (open it in the effect editor).")});
            continue;
        }
        m_items.insert(name, Item{name, file.filePath(), effectIdOf(name), false, tr("Compiling…")});
        const QString path = file.filePath();
        async::run([source, name] { return gpu::Compiler::compile(source, name); })
            .then(this, [this, generation, name, path, source](const gpu::Compiled &compiled) {
                if (generation != m_generation)
                    return;
                store(source, compiled);
                add(name, path, compiled);
                emit changed();
            });
    }
    // Gone from the folder: gone as effects and nodes.
    for (const QString &name : before) {
        if (!now.contains(name)) {
            gpu::Effect::removeCompiled(effectIdOf(name));
            filters::effects::removeYourNode(filters::NodeRegistry::global(), name);
        }
    }
    emit changed();
}

QFuture<gpu::Compiled> UserEffects::compile(const QString &source, const QString &name) const
{
    const gpu::Compiled known = cached(source);
    if (known.ok()) {
        QPromise<gpu::Compiled> promise;
        promise.start();
        promise.addResult(known);
        promise.finish();
        return promise.future();
    }
    return async::run([source, name] {
        const gpu::Compiled compiled = gpu::Compiler::compile(source, name);
        store(source, compiled);
        return compiled;
    });
}

QFuture<gpu::Compiled> UserEffects::save(const QString &name, const QString &source)
{
    auto failed = [](const QString &why) {
        QPromise<gpu::Compiled> promise;
        promise.start();
        gpu::Compiled c;
        c.error = why;
        promise.addResult(c);
        promise.finish();
        return promise.future();
    };
    if (!isValidName(name))
        return failed(tr("“%1” cannot be a file's name.").arg(name));
    QDir().mkpath(folder());
    const QString path = QDir(folder()).filePath(name + QStringLiteral(".slang"));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(source.toUtf8()) < 0 || !file.commit())
        return failed(tr("%1 could not be written.").arg(QDir::toNativeSeparators(path)));
    QPointer<UserEffects> self(this);
    QFuture<gpu::Compiled> done = compile(source, name);
    done.then(this, [self, name, path](const gpu::Compiled &compiled) {
        if (!self)
            return;
        self->add(name, path, compiled);
        emit self->changed();
    });
    return done;
}

bool UserEffects::remove(const QString &name)
{
    const Item it = m_items.value(name);
    const QString path = it.path.isEmpty() ? QDir(folder()).filePath(name + QStringLiteral(".slang")) : it.path;
    if (QFileInfo::exists(path) && !QFile::moveToTrash(path) && !QFile::remove(path))
        return false;
    m_items.remove(name);
    gpu::Effect::removeCompiled(effectIdOf(name));
    filters::effects::removeYourNode(filters::NodeRegistry::global(), name);
    emit changed();
    return true;
}

bool UserEffects::compilerReady() const
{
    return gpu::Compiler::isAvailable();
}

qint64 UserEffects::compilerDownloadSize() const
{
    return m_components ? m_components->missingSize({kComponent}) : 0;
}

Job *UserEffects::installCompiler()
{
    return m_components ? m_components->install(kComponent) : nullptr;
}

} // namespace lumen::ui
