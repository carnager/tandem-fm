#include "ui/Thumbnails.h"

#include "core/Util.h"
#include "ui/viewer/Previewers.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QMimeDatabase>
#include <QStandardPaths>
#include <QThreadPool>
#include <QUrl>

namespace tandem {

namespace {

QThreadPool* thumbPool()
{
    static QThreadPool* pool = [] {
        auto* p = new QThreadPool;
        p->setMaxThreadCount(qBound(2, QThread::idealThreadCount() / 2, 4));
        return p;
    }();
    return pool;
}

QString cacheRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/thumbnails");
}

}  // namespace

Thumbnails& Thumbnails::instance()
{
    static Thumbnails t;
    return t;
}

Thumbnails::Thumbnails()
{
    m_images.setMaxCost(400);  // thumbnails kept in memory
}

QString Thumbnails::cacheFile(const QString& path)
{
    const QByteArray uri = QUrl::fromLocalFile(path).toEncoded();
    return cacheRoot() + QStringLiteral("/large/") +
           QString::fromLatin1(QCryptographicHash::hash(uri, QCryptographicHash::Md5).toHex()) + QStringLiteral(".png");
}

bool Thumbnails::canThumbnail(const QString& name)
{
    static QMimeDatabase db;
    const QString mime = db.mimeTypeForFile(name, QMimeDatabase::MatchExtension).name();
    return mime.startsWith(QLatin1String("image/")) || mime.startsWith(QLatin1String("video/")) ||
           mime == QLatin1String("application/pdf") || mime.startsWith(QLatin1String("audio/"));
}

QImage Thumbnails::get(const QString& path, qint64 mtime)
{
    const QString key = path + QLatin1Char('|') + QString::number(mtime);
    QMutexLocker lock(&m_mutex);
    if (QImage* img = m_images.object(key))
        return *img;
    if (m_failed.contains(key) || m_queued.contains(key))
        return {};
    m_queue.push_back({path, mtime});
    m_queued.insert(key);
    lock.unlock();
    pump();
    return {};
}

void Thumbnails::keepOnly(const QSet<QString>& paths)
{
    QMutexLocker lock(&m_mutex);
    std::deque<std::pair<QString, qint64>> kept;
    for (const auto& item : m_queue) {
        if (paths.contains(item.first))
            kept.push_back(item);
        else
            m_queued.remove(item.first + QLatin1Char('|') + QString::number(item.second));
    }
    m_queue.swap(kept);
}

void Thumbnails::pump()
{
    QMutexLocker lock(&m_mutex);
    while (m_running < thumbPool()->maxThreadCount() && !m_queue.empty()) {
        // Newest first: what just scrolled into view.
        const auto [path, mtime] = m_queue.back();
        m_queue.pop_back();
        ++m_running;
        thumbPool()->start([this, path, mtime] {
            const QImage img = make(path, mtime);
            postToUi([this, path, mtime, img] {
                const QString key = path + QLatin1Char('|') + QString::number(mtime);
                {
                    QMutexLocker lock(&m_mutex);
                    --m_running;
                    m_queued.remove(key);
                    if (img.isNull())
                        m_failed.insert(key);
                    else
                        m_images.insert(key, new QImage(img), 1);
                }
                if (!img.isNull())
                    emit ready(path);
                pump();
            });
        });
    }
}

QImage Thumbnails::make(const QString& path, qint64 mtime)
{
    // The shared cache first (written by us, Nautilus, Dolphin, …).
    const QString cached = cacheFile(path);
    QImage img(cached);
    if (!img.isNull() && img.text(QStringLiteral("Thumb::MTime")).toLongLong() == mtime)
        return img;

    static QMimeDatabase db;
    const QString mime = db.mimeTypeForFile(path, QMimeDatabase::MatchExtension).name();
    const PreviewKind kind = classifyPreview(path, mime, false, false, {});
    if (kind != PreviewKind::Image && kind != PreviewKind::Pdf && kind != PreviewKind::Video && kind != PreviewKind::Audio)
        return {};
    img = produceRichPreview(path, kind, QSize(kSize, kSize)).image;
    if (img.isNull())
        return {};
    if (img.width() > kSize || img.height() > kSize)
        img = img.scaled(kSize, kSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    img = img.convertToFormat(QImage::Format_ARGB32);

    // Into the shared cache, as the spec says (never thumbnails of thumbnails).
    if (!path.startsWith(cacheRoot() + QLatin1Char('/'))) {
        img.setText(QStringLiteral("Thumb::URI"), QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded));
        img.setText(QStringLiteral("Thumb::MTime"), QString::number(mtime));
        img.setText(QStringLiteral("Software"), QStringLiteral("Tandem"));
        QDir().mkpath(cacheRoot() + QStringLiteral("/large"));
        const QString part = cached + QStringLiteral(".tandem-part");
        if (img.save(part, "PNG")) {
            QFile::setPermissions(part, QFile::ReadOwner | QFile::WriteOwner);
            if (::rename(QFile::encodeName(part).constData(), QFile::encodeName(cached).constData()) != 0)
                QFile::remove(part);
        }
    }
    return img;
}

}  // namespace tandem
