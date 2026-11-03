#include "ui/IconCache.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QMimeDatabase>
#include <QSet>

#include <algorithm>
#include <sys/stat.h>

namespace tandem {

namespace {

struct Cache {
    bool ready = false;
    QIcon parent, folder, folderLink, broken, exec, generic;
    QHash<QString, QIcon> byExt;
};

Cache& cache()
{
    static Cache c;
    if (!c.ready) {
        c.parent = IconCache::themed("go-up", "folder");
        c.folder = IconCache::themed("folder", "inode-directory");
        c.folderLink = IconCache::themed("folder-link", "folder");
        c.broken = IconCache::themed("emblem-unreadable", "text-x-generic");
        c.exec = IconCache::themed("application-x-executable", "text-x-generic");
        c.generic = IconCache::themed("text-x-generic", "unknown");
        c.ready = true;
    }
    return c;
}

QString& systemThemeName()
{
    static QString name;
    return name;
}

QIcon iconForMime(const QList<QMimeType>& types, const QIcon& generic)
{
    if (types.isEmpty())
        return generic;
    QIcon icon = QIcon::fromTheme(types.first().iconName());
    if (icon.isNull())
        icon = QIcon::fromTheme(types.first().genericIconName(), generic);
    return icon;
}

}  // namespace

QIcon IconCache::themed(const char* name, const char* fallback)
{
    QIcon icon = QIcon::fromTheme(QLatin1String(name));
    if (icon.isNull() && fallback)
        icon = QIcon::fromTheme(QLatin1String(fallback));
    return icon;
}

void IconCache::clear()
{
    Cache& c = cache();
    c.ready = false;
    c.byExt.clear();
}

QString IconCache::systemTheme()
{
    static bool remembered = false;
    if (!remembered) {
        systemThemeName() = QIcon::themeName();
        remembered = true; // an empty native macOS theme is a valid original
    }
    return systemThemeName();
}

void IconCache::setTheme(const QString& name)
{
    systemTheme();  // remember the original before changing it
    QIcon::setThemeName(name.isEmpty() ? systemThemeName() : name);
    clear();
}

const QIcon& IconCache::forEntry(const FileEntry& e)
{
    static QMimeDatabase db;
    Cache& c = cache();

    if (e.isParent)
        return c.parent;
    if (e.isDir)
        return e.isLink ? c.folderLink : c.folder;
    if (e.brokenLink)
        return c.broken;

    const qsizetype dot = e.name.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0) {
        if (e.mode & (S_IXUSR | S_IXGRP | S_IXOTH))
            return c.exec;
        // Names like "Makefile" or "README" still have glob matches.
        const QString key = QLatin1Char('/') + e.name;
        auto it = c.byExt.find(key);
        if (it == c.byExt.end())
            it = c.byExt.insert(key, iconForMime(db.mimeTypesForFileName(e.name), c.generic));
        return it.value();
    }

    const QString ext = e.name.mid(dot + 1).toLower();
    auto it = c.byExt.find(ext);
    if (it == c.byExt.end())
        it = c.byExt.insert(ext, iconForMime(db.mimeTypesForFileName(QStringLiteral("x.") + ext), c.generic));
    return it.value();
}

QList<IconTheme> scanIconThemes()
{
    QStringList bases = QIcon::themeSearchPaths();
    bases << QDir::homePath() + QStringLiteral("/.icons") << QDir::homePath() + QStringLiteral("/.local/share/icons");
    QList<IconTheme> out;
    QSet<QString> seen;
    for (const QString& base : std::as_const(bases)) {
        const QStringList dirs = QDir(base).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString& id : dirs) {
            if (seen.contains(id) || id == QLatin1String("hicolor") || id == QLatin1String("default"))
                continue;
            QFile f(base + QLatin1Char('/') + id + QStringLiteral("/index.theme"));
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
                continue;
            QString name;
            bool inTheme = false, hasDirs = false, hidden = false;
            while (!f.atEnd()) {
                const QString line = QString::fromUtf8(f.readLine()).trimmed();
                if (line.startsWith(QLatin1Char('['))) {
                    inTheme = line == QLatin1String("[Icon Theme]");
                    continue;
                }
                if (!inTheme)
                    continue;
                if (line.startsWith(QLatin1String("Name=")))
                    name = line.mid(5).trimmed();
                else if (line.startsWith(QLatin1String("Directories=")))
                    hasDirs = !line.mid(12).trimmed().isEmpty();
                else if (line.startsWith(QLatin1String("Hidden=")))
                    hidden = line.mid(7).trimmed() == QLatin1String("true");
            }
            if (!hasDirs || hidden)  // cursor themes have no icon directories
                continue;
            seen.insert(id);
            out.append({id, name.isEmpty() ? id : name});
        }
    }
    std::sort(out.begin(), out.end(), [](const IconTheme& a, const IconTheme& b) {
        return QString::compare(a.name, b.name, Qt::CaseInsensitive) < 0;
    });
    return out;
}

}  // namespace tandem
