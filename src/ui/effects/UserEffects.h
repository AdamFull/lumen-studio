#pragma once

#include "gpu/Compiler.h"

#include <QFuture>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>

namespace lumen {
class Job;
}
namespace lumen::runtime {
class Components;
}

namespace lumen::ui {

// Your own effects (plan M20 step 7): the Slang files in the effects folder (Documents/Lumen/Effects),
// each an effect ("user:<name>") and a node of the filter editor ("effect:<name>"). What a file
// compiled to is kept in the cache by its text, so starting Lumen needs no compiler; the compiler is
// the "slang" component, downloaded when you first write an effect. GUI-thread object.
class UserEffects : public QObject
{
    Q_OBJECT
public:
    explicit UserEffects(runtime::Components *components, QObject *parent = nullptr);

    struct Item
    {
        QString name;     // the file's name without ".slang"
        QString path;
        QString effectId; // "user:<name>"
        bool ready = false;
        QString problem;  // why it is not ready, in plain words
    };

    static QString folder();
    static QString effectIdOf(const QString &name);
    QList<Item> items() const;
    Item item(const QString &name) const;

    // Reads the folder again: effects from the cache at once, the others compiled in the
    // background when the compiler is there.
    void reload();
    // Writes the source as <name>.slang in the folder, then compiles and adds it; the future gives
    // what the compiler said. An empty or unusable name fails at once.
    QFuture<gpu::Compiled> save(const QString &name, const QString &source);
    bool remove(const QString &name);
    // A name that can be a file and is not taken yet ("My effect", "My effect 2").
    QString freeName(const QString &wanted) const;
    static bool isValidName(const QString &name);

    // Compiles without saving (the editor's preview), in the background.
    QFuture<gpu::Compiled> compile(const QString &source, const QString &name) const;

    // The compiler: installed, its download's size, installing it (a Queue job).
    bool compilerReady() const;
    qint64 compilerDownloadSize() const;
    Job *installCompiler();

    // What `source` compiled to, from the cache; empty when it has not been compiled.
    static gpu::Compiled cached(const QString &source);
    static void store(const QString &source, const gpu::Compiled &compiled);

signals:
    void changed();
    void compilerInstalled();

private:
    void useCompiler();
    void add(const QString &name, const QString &path, const gpu::Compiled &compiled);

    runtime::Components *m_components;
    QHash<QString, Item> m_items;
    int m_generation = 0; // a reload drops what an older one is still compiling
};

} // namespace lumen::ui
