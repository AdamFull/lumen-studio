#pragma once

#include <QCryptographicHash>
#include <QFile>
#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QTimer>
#include <QUrl>

#include <memory>

class QNetworkAccessManager;
class QNetworkReply;

namespace lumen::runtime {

struct DownloadSpec
{
    QUrl url;
    QString destination;      // final path; data goes to "<destination>.part" until verified
    QString sha256;           // expected digest (hex); empty = not verified
    qint64 size = -1;         // expected size; -1 = unknown
    // Request headers. Credentials (Authorization, Cookie) only go to the URL's own origin: hubs
    // redirect downloads to CDNs with signed URLs, which must not receive the token.
    QList<QPair<QByteArray, QByteArray>> headers;
};

// Downloads one file with resume (HTTP Range into a .part file that survives restarts), retries
// on network errors, incremental SHA-256 and size verification. The destination only appears
// once the content is verified. GUI-thread object.
class FileDownload : public QObject
{
    Q_OBJECT
public:
    FileDownload(QNetworkAccessManager *nam, DownloadSpec spec, QObject *parent = nullptr);
    ~FileDownload() override;

    const DownloadSpec &spec() const { return m_spec; }
    void start();
    void cancel();

    bool isFinished() const { return m_finished; }
    bool ok() const { return m_finished && m_error.isEmpty(); }
    QString errorString() const { return m_error; }
    QString sha256() const { return m_digest; }
    qint64 received() const { return m_received; }
    qint64 total() const { return m_total; }
    int httpStatus() const { return m_status; } // of the last response (0 = none)

    static QString partPath(const QString &destination) { return destination + QStringLiteral(".part"); }

    int maxRetries = 4;
    int maxRedirects = 10;
    int retryDelayMs = 2000;

signals:
    void progress(qint64 received, qint64 total);
    void finished();

private:
    void resumeFromPart();
    void request();
    void onReadyRead();
    void onReplyFinished();
    void complete();
    void finish(const QString &error);

    QPointer<QNetworkAccessManager> m_nam;
    DownloadSpec m_spec;
    QPointer<QNetworkReply> m_reply;
    QUrl m_url;               // current location (after redirects)
    int m_redirects = 0;
    int m_status = 0;
    QFile m_file;
    std::shared_ptr<QCryptographicHash> m_hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    qint64 m_received = 0;
    qint64 m_total = -1;
    int m_attempts = 0;
    bool m_rangeRequested = false;
    bool m_accepted = false; // response status checked for the current reply
    bool m_finished = false;
    bool m_cancelled = false;
    QString m_error;
    QString m_digest;
};

// SHA-256 of a file (hex); empty on read errors. Blocking: call from a worker thread.
QString sha256OfFile(const QString &path, qint64 limit = -1);

} // namespace lumen::runtime
