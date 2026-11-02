#pragma once

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QMutex>
#include <QPair>
#include <QSet>
#include <QSize>
#include <QString>
#include <QThreadPool>

#include <memory>

namespace tandem {

enum class PreviewKind { Text, Binary, Json, Markdown, Image, Pdf, Video, Audio, Font, Folder, Archive, Special };

// What a file is, for viewing. Name and content based; no extra disk access.
PreviewKind classifyPreview(const QString& path, const QString& mimeName, bool isDir, bool isSpecial,
                            const QByteArray& head);
// Kinds with a "rich" view (image, page, frame, cover, rendered, contents).
bool hasRichView(PreviewKind kind);
QString richViewLabel(PreviewKind kind);

struct RichPreview {
    QImage image;        // picture, PDF page, video frame, audio cover
    QString info;        // one or more lines shown under the image
    QString markdown;    // Markdown source to render
    QByteArray fontData; // font file contents (rendered on the UI thread)
    struct Item {
        QString name;
        bool isDir = false;
        qint64 size = -1;
        quint32 mode = 0;
    };
    QList<Item> items;   // folder or archive contents
    QString error;
};

// Blocking: runs on the media pool. maxSize limits decoded pictures (the
// preview column doesn't need a 50-megapixel image); invalid = full size.
RichPreview produceRichPreview(const QString& path, PreviewKind kind, QSize maxSize);
// UI thread only (fonts and painting): a sample sheet for a font file.
QImage renderFontSample(const QByteArray& fontData, QString* familyOut);

// Small pool for previews: ffmpeg or a huge image never delays listings.
QThreadPool* mediaPool();

// Finished previews by path + mtime + size + target size, most recent kept.
// Thread-safe (preloading checks it from workers).
class PreviewCache {
public:
    static PreviewCache& instance();
    static QString key(const QString& path, qint64 mtime, qint64 size, QSize maxSize);
    std::shared_ptr<const RichPreview> get(const QString& key);
    void put(const QString& key, std::shared_ptr<const RichPreview> preview);
    bool beginLoad(const QString& key);  // false when already cached or in flight
    void endLoad(const QString& key);

private:
    QMutex m_mutex;
    QList<QPair<QString, std::shared_ptr<const RichPreview>>> m_items;
    QSet<QString> m_inFlight;
};

}  // namespace tandem
