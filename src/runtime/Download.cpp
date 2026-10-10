#include "runtime/Download.h"

#include "core/Async.h"
#include "core/Log.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace lumen::runtime {

namespace {

std::shared_ptr<QCryptographicHash> freshHash()
{
    return std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
}

bool sameOrigin(const QUrl &a, const QUrl &b)
{
    return a.scheme() == b.scheme() && a.host().compare(b.host(), Qt::CaseInsensitive) == 0
           && a.port(a.scheme() == QLatin1String("https") ? 443 : 80) == b.port(b.scheme() == QLatin1String("https") ? 443 : 80);
}

bool isCredential(const QByteArray &header)
{
    return header.compare("Authorization", Qt::CaseInsensitive) == 0 || header.compare("Cookie", Qt::CaseInsensitive) == 0;
}

} // namespace

QString sha256OfFile(const QString &path, qint64 limit)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buf(1 << 20, Qt::Uninitialized);
    qint64 left = limit < 0 ? f.size() : std::min(limit, f.size());
    while (left > 0) {
        const qint64 n = f.read(buf.data(), std::min<qint64>(left, buf.size()));
        if (n <= 0)
            return {};
        hash.addData(QByteArrayView(buf.constData(), n));
        left -= n;
    }
    return QString::fromLatin1(hash.result().toHex());
}

FileDownload::FileDownload(QNetworkAccessManager *nam, DownloadSpec spec, QObject *parent)
    : QObject(parent)
    , m_nam(nam)
    , m_spec(std::move(spec))
{
    m_spec.sha256 = m_spec.sha256.trimmed().toLower();
    m_total = m_spec.size;
}

FileDownload::~FileDownload()
{
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
    }
}

void FileDownload::start()
{
    if (!m_nam) {
        finish(tr("No network access."));
        return;
    }
    QDir().mkpath(QFileInfo(m_spec.destination).absolutePath());
    // A previous run may have finished already: verify the existing file instead of downloading.
    if (QFileInfo::exists(m_spec.destination) && !m_spec.sha256.isEmpty()) {
        QPointer<FileDownload> self(this);
        async::run([path = m_spec.destination] { return sha256OfFile(path); }).then(this, [self](QString digest) {
            if (!self || self->m_cancelled)
                return;
            if (digest == self->m_spec.sha256) {
                self->m_digest = digest;
                self->m_received = QFileInfo(self->m_spec.destination).size();
                self->m_total = self->m_received;
                self->finish({});
            } else {
                QFile::remove(self->m_spec.destination);
                self->resumeFromPart();
            }
        });
        return;
    }
    resumeFromPart();
}

void FileDownload::resumeFromPart()
{
    const QString part = partPath(m_spec.destination);
    const qint64 existing = QFileInfo::exists(part) ? QFileInfo(part).size() : 0;
    if (existing <= 0 || (m_spec.size >= 0 && existing > m_spec.size)) {
        QFile::remove(part);
        m_hash = freshHash();
        m_received = 0;
        request();
        return;
    }
    // Re-hash what is already there (can be hundreds of MB) off the GUI thread.
    QPointer<FileDownload> self(this);
    async::run([part, existing] {
        QFile f(part);
        auto hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
        if (!f.open(QIODevice::ReadOnly))
            return std::shared_ptr<QCryptographicHash>();
        QByteArray buf(1 << 20, Qt::Uninitialized);
        qint64 left = existing;
        while (left > 0) {
            const qint64 n = f.read(buf.data(), std::min<qint64>(left, buf.size()));
            if (n <= 0)
                return std::shared_ptr<QCryptographicHash>();
            hash->addData(QByteArrayView(buf.constData(), n));
            left -= n;
        }
        return hash;
    }).then(this, [self, existing, part](std::shared_ptr<QCryptographicHash> hash) {
        if (!self || self->m_cancelled)
            return;
        if (!hash) {
            QFile::remove(part);
            self->m_hash = freshHash();
            self->m_received = 0;
        } else {
            self->m_hash = std::move(hash); // continue feeding the worker's state
            self->m_received = existing;
        }
        self->request();
    });
}

void FileDownload::request()
{
    if (m_cancelled)
        return;
    const QString part = partPath(m_spec.destination);
    m_file.setFileName(part);
    if (!m_file.open(QIODevice::ReadWrite)) {
        finish(tr("Cannot write %1: %2").arg(QDir::toNativeSeparators(part), m_file.errorString()));
        return;
    }
    m_file.seek(m_received);
    if (m_url.isEmpty())
        m_url = m_spec.url;
    QNetworkRequest req(m_url);
    // Redirects are followed here, so that credentials can be dropped when the origin changes.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setTransferTimeout(60000);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("LumenStudio/%1").arg(QCoreApplication::applicationVersion()));
    const bool trusted = sameOrigin(m_url, m_spec.url);
    for (const auto &h : std::as_const(m_spec.headers)) {
        if (trusted || !isCredential(h.first))
            req.setRawHeader(h.first, h.second);
    }
    m_rangeRequested = m_received > 0;
    if (m_rangeRequested)
        req.setRawHeader("Range", "bytes=" + QByteArray::number(m_received) + '-');
    m_accepted = false;
    ++m_attempts;
    m_reply = m_nam->get(req);
    connect(m_reply, &QNetworkReply::readyRead, this, &FileDownload::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &FileDownload::onReplyFinished);
}

void FileDownload::onReadyRead()
{
    if (!m_reply)
        return;
    if (!m_accepted) {
        const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 200 && status != 206)
            return; // error body: handled in onReplyFinished
        if (status == 200 && m_rangeRequested) {
            // The server ignored the range: start over.
            m_file.resize(0);
            m_file.seek(0);
            m_hash = freshHash();
            m_received = 0;
        }
        const qint64 length = m_reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        if (length > 0)
            m_total = m_received + length;
        else if (m_spec.size >= 0)
            m_total = m_spec.size;
        m_accepted = true;
    }
    const QByteArray data = m_reply->readAll();
    if (data.isEmpty())
        return;
    if (m_file.write(data) != data.size()) {
        const QString err = tr("Cannot write %1: %2").arg(QDir::toNativeSeparators(m_file.fileName()), m_file.errorString());
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        finish(err);
        return;
    }
    m_hash->addData(data);
    m_received += data.size();
    if (m_spec.size >= 0 && m_received > m_spec.size) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_file.close();
        QFile::remove(m_file.fileName());
        finish(tr("The download is larger than expected."));
        return;
    }
    emit progress(m_received, m_total);
}

void FileDownload::onReplyFinished()
{
    QNetworkReply *reply = m_reply;
    if (!reply)
        return;
    onReadyRead();
    if (m_finished)
        return;
    m_reply = nullptr;
    reply->deleteLater();
    m_file.flush();
    m_file.close();
    if (m_cancelled)
        return;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    m_status = status;
    const QNetworkReply::NetworkError err = reply->error();
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
        const QUrl location = QUrl::fromEncoded(reply->rawHeader("Location"));
        const QUrl next = m_url.resolved(location);
        if (location.isEmpty() || !next.isValid()) {
            finish(tr("The server redirected the download to an invalid address."));
            return;
        }
        if (m_url.scheme() == QLatin1String("https") && next.scheme() != QLatin1String("https")) {
            finish(tr("The server redirected the download to an insecure address."));
            return;
        }
        if (++m_redirects > maxRedirects) {
            finish(tr("Too many redirects while downloading %1.").arg(m_spec.url.fileName()));
            return;
        }
        m_url = next;
        --m_attempts; // a redirect is not a failed attempt
        request();
        return;
    }
    if (status == 416 && m_received > 0) {
        // Range not satisfiable: the .part file probably holds the whole file already.
        complete();
        return;
    }
    if (err == QNetworkReply::NoError && (status == 200 || status == 206)) {
        complete();
        return;
    }
    const bool transient = status == 0 || status == 408 || status == 429 || status >= 500;
    if (transient && m_attempts <= maxRetries) {
        qCInfo(lcNet) << "download interrupted, resuming" << m_spec.url.host() << reply->errorString();
        // Start again from the original address: signed CDN links expire.
        m_url = QUrl();
        m_redirects = 0;
        QTimer::singleShot(retryDelayMs * m_attempts, this, [this] { request(); });
        return;
    }
    QString message = status ? tr("HTTP %1 while downloading %2").arg(status).arg(QFileInfo(m_spec.destination).fileName())
                             : reply->errorString();
    if (status == 401 || status == 403)
        message += QStringLiteral(" — ") + tr("access denied (a token may be required)");
    finish(message);
}

void FileDownload::complete()
{
    const QString part = partPath(m_spec.destination);
    const qint64 size = QFileInfo(part).size();
    if (m_spec.size >= 0 && size != m_spec.size) {
        // Wrong size on a "complete" file: drop it so the next attempt starts clean.
        QFile::remove(part);
        finish(tr("Size mismatch for %1 (%2 instead of %3 bytes).").arg(m_spec.url.fileName()).arg(size).arg(m_spec.size));
        return;
    }
    m_digest = QString::fromLatin1(m_hash->result().toHex());
    if (!m_spec.sha256.isEmpty() && m_digest != m_spec.sha256) {
        QFile::remove(part);
        finish(tr("Checksum mismatch for %1: the file was damaged or tampered with.").arg(m_spec.url.fileName()));
        return;
    }
    QFile::remove(m_spec.destination);
    if (!QFile::rename(part, m_spec.destination)) {
        finish(tr("Cannot move the download to %1.").arg(QDir::toNativeSeparators(m_spec.destination)));
        return;
    }
    m_received = size;
    m_total = size;
    emit progress(m_received, m_total);
    finish({});
}

void FileDownload::cancel()
{
    if (m_finished)
        return;
    m_cancelled = true;
    if (m_reply) {
        QNetworkReply *r = m_reply;
        m_reply = nullptr;
        r->disconnect(this);
        r->abort();
        r->deleteLater();
    }
    m_file.close(); // the .part file stays for a later resume
    finish(tr("Cancelled."));
}

void FileDownload::finish(const QString &error)
{
    if (m_finished)
        return;
    m_finished = true;
    m_error = error;
    if (m_file.isOpen())
        m_file.close();
    emit finished();
}

} // namespace lumen::runtime
