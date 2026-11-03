#ifdef __APPLE__
#include "core/DirWatcher.h"
#include <QSocketNotifier>
#include <fcntl.h>
#include <sys/event.h>
#include <unistd.h>

namespace tandem {
DirWatcher* DirWatcher::instance()
{
    static DirWatcher* watcher = new DirWatcher;
    return watcher;
}
DirWatcher::DirWatcher()
{
    m_fd = ::kqueue();
    if (m_fd >= 0) {
        ::fcntl(m_fd, F_SETFD, FD_CLOEXEC);
        m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
        connect(m_notifier, &QSocketNotifier::activated, this, &DirWatcher::readEvents);
    }
    m_flushTimer.setSingleShot(true);
    m_flushTimer.setInterval(120);
    connect(&m_flushTimer, &QTimer::timeout, this, &DirWatcher::flush);
}
int DirWatcher::addWatch(const QByteArray& path)
{
    if (m_fd < 0)
        return -1;
    const int fd = ::open(path.constData(), O_EVTONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    struct kevent change;
    EV_SET(&change, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_LINK | NOTE_RENAME | NOTE_DELETE | NOTE_REVOKE, 0, nullptr);
    QMutexLocker lock(&m_mutex);
    if (::kevent(m_fd, &change, 1, nullptr, 0, nullptr) < 0) {
        ::close(fd);
        return -1;
    }
    m_refs[fd] = 1;
    return fd;
}
void DirWatcher::removeWatch(int fd)
{
    QMutexLocker lock(&m_mutex);
    if (!m_refs.remove(fd))
        return;
    m_gone.remove(fd);
    m_pending.remove(fd);
    struct kevent change;
    EV_SET(&change, fd, EVFILT_VNODE, EV_DELETE, 0, 0, nullptr);
    ::kevent(m_fd, &change, 1, nullptr, 0, nullptr);
    ::close(fd);
}
void DirWatcher::readEvents()
{
    struct kevent events[64];
    const struct timespec timeout = {0, 0};
    int count;
    bool changed = false;
    while ((count = ::kevent(m_fd, nullptr, 0, events, 64, &timeout)) > 0) {
        for (int i = 0; i < count; ++i) {
            QMutexLocker lock(&m_mutex);
            const int fd = int(events[i].ident);
            if (!m_refs.contains(fd))
                continue;
            if (events[i].fflags & (NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE))
                m_gone.insert(fd);
            else
                m_pending[fd]; // kqueue does not provide changed entry names
            changed = true;
        }
    }
    if (changed && !m_flushTimer.isActive())
        m_flushTimer.start();
}
void DirWatcher::flush()
{
    QSet<int> goneDirs;
    QHash<int, QSet<QString>> pending;
    {
        QMutexLocker lock(&m_mutex);
        goneDirs = std::exchange(m_gone, {});
        pending = std::exchange(m_pending, {});
    }
    for (int fd : goneDirs)
        emit gone(fd);
    for (auto it = pending.cbegin(); it != pending.cend(); ++it)
        if (!goneDirs.contains(it.key()))
            emit changed(it.key(), {}); // empty names request a full listing
}
} // namespace tandem

#else
#include "core/DirWatcher.h"

#include <QFile>
#include <QSocketNotifier>

#include <cerrno>
#include <sys/inotify.h>
#include <unistd.h>

namespace tandem {

namespace {
constexpr uint32_t kMask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_ATTRIB |
                           IN_MODIFY | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR | IN_EXCL_UNLINK;
constexpr int kCoalesceMs = 120;
}  // namespace

DirWatcher* DirWatcher::instance()
{
    static DirWatcher* w = new DirWatcher;
    return w;
}

DirWatcher::DirWatcher()
{
    m_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (m_fd >= 0) {
        m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
        connect(m_notifier, &QSocketNotifier::activated, this, &DirWatcher::readEvents);
    }
    m_flushTimer.setSingleShot(true);
    m_flushTimer.setInterval(kCoalesceMs);
    connect(&m_flushTimer, &QTimer::timeout, this, &DirWatcher::flush);
}

int DirWatcher::addWatch(const QByteArray& nativePath)
{
    if (m_fd < 0)
        return -1;
    const int wd = inotify_add_watch(m_fd, nativePath.constData(), kMask);
    if (wd < 0)
        return -1;
    QMutexLocker lock(&m_mutex);
    m_refs[wd] += 1;
    return wd;
}

void DirWatcher::removeWatch(int wd)
{
    if (wd < 0)
        return;
    QMutexLocker lock(&m_mutex);
    auto it = m_refs.find(wd);
    if (it == m_refs.end())
        return;
    if (--it.value() <= 0) {
        m_refs.erase(it);
        inotify_rm_watch(m_fd, wd);
    }
}

void DirWatcher::readEvents()
{
    alignas(inotify_event) char buf[64 * 1024];
    bool overflowed = false;
    for (;;) {
        const ssize_t n = ::read(m_fd, buf, sizeof buf);
        if (n <= 0)
            break;
        for (char* p = buf; p < buf + n;) {
            auto* ev = reinterpret_cast<inotify_event*>(p);
            p += sizeof(inotify_event) + ev->len;
            if (ev->mask & IN_Q_OVERFLOW) {
                overflowed = true;
                continue;
            }
            if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED)) {
                QMutexLocker lock(&m_mutex);
                if (m_refs.contains(ev->wd))
                    m_gone.insert(ev->wd);
                continue;
            }
            if (ev->len > 0)
                m_pending[ev->wd].insert(QFile::decodeName(ev->name));
        }
    }
    if (overflowed) {
        m_pending.clear();
        emit overflow();
    }
    if ((!m_pending.isEmpty() || !m_gone.isEmpty()) && !m_flushTimer.isActive())
        m_flushTimer.start();  // not restarted on new events: bounded latency
}

void DirWatcher::flush()
{
    const auto pending = std::exchange(m_pending, {});
    QSet<int> goneDirs;
    {
        QMutexLocker lock(&m_mutex);
        goneDirs = std::exchange(m_gone, {});
    }
    for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
        if (!goneDirs.contains(it.key()))
            emit changed(it.key(), QStringList(it.value().cbegin(), it.value().cend()));
    }
    for (int wd : std::as_const(goneDirs))
        emit gone(wd);
}

}  // namespace tandem

#endif
