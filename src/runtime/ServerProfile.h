#pragma once

#include "runtime/RuntimeStore.h"

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>

namespace lumen::runtime {

// A launch option of a server kind; the settings form is generated from these.
struct OptionField
{
    // ModelList: a list of {name, model, mmproj} maps (further models of a llama.cpp server).
    enum class Type { Text, File, Directory, Integer, Number, Boolean, Choice, ModelList };
    QString key;
    QString label;
    Type type = Type::Text;
    QVariant defaultValue;
    QString help;
    QStringList choices;
    double minimum = 0;
    double maximum = 0;
    QString filter;          // File: name filter, e.g. "GGUF models (*.gguf)"
    bool required = false;
    bool advanced = false;
};

QList<OptionField> serverOptions(const QString &kind);

enum class StartPolicy { OnDemand, AtStartup, Manual };
QString startPolicyKey(StartPolicy p);
StartPolicy startPolicyFromKey(const QString &key);

// How one server is run: which runtime, its options and lifecycle.
struct ServerProfile
{
    QString id;
    QString name;
    QString runtimeId;        // InstalledRuntime::id
    int port = 0;
    QVariantMap options;      // keys from serverOptions(kind)
    StartPolicy startPolicy = StartPolicy::OnDemand;
    int idleStopMinutes = 0;  // stop when unused for this long; 0 = keep running
    int startupTimeoutSec = 600;

    QVariant option(const QString &key, const QString &kind) const; // value or the field default
    QJsonObject toJson() const;
    static ServerProfile fromJson(const QJsonObject &o);
};

struct LaunchCommand
{
    QString program;
    QStringList arguments;
    QString workingDirectory;
    QMap<QString, QByteArray> files; // written before the start (llama.cpp model presets)
    QMap<QString, QString> environment; // added to the runtime's environment
    QString error;            // set when a required option is missing

    QString commandLine() const; // quoted, for display and copying
    // Command line plus file contents: changes when a restart is needed.
    QByteArray signature() const;
};

// filesDir: where generated files (llama.cpp presets) go; empty = the temporary folder.
LaunchCommand buildLaunch(const ServerProfile &profile, const InstalledRuntime &rt, const QString &filesDir = QString());

// ---- llama.cpp ----------------------------------------------------------------------------------

// One model of a llama.cpp server. Several models are served through llama.cpp's router mode
// (a presets file); requests pick one by name, and on small GPUs one is loaded at a time.
struct LlamaModel
{
    QString name;
    QString model;
    QString mmproj;
    bool vision() const { return !mmproj.isEmpty(); }
};
// The profile's main model followed by its further models (names made unique).
QList<LlamaModel> llamaModels(const ServerProfile &profile);
// Default model name for a file: its stem ("Qwen3VL-8B-Instruct-Q4_K_M").
QString llamaModelName(const QString &path);
// Router mode (llama.cpp ≥ b7xxx): the build lists --models-preset, or was not probed yet.
bool llamaRouter(const InstalledRuntime &rt);
// The vision projector next to a model ("mmproj-Qwen3VL-8B-Instruct-Q8_0.gguf" for
// "Qwen3VL-8B-Instruct-Q4_K_M.gguf"); empty when there is none.
QString findProjector(const QString &modelPath);

// ---- VRAM presets -------------------------------------------------------------------------------

enum class VramTier { Small, Medium, Large }; // up to 8 GB, up to 12 GB, more
VramTier vramTierFor(quint64 vramBytes);
// Launch options suited to a GPU size; empty for kinds without presets.
QVariantMap presetOptions(const QString &kind, VramTier tier);

// Splits an "extra arguments" string like a shell would (double quotes group words).
QStringList splitArguments(const QString &text);

// Base URL a provider uses to talk to the server ("http://127.0.0.1:8080/v1" for llama.cpp).
QString providerUrl(const QString &kind, int port);
// Provider type that talks to servers of this kind ("llamacpp", "sdcpp").
QString providerTypeFor(const QString &kind);

// First port at or above `preferred` that nothing listens on and no other profile uses.
int choosePort(int preferred, const QList<int> &taken);
bool portIsFree(int port);

} // namespace lumen::runtime
