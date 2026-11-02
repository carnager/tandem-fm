#include "ui/viewer/BlockCache.h"

#include "core/Util.h"

#include <QPointer>

#include <atomic>
#include <fcntl.h>
#include <mutex>
#include <unistd.h>

namespace tandem {

namespace {
constexpr int kMaxBlocks = 512;  // 32 MiB per open file
}

struct BlockCache::Source {
    QByteArray path;
    std::mutex mutex;
    int fd = -1;
    int openError = 0;
    std::atomic<bool> dead{false};

    ~Source()
    {
        if (fd >= 0)
            ::close(fd);
    }
    int get()
    {
        std::lock_guard lock(mutex);
        if (fd < 0 && openError == 0) {
            fd = ::open(path.constData(), O_RDONLY | O_CLOEXEC);
            if (fd < 0)
                openError = errno;
        }
        return fd;
    }
};

BlockCache::BlockCache(const QString& path, qint64 size, QObject* parent)
    : QObject(parent), m_src(std::make_shared<Source>()), m_size(size)
{
    m_src->path = toNative(path);
}

BlockCache::~BlockCache()
{
    m_src->dead.store(true);
}

QByteArray BlockCache::read(qint64 offset, qint64 len, bool* complete)
{
    offset = qBound<qint64>(0, offset, m_size);
    const qint64 end = qMin(m_size, offset + qMax<qint64>(0, len));
    QByteArray out;
    out.reserve(int(end - offset));
    bool ok = true;
    for (qint64 pos = offset; pos < end;) {
        const qint64 block = pos / kBlockSize;
        const auto it = m_blocks.constFind(block);
        if (it == m_blocks.cend()) {
            request(block);
            request(block + 1);  // read ahead
            ok = false;
            break;
        }
        const qint64 inBlock = pos - block * kBlockSize;
        const qint64 n = qMin<qint64>(it->size() - inBlock, end - pos);
        if (n <= 0) {
            ok = false;  // file shrank under us
            break;
        }
        out.append(it->constData() + inBlock, n);
        pos += n;
    }
    if (ok && end < m_size)
        request(end / kBlockSize);  // read ahead
    if (complete)
        *complete = ok;
    return out;
}

void BlockCache::request(qint64 block)
{
    if (m_failed || block < 0 || block * kBlockSize >= m_size || m_blocks.contains(block) || m_pending.contains(block))
        return;
    m_pending.insert(block);
    QPointer<BlockCache> self(this);
    auto src = m_src;
    ioPool()->start([self, src, block] {
        if (src->dead.load())
            return;
        QByteArray data;
        int err = 0;
        const int fd = src->get();
        if (fd < 0) {
            err = src->openError;
        } else {
            data.resize(kBlockSize);
            const ssize_t n = ::pread(fd, data.data(), size_t(kBlockSize), off_t(block * kBlockSize));
            if (n < 0) {
                err = errno;
                data.clear();
            } else {
                data.resize(n);
            }
        }
        postToUi([self, block, data, err] {
            if (!self)
                return;
            self->m_pending.remove(block);
            if (err) {
                if (!self->m_failed) {
                    self->m_failed = true;
                    emit self->failed(errnoString(err));
                }
                return;
            }
            self->m_blocks.insert(block, data);
            self->m_order.append(block);
            while (self->m_order.size() > kMaxBlocks)
                self->m_blocks.remove(self->m_order.takeFirst());
            emit self->loaded();
        });
    });
}

}  // namespace tandem
