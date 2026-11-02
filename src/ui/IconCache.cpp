#include "ui/IconCache.h"

#include <QHash>
#include <QMimeDatabase>

#include <sys/stat.h>

namespace tandem {

QIcon IconCache::themed(const char* name, const char* fallback)
{
    QIcon icon = QIcon::fromTheme(QLatin1String(name));
    if (icon.isNull() && fallback)
        icon = QIcon::fromTheme(QLatin1String(fallback));
    return icon;
}

const QIcon& IconCache::forEntry(const FileEntry& e)
{
    static const QIcon parent = themed("go-up", "folder");
    static const QIcon folder = themed("folder", "inode-directory");
    static const QIcon folderLink = themed("folder-link", "folder");
    static const QIcon broken = themed("emblem-unreadable", "text-x-generic");
    static const QIcon exec = themed("application-x-executable", "text-x-generic");
    static const QIcon generic = themed("text-x-generic", "unknown");
    static QHash<QString, QIcon> byExt;
    static QMimeDatabase db;

    if (e.isParent)
        return parent;
    if (e.isDir)
        return e.isLink ? folderLink : folder;
    if (e.brokenLink)
        return broken;

    const qsizetype dot = e.name.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0) {
        if (e.mode & (S_IXUSR | S_IXGRP | S_IXOTH))
            return exec;
        // Names like "Makefile" or "README" still have glob matches.
        const QString key = QLatin1Char('/') + e.name;
        auto it = byExt.find(key);
        if (it == byExt.end()) {
            const QList<QMimeType> types = db.mimeTypesForFileName(e.name);
            QIcon icon = generic;
            if (!types.isEmpty()) {
                icon = QIcon::fromTheme(types.first().iconName());
                if (icon.isNull())
                    icon = QIcon::fromTheme(types.first().genericIconName(), generic);
            }
            it = byExt.insert(key, icon);
        }
        return it.value();
    }

    const QString ext = e.name.mid(dot + 1).toLower();
    auto it = byExt.find(ext);
    if (it == byExt.end()) {
        const QList<QMimeType> types = db.mimeTypesForFileName(QStringLiteral("x.") + ext);
        QIcon icon = generic;
        if (!types.isEmpty()) {
            icon = QIcon::fromTheme(types.first().iconName());
            if (icon.isNull())
                icon = QIcon::fromTheme(types.first().genericIconName(), generic);
        }
        it = byExt.insert(ext, icon);
    }
    return it.value();
}

}  // namespace tandem
