#pragma once

#include "runtime/RuntimeStore.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>

namespace lumen::runtime {

// A launch option of a server kind; the settings form is generated from these.
struct OptionField
{
    enum class Type { Text, File, Directory, Integer, Number, Boolean, Choice };
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
    QString error;            // set when a required option is missing

    QString commandLine() const; // quoted, for display and copying
};

LaunchCommand buildLaunch(const ServerProfile &profile, const InstalledRuntime &rt);

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
