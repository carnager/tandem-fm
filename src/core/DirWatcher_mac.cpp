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

