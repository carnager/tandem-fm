#include "core/Undo.h"

#include "core/FileOps.h"
#include "core/Util.h"

#include <QDir>
#include <QObject>

#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

namespace tandem {

QStringList restoreFromTrash(const QList<QPair<QString, QString>>& pairs)
{
    QStringList errors;
    for (const auto& [inTrash, original] : pairs) {
        struct stat st;
        if (::lstat(toNative(original).constData(), &st) == 0) {
            errors << QObject::tr("%1 exists again; left in the trash").arg(original);
            continue;
        }
        makeDirectories(parentPath(original));
        if (const int err = renameNoReplace(inTrash, original)) {
            errors << QObject::tr("%1: %2").arg(original, errnoString(err));
            continue;
        }
        #ifndef __APPLE__
        // .../Trash/files/<name>  ->  .../Trash/info/<name>.trashinfo
        const QString trashRoot = parentPath(parentPath(inTrash));
        QFile::remove(trashRoot + QStringLiteral("/info/") + baseName(inTrash) + QStringLiteral(".trashinfo"));
        #endif
    }
    return errors;
}

static bool removeEmptyTree(const QString& path)
{
    QDir dir(path);
    for (const QString& sub : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
        if (!removeEmptyTree(dir.filePath(sub)))
            return false;
    return ::rmdir(toNative(path).constData()) == 0;
}

QStringList removeIfStillEmpty(const QStringList& paths)
{
    QStringList errors;
    for (const QString& p : paths) {
        struct stat st;
        if (::lstat(toNative(p).constData(), &st) != 0)
            continue;  // already gone
        // Links go (only the link, never what it points to); a hard link only
        // while the file still has another name, so no data is lost.
        const bool removable = S_ISLNK(st.st_mode) || st.st_size == 0 || (S_ISREG(st.st_mode) && st.st_nlink > 1);
        const bool ok = S_ISDIR(st.st_mode) ? removeEmptyTree(p)
                                            : (removable && ::unlink(toNative(p).constData()) == 0);
        if (!ok)
            errors << QObject::tr("%1 is no longer empty; kept").arg(p);
    }
    return errors;
}

}  // namespace tandem
