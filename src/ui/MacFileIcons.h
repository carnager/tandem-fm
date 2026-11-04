#pragma once

#include "core/FileEntry.h"

#include <QCache>
#include <QHash>
#include <QIcon>
#include <QImage>
#include <QObject>
#include <QSet>

#include <deque>
#include <functional>

namespace tandem {

struct MacIconRequest {
    QString name, path;  // path is empty on servers and inside archives
    bool folder = false;
    bool executable = false;
    int pixels = 64;
};

// Blocking AppKit work. Only call from a worker; returns pixels, never a QPixmap.
QImage readMacFileIcon(const MacIconRequest& request);

// One model's visible icons. The UI only looks in memory and queues requests.
class MacFileIcons : public QObject {
    Q_OBJECT
public:
    using Loader = std::function<QImage(const MacIconRequest&)>;
    explicit MacFileIcons(QObject* parent = nullptr, Loader loader = readMacFileIcon);
    QIcon get(const FileEntry& entry, const QString& localPath = {}, int pixels = 64);
    void setVisibleItems(int count, int pixels);
    void reset(bool keepIcons = false);
    void invalidate(const QString& path);

signals:
    void ready();

private:
    struct Pending {
        QString key;
        MacIconRequest request;
        quint64 generation;
    };
    struct Cached {
        QIcon icon;
    };
    void pump();
    void notify();

    Loader m_loader;
    QCache<QString, Cached> m_icons{16 * 1024 * 1024};  // pixel bytes plus entry overhead
    QSet<QString> m_pending;
    QHash<QString, quint64> m_revisions;
    std::deque<Pending> m_queue;
    quint64 m_generation = 0;
    int m_running = 0;
    bool m_notifyPending = false;
};

}  // namespace tandem
