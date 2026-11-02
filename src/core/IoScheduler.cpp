#include "core/IoScheduler.h"

#include "core/Backend.h"
#include "core/Remote.h"
#include "core/Util.h"

#include <chrono>
#include <sys/stat.h>

namespace tandem {

IoScheduler& IoScheduler::instance()
{
    static IoScheduler s;
    return s;
}

quint64 IoScheduler::deviceOf(const QString& path)
{
    // All GVfs mounts share one FUSE device; queue each server separately
    // so a copy to one doesn't wait for a copy to another.
    if (const auto r = remoteRootOf(path))
        return (quint64(1) << 63) | quint64(qHash(r->root));
    if (isRemoteLocation(path))  // native backends: one queue per server
        return (quint64(1) << 63) | quint64(qHash(locationAuthority(path)));
    QString p = path;
    struct stat st;
    for (;;) {
        if (::stat(toNative(p).constData(), &st) == 0)
            return quint64(st.st_dev);
        if (p == QLatin1String("/") || p.isEmpty())
            return 0;
        p = parentPath(p);
    }
}

bool IoScheduler::canRun(const Entry& e) const
{
    for (const Entry& other : m_entries) {
        if (&other == &e)
            continue;
        const bool ahead = other.running || other.order < e.order;
        if (ahead && other.devices.intersects(e.devices))
            return false;
    }
    return true;
}

bool IoScheduler::acquire(const void* job, const QSet<quint64>& devices, const std::function<bool()>& cancelled,
                          const std::atomic<bool>& force)
{
    std::unique_lock lock(m_mutex);
    auto it = m_entries.insert(m_entries.end(), Entry{job, devices, m_nextOrder++, false});
    for (;;) {
        if (cancelled()) {
            m_entries.erase(it);
            m_cv.notify_all();
            return false;
        }
        if (force.load() || canRun(*it)) {
            it->running = true;
            return true;
        }
        m_cv.wait_for(lock, std::chrono::milliseconds(200));
    }
}

void IoScheduler::release(const void* job)
{
    std::lock_guard lock(m_mutex);
    m_entries.remove_if([job](const Entry& e) { return e.job == job; });
    m_cv.notify_all();
}

void IoScheduler::prioritize(const void* job)
{
    std::lock_guard lock(m_mutex);
    qint64 lowest = 0;
    for (const Entry& e : m_entries)
        lowest = std::min(lowest, e.order);
    for (Entry& e : m_entries)
        if (e.job == job && !e.running)
            e.order = lowest - 1;
    m_cv.notify_all();
}

void IoScheduler::wake()
{
    std::lock_guard lock(m_mutex);
    m_cv.notify_all();
}

}  // namespace tandem
