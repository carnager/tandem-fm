#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>

#include <memory>

namespace tandem {

// Reads a file in 64 KiB blocks on the I/O pool and keeps a bounded cache.
// Views ask for byte ranges and get whatever is already loaded; the rest is
// fetched in the background and loaded() tells them to repaint. Nothing here
// ever blocks the UI thread, so multi-GB files and slow mounts are fine.
class BlockCache : public QObject {
    Q_OBJECT
public:
    static constexpr qint64 kBlockSize = 64 * 1024;

    BlockCache(const QString& path, qint64 size, QObject* parent = nullptr);
    ~BlockCache() override;

    qint64 size() const { return m_size; }

    // Bytes of [offset, offset + len) clipped to the file, contiguous from
    // offset as far as loaded. *complete tells whether the whole range was there.
    QByteArray read(qint64 offset, qint64 len, bool* complete = nullptr);

signals:
    void loaded();
    void failed(const QString& error);

private:
    struct Source;
    void request(qint64 block);

    std::shared_ptr<Source> m_src;
    qint64 m_size;
    QHash<qint64, QByteArray> m_blocks;
    QSet<qint64> m_pending;
    QList<qint64> m_order;  // insertion order, for eviction
    bool m_failed = false;
};

}  // namespace tandem
