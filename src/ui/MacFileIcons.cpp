#include "ui/MacFileIcons.h"

#include "core/Sorting.h"
#include "core/Util.h"
#include "ui/IconCache.h"
#include "ui/Theme.h"

#include <QPointer>
#include <QTimer>

#include <sys/stat.h>

namespace tandem {

MacFileIcons::MacFileIcons(QObject* parent, Loader loader) : QObject(parent), m_loader(std::move(loader))
{
    connect(&ThemeNotifier::instance(), &ThemeNotifier::changed, this, [this] {
        reset();
        notify();
    });
}

QIcon MacFileIcons::get(const FileEntry& entry, const QString& localPath)
{
    if (entry.isParent || (!QIcon::themeName().isEmpty() && QIcon::themeName() != IconCache::systemTheme()))
        return IconCache::forEntry(entry);

    MacIconRequest request{entry.name, localPath, entry.isDir,
                           bool(entry.mode & (S_IXUSR | S_IXGRP | S_IXOTH))};
    QString key;
    if (!localPath.isEmpty()) {
        key = QStringLiteral("path:") + localPath + QChar(0) + QString::number(m_revisions.value(localPath)) +
              QChar(0) + QString::number(entry.mtime) + QChar(0) + QString::number(entry.mode) +
              QChar(0) + QString::number(entry.size);
    } else if (entry.isDir) {
        key = QStringLiteral("type:folder");
    } else if (request.executable && extensionOf(entry.name).isEmpty()) {
        key = QStringLiteral("type:executable");
    } else {
        const QString ext = extensionOf(entry.name).toString().toLower();
        key = QStringLiteral("type:") + (ext.isEmpty() ? QLatin1Char('/') + entry.name : ext);
    }
    const Cached* cached = m_icons.object(key);
    if (cached && cached->generation == m_generation)
        return cached->icon.isNull() ? IconCache::forEntry(entry) : cached->icon;
    if (!m_pending.contains(key) && m_pending.size() < 256) {
        m_pending.insert(key);
        m_queue.push_back({key, request, m_generation});
        pump();
    }
    // Keep the previous picture during a re-list, until its replacement is ready.
    return cached && !cached->icon.isNull() ? cached->icon : IconCache::forEntry(entry);
}

void MacFileIcons::reset(bool keepIcons)
{
    ++m_generation;
    if (!keepIcons) {
        m_icons.clear();
        m_revisions.clear();
    }
    m_pending.clear();
    m_queue.clear();
}

void MacFileIcons::invalidate(const QString& path)
{
    ++m_revisions[path];  // an xattr-only icon change need not change mtime
}

void MacFileIcons::notify()
{
    if (m_notifyPending)
        return;
    m_notifyPending = true;
    QTimer::singleShot(0, this, [this] {
        m_notifyPending = false;
        emit ready();
    });
}

void MacFileIcons::pump()
{
    while (m_running < 2 && !m_queue.empty()) {
        Pending task = std::move(m_queue.back());  // newly visible rows first
        m_queue.pop_back();
        ++m_running;
        const Loader loader = m_loader;
        QPointer<MacFileIcons> self(this);
        ioPool()->start([self, task, loader] {
            const QImage image = loader(task.request);
            postToUi([self, task, image] {
                if (!self)
                    return;
                --self->m_running;
                if (task.generation == self->m_generation) {
                    self->m_pending.remove(task.key);
                    // Cache failures too: repainting must not retry a missing icon.
                    self->m_icons.insert(task.key, new Cached{
                        image.isNull() ? QIcon() : QIcon(QPixmap::fromImage(image)), task.generation});
                    self->notify();
                }
                self->pump();
            });
        });
    }
}

}  // namespace tandem
