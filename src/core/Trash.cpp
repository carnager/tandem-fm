#include "core/Trash.h"

#include "core/FileOps.h"
#include "core/Mounts.h"
#include "core/Util.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QUrl>

#include <sys/stat.h>
#include <unistd.h>

namespace tandem {

namespace {

QString homeTrash()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/Trash");
}

// The drive a trash belongs to: paths in its .trashinfo are relative to it.
QString topOf(const QString& root)
{
    if (root == homeTrash())
        return {};
    QString top = parentPath(root);  // .../.Trash-1000 -> drive
    if (baseName(top) == QLatin1String(".Trash"))
        top = parentPath(top);  // .../.Trash/1000 -> drive
    return top;
}

QString uniqueTarget(const QString& path)
{
    struct stat st;
    if (::lstat(toNative(path).constData(), &st) != 0)
        return path;
    const QString dir = parentPath(path), name = baseName(path);
    int dot = int(name.lastIndexOf(QLatin1Char('.')));
    if (dot <= 0)
        dot = int(name.size());
    for (int i = 1;; ++i) {
        const QString c = joinPath(dir, name.left(dot) + (i == 1 ? QObject::tr(" (restored)") : QObject::tr(" (restored %1)").arg(i)) +
                                            name.mid(dot));
        if (::lstat(toNative(c).constData(), &st) != 0)
            return c;
    }
}

}  // namespace

QStringList trashRoots()
{
    QStringList roots{homeTrash()};
    const QString uid = QString::number(::getuid());
    for (const MountPoint& m : listMounts()) {
        if (m.isNetwork || m.path == QLatin1String("/"))
            continue;
        for (const QString& r : {m.path + QStringLiteral("/.Trash/") + uid, m.path + QStringLiteral("/.Trash-") + uid})
            if (!roots.contains(r) && QFileInfo(r + QStringLiteral("/files")).isDir())
                roots << r;
    }
    return roots;
}

QList<TrashItem> listTrash(const std::atomic<bool>* cancel)
{
    QList<TrashItem> out;
    for (const QString& root : trashRoots()) {
        const QString top = topOf(root);
        QDir files(root + QStringLiteral("/files"));
        for (const QString& name : files.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
            if (cancel && cancel->load())
                return out;
            TrashItem it;
            it.trashRoot = root;
            it.name = name;
            QFile info(it.infoPath());
            if (info.open(QIODevice::ReadOnly)) {
                for (const QByteArray& line : info.readAll().split('\n')) {
                    if (line.startsWith("Path=")) {
                        QString p = QUrl::fromPercentEncoding(line.mid(5).trimmed());
                        it.originalPath = p.startsWith(QLatin1Char('/')) || top.isEmpty() ? p : joinPath(top, p);
                    } else if (line.startsWith("DeletionDate=")) {
                        const QDateTime t = QDateTime::fromString(QString::fromLatin1(line.mid(13).trimmed()), Qt::ISODate);
                        it.deletedAt = t.isValid() ? t.toSecsSinceEpoch() : 0;
                    }
                }
            }
            struct stat st;
            if (::lstat(toNative(it.filePath()).constData(), &st) == 0) {
                it.isDir = S_ISDIR(st.st_mode);
                it.size = it.isDir ? qMax<int64_t>(0, directorySize(it.filePath(), cancel)) : st.st_size;
                if (!it.deletedAt)
                    it.deletedAt = st.st_ctime;  // moved into the trash then
            }
            out << it;
        }
    }
    std::sort(out.begin(), out.end(), [](const TrashItem& a, const TrashItem& b) { return a.deletedAt > b.deletedAt; });
    return out;
}

QStringList restoreTrashItems(const QList<TrashItem>& items)
{
    QStringList errors;
    for (const TrashItem& it : items) {
        if (it.originalPath.isEmpty()) {
            errors << QObject::tr("%1: nobody knows where it came from").arg(it.name);
            continue;
        }
        const QString target = uniqueTarget(it.originalPath);
        makeDirectories(parentPath(target));
        // A trash is always on the same drive as what came from there: a rename.
        if (const int err = renameNoReplace(it.filePath(), target)) {
            errors << QObject::tr("%1: %2").arg(it.originalPath, errnoString(err));
            continue;
        }
        QFile::remove(it.infoPath());
    }
    return errors;
}

QStringList deleteTrashItems(const QList<TrashItem>& items)
{
    QStringList errors;
    for (const TrashItem& it : items) {
        struct stat st;
        const QByteArray p = toNative(it.filePath());
        bool ok = true;
        if (::lstat(p.constData(), &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                ::chmod(p.constData(), st.st_mode | 0700);  // read-only folders inside still go
                ok = QDir(it.filePath()).removeRecursively();
            } else {
                ok = ::unlink(p.constData()) == 0;
            }
        }
        if (!ok) {
            errors << QObject::tr("%1: could not delete everything").arg(it.name);
            continue;
        }
        QFile::remove(it.infoPath());
    }
    return errors;
}

}  // namespace tandem
