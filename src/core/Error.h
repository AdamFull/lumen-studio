#pragma once

#include <QByteArray>
#include <QException>
#include <QString>

#include <exception>

namespace lumen {

// Base error type. Derives from QException so it crosses QtConcurrent/QFuture boundaries intact
// (other exception types arrive as QUnhandledException).
class Error : public QException
{
public:
    explicit Error(const QString &message)
        : m_message(message.toUtf8())
    {
    }
    explicit Error(const char *message)
        : m_message(message)
    {
    }
    const char *what() const noexcept override { return m_message.constData(); }
    QString message() const { return QString::fromUtf8(m_message); }
    void raise() const override { throw *this; }
    Error *clone() const override { return new Error(*this); }

private:
    QByteArray m_message;
};

// Human-readable message for any exception, unwrapping QUnhandledException.
QString describeException(const std::exception &e);
QString describeException(const std::exception_ptr &e);

} // namespace lumen

// Declares an Error subclass that keeps its dynamic type across QFuture boundaries.
#define LUMEN_DECLARE_ERROR(Name)                                                      \
    class Name : public ::lumen::Error                                                 \
    {                                                                                  \
    public:                                                                            \
        using ::lumen::Error::Error;                                                   \
        void raise() const override { throw *this; }                                   \
        Name *clone() const override { return new Name(*this); }                       \
    }
