#pragma once

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>

class QSocketNotifier;

namespace tandem {

// Directory watcher: inotify reports changed names on Linux; kqueue reports
// changed directories on macOS. Empty names request a full directory listing.
// Events are coalesced for a short interval so a copy of 10k files into a
// watched directory costs a handful of updates, not 10k.
class DirWatcher : public QObject {
    Q_OBJECT
public:
    // First call must happen on the UI thread.
    static DirWatcher* instance();

    // Thread-safe. May block (path resolution), so call from a worker.
    // Returns a watch id, or -1. Linux watches are ref-counted per directory.
    int addWatch(const QByteArray& nativePath);
    // Thread-safe and non-blocking.
    void removeWatch(int wd);

signals:
    void changed(int wd, const QStringList& names);
    void gone(int wd);    // directory deleted or moved away
    void overflow();      // kernel queue overflowed: everyone should refresh

private:
    DirWatcher();
    void readEvents();
    void flush();

    int m_fd = -1;
    QSocketNotifier* m_notifier = nullptr;
    QMutex m_mutex;
    QHash<int, int> m_refs;
    QHash<int, QSet<QString>> m_pending;
    QSet<int> m_gone;
    QTimer m_flushTimer;
};

}  // namespace tandem
