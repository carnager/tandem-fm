#pragma once

#include "core/FileEntry.h"

#include <QHash>
#include <QIcon>
#include <QObject>
#include <QSet>
#include <deque>
#include <functional>

namespace tandem {

// Type lookup is main-thread AppKit. File lookup is blocking worker work and
// returns an icon already drawn to pixels.
QIcon macTypeIcon(const FileEntry& entry);
QIcon macPathIcon(const QString& path);
// Makes every native icon draw again, e.g. after an appearance change.
void macForgetDrawnIcons();

// One application-wide cache; only custom icons and packages enter the queue.
class MacFileIcons : public QObject {
    Q_OBJECT
public:
    using TypeLoader = std::function<QIcon(const FileEntry&)>;
    using PathLoader = std::function<QIcon(const QString&)>;
    static MacFileIcons& instance();
    explicit MacFileIcons(QObject* parent = nullptr, TypeLoader types = macTypeIcon, PathLoader paths = macPathIcon);
    QIcon get(const FileEntry& entry, const QString& localPath = {});
    // Drops all icons and drawings; the theme or system appearance changed.
    void reset();

signals:
    void ready(const QString& path);
    void changed();  // every icon may look different now

private:
    struct Request { QString path; qint64 revision; quint64 generation; };
    struct PathIcon { qint64 revision; QIcon icon; };
    void pump();
    TypeLoader m_typeLoader;
    PathLoader m_pathLoader;
    QHash<QString, QIcon> m_types;
    QHash<QString, PathIcon> m_paths;  // one entry per path; a new ctime replaces it
    QSet<QString> m_pending;
    std::deque<Request> m_queue;
    quint64 m_generation = 0;
    int m_running = 0;
};

}  // namespace tandem
