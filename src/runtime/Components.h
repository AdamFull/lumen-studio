#pragma once

#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

namespace lumen {
class Job;
class JobQueue;
} // namespace lumen

namespace lumen::net {
class HttpClient;
}

namespace lumen::runtime {

// A file of a component: pinned by its address, SHA-256 and size. An archive (`extract` not empty)
// is unpacked and only the listed entries are kept, renamed to their destinations.
struct ComponentFile
{
    QUrl url;
    QString sha256;
    qint64 size = 0;
    QString path;                    // destination in the component's folder (a plain file)
    QMap<QString, QString> extract;  // archive entry → destination
};

// Something a feature downloads the first time it is used (plan M18 §18.1), pinned as data in
// resources/presets/components.json: ONNX Runtime (into Lumen's data, under components/) and
// detection models (into the models folder, under detection/).
struct ComponentSpec
{
    QString id;                      // "onnxruntime", "sam2.1-tiny"
    QString name;                    // "SAM 2.1 tiny (cuts out objects)"
    QString license;
    QString folder;                  // its folder's name
    bool inModels = false;           // in the models folder (models) rather than Lumen's data (programs)
    QList<ComponentFile> files;

    qint64 downloadSize() const;
    // The destinations, relative to the component's folder.
    QStringList destinations() const;
};

QList<ComponentSpec> parseComponents(const QByteArray &json, QString *error = nullptr);
// The bundled components.
const QList<ComponentSpec> &componentSpecs();

// Installs components and says where they are. GUI-thread object.
class Components : public QObject
{
    Q_OBJECT
public:
    Components(net::HttpClient *http, JobQueue *jobs, QObject *parent = nullptr);

    // Where programs (Lumen's data) and models (the models folder) go.
    void setRoots(const QString &dataRoot, const QString &modelsRoot);
    // The specs in use (the bundled ones; tests give their own).
    void setSpecs(const QList<ComponentSpec> &specs) { m_specs = specs; }
    const ComponentSpec *spec(const QString &id) const;

    QString folder(const QString &id) const;
    QString path(const QString &id, const QString &destination) const;
    bool isInstalled(const QString &id) const;
    // What is still to download for these components (bytes), and their names.
    qint64 missingSize(const QStringList &ids) const;

    // Downloads and unpacks one component as a Queue job (the running one when it is already being
    // installed). Files go to a ".download" folder first; the component's folder is complete or not
    // there.
    Job *install(const QString &id);

signals:
    void installed(const QString &id);

private:
    net::HttpClient *m_http;
    JobQueue *m_jobs;
    QString m_dataRoot;
    QString m_modelsRoot;
    QList<ComponentSpec> m_specs;
    QMap<QString, QPointer<Job>> m_running;
};

} // namespace lumen::runtime
