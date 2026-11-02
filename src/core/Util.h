#pragma once

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QMetaObject>
#include <QString>
#include <QThreadPool>

#include <cstdint>
#include <utility>

namespace tandem {

// Run a callable on the UI (main) thread. Safe to call from any thread.
template <typename F>
void postToUi(F&& f)
{
    QMetaObject::invokeMethod(QCoreApplication::instance(), std::forward<F>(f), Qt::QueuedConnection);
}

// Pool for short filesystem tasks (listing, stat, mkdir...). Sized generously
// because a task can hang on a dead network mount; the UI never waits on it.
QThreadPool* ioPool();

QString formatSize(int64_t bytes);
QString formatSpeed(double bytesPerSec);
QString formatDuration(int64_t seconds);
QString errnoString(int err);
QString modeString(uint32_t mode);

inline QByteArray toNative(const QString& path) { return QFile::encodeName(path); }
inline QString fromNative(const QByteArray& path) { return QFile::decodeName(path); }

QString joinPath(const QString& dir, const QString& name);
QByteArray joinPath(const QByteArray& dir, const QByteArray& name);
QString parentPath(const QString& path);
QString baseName(const QString& path);
QString expandUserPath(const QString& path);  // ~ expansion + cleanPath

}  // namespace tandem
