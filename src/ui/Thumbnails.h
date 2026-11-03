#pragma once

#include <QCache>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QStringList>

#include <deque>

namespace tandem {

// Thumbnails for the grid view: pictures, PDFs, videos, audio covers. Made
// in the background (what's on screen first) and kept in the shared
// freedesktop cache (~/.cache/thumbnails/large), so other apps' thumbnails
// are reused and ours are theirs.
class Thumbnails : public QObject {
    Q_OBJECT
public:
    static Thumbnails& instance();
    static constexpr int kSize = 256;

    // The thumbnail if it's ready; otherwise null, and it's made (ready()).
    QImage get(const QString& path, qint64 mtime);
    // Only these are still worth making (the rest scrolled away).
    void keepOnly(const QSet<QString>& paths);
    // Where the freedesktop cache keeps a file's large thumbnail.
    static QString cacheFile(const QString& path);
    // Whether a file is a kind we can make a thumbnail of (by name).
    static bool canThumbnail(const QString& name);

signals:
    void ready(const QString& path);

private:
    Thumbnails();
    void pump();
    static QImage make(const QString& path, qint64 mtime);

    QCache<QString, QImage> m_images;  // path|mtime -> image
    QMutex m_mutex;
    std::deque<std::pair<QString, qint64>> m_queue;  // newest last
    QSet<QString> m_queued;
    QSet<QString> m_failed;
    int m_running = 0;
};

}  // namespace tandem
