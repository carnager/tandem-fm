#pragma once

#include <QSet>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <list>
#include <mutex>

namespace tandem {

// Lets at most one data-moving job use a drive at a time. Two copies on the
// same disk run one after another (parallel I/O on one device mostly causes
// seeking and is slower in total); jobs on different drives run in parallel.
// Jobs wait in order of arrival per drive; "run next" moves one to the front.
class IoScheduler {
public:
    static IoScheduler& instance();

    // Blocks until no earlier or running job shares a device with `devices`,
    // or `force` becomes true. Returns false if `cancelled()` turned true.
    bool acquire(const void* job, const QSet<quint64>& devices, const std::function<bool()>& cancelled,
                 const std::atomic<bool>& force);
    void release(const void* job);
    void prioritize(const void* job);
    void wake();

    // The device a path lives on (the nearest existing ancestor for new paths).
    static quint64 deviceOf(const QString& path);

private:
    struct Entry {
        const void* job;
        QSet<quint64> devices;
        qint64 order;
        bool running;
    };
    bool canRun(const Entry& e) const;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::list<Entry> m_entries;
    qint64 m_nextOrder = 0;
};

}  // namespace tandem
