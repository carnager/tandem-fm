#include "ui/MacFileIcons.h"

#include "core/Sorting.h"
#include "core/Util.h"
#include "ui/IconCache.h"
#include "ui/Theme.h"

#include <QGuiApplication>
#include <QPointer>
#include <QStyleHints>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <sys/stat.h>

namespace tandem {

MacFileIcons& MacFileIcons::instance()
{
    static auto* icons = new MacFileIcons(QCoreApplication::instance());
    return *icons;
}

MacFileIcons::MacFileIcons(QObject* parent, TypeLoader types, PathLoader paths)
    : QObject(parent), m_typeLoader(std::move(types)), m_pathLoader(std::move(paths))
{
    // System icons follow the appearance even when Tandem's theme doesn't.
    connect(&ThemeNotifier::instance(), &ThemeNotifier::changed, this, &MacFileIcons::reset);
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, &MacFileIcons::reset);
}

void MacFileIcons::reset()
{
    ++m_generation;  // lookups still running were made for the old appearance
    m_types.clear();
    m_paths.clear();
    m_pending.clear();
    m_queue.clear();
    macForgetDrawnIcons();
    emit changed();
}

QIcon MacFileIcons::get(const FileEntry& entry, const QString& localPath)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (entry.isParent || !IconCache::usesSystemTheme())
        return IconCache::forEntry(entry);

    QString type;
    if (entry.isDir)
        type = QStringLiteral("folder");
    else if ((entry.mode & (S_IXUSR | S_IXGRP | S_IXOTH)) && extensionOf(entry.name).isEmpty())
        type = QStringLiteral("executable");
    else {
        const QString ext = extensionOf(entry.name).toString().toLower();
        type = QStringLiteral("file:") + ext;
    }
    auto found = m_types.constFind(type);
    if (found == m_types.cend())
        found = m_types.insert(type, m_typeLoader(entry));
    const QIcon fallback = found->isNull() ? IconCache::forEntry(entry) : *found;
    if (localPath.isEmpty() || (!entry.hasCustomIcon && !entry.isPackage))
        return fallback;

    if (auto it = m_paths.find(localPath); it != m_paths.end() && it->revision == entry.iconRevision) {
        touch(*it);
        return it->icon.isNull() ? fallback : it->icon;
    }
    const QString key = localPath + QChar(0) + QString::number(entry.iconRevision);
    if (!m_pending.contains(key)) {
        m_pending.insert(key);
        m_queue.push_back({localPath, entry.iconRevision, m_generation});
        pump();
    }
    return fallback;
}

void MacFileIcons::touch(PathIcon& icon)
{
    icon.shown = m_pass;
    if (!m_passQueued) {
        m_passQueued = true;
        QTimer::singleShot(0, this, [this] {
            ++m_pass;
            m_passQueued = false;
        });
    }
}

void MacFileIcons::trim()
{
    if (m_paths.size() <= kMaxPathIcons)
        return;
    // Least recently shown first, but never one shown in this or the last
    // paint: evicting a visible icon would make the next repaint reload it.
    std::vector<std::pair<quint64, QString>> old;
    for (auto it = m_paths.cbegin(); it != m_paths.cend(); ++it) {
        if (it->shown + 1 < m_pass)
            old.emplace_back(it->shown, it.key());
    }
    std::sort(old.begin(), old.end());
    for (const auto& [shown, path] : old) {
        if (m_paths.size() <= kMaxPathIcons)
            break;
        m_paths.remove(path);
    }
}

void MacFileIcons::pump()
{
    while (m_running < 2 && !m_queue.empty()) {
        Request request = std::move(m_queue.front());
        m_queue.pop_front();
        ++m_running;
        const PathLoader loader = m_pathLoader;
        QPointer<MacFileIcons> self(this);
        ioPool()->start([self, request, loader] {
            const QIcon icon = loader(request.path);
            postToUi([self, request, icon] {
                if (!self)
                    return;
                --self->m_running;
                if (request.generation != self->m_generation) {
                    self->pump();
                    return;
                }
                self->m_pending.remove(request.path + QChar(0) + QString::number(request.revision));
                // Keep failures too, until ctime changes; a late, older lookup must not win.
                auto it = self->m_paths.find(request.path);
                if (it == self->m_paths.end() || it->revision <= request.revision) {
                    PathIcon& stored = self->m_paths[request.path];
                    stored = {request.revision, icon, 0};
                    self->touch(stored);
                    self->trim();
                }
                emit self->ready(request.path);
                self->pump();
            });
        });
    }
}

}  // namespace tandem
