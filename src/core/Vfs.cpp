// Resolves a path to a backend: local folder, or a folder inside an archive.

#include "core/Archive.h"
#include "core/Backend.h"
#include "core/DirLister.h"
#include "core/Util.h"

#include <QElapsedTimer>

#include <sys/stat.h>
#include <sys/statvfs.h>

namespace tandem {

ListResult listLocation(const QString& path, const ListOptions& opt, const std::atomic<bool>* cancel)
{
    if (isRemoteLocation(path)) {
        QElapsedTimer timer;
        timer.start();
        Backend* b = backendFor(path);
        ListResult r;
        if (!b) {
            r.path = path;
            r.error = EPROTONOSUPPORT;
            r.errorText = QObject::tr("Tandem can't open %1 locations").arg(path.section(QLatin1Char(':'), 0, 0));
        } else {
            ListOptions o = opt;
            o.watch = false;
            r = b->list(path, o, cancel);
            r.path = path;
            for (FileEntry& e : r.entries)
                if (!e.key)
                    e.key = makeSortKey(e.name);
            sortEntries(r.entries, opt.sort);
            r.sortedBy = opt.sort;
        }
        r.remote = true;
        r.elapsedMs = timer.elapsed();
        return r;
    }
    ListResult local = listDirectory(path, opt, cancel);
    if (local.error != ENOTDIR && local.error != ENOENT)
        return local;

    // Find the deepest existing component; if it's a regular file, browse it.
    QString archive = path;
    struct stat st{};
    bool found = false;
    while (!archive.isEmpty() && archive != QLatin1String("/")) {
        if (::stat(toNative(archive).constData(), &st) == 0) {
            found = S_ISREG(st.st_mode);
            break;
        }
        archive = parentPath(archive);
    }
    if (!found)
        return local;

    QElapsedTimer timer;
    timer.start();
    ListResult r;
    r.path = path;
    r.inArchive = true;
    r.archivePath = archive;
    r.innerPath = path == archive ? QString() : path.mid(archive.size() + 1);

    QString error;
    const auto idx = loadArchiveIndex(archive, &error, cancel);
    if (!idx) {
        r.error = (cancel && cancel->load()) ? ECANCELED : EIO;
        r.errorText = error;
        return r;
    }
    if (!r.innerPath.isEmpty()) {
        const ArchiveEntry* dir = idx->find(r.innerPath);
        if (!dir || !dir->isDir) {
            r.error = dir ? ENOTDIR : ENOENT;
            if (dir && isArchiveName(dir->name))
                r.errorText = QObject::tr("Archives inside archives: extract it first (F5)");
            return r;
        }
    }
    for (int i : idx->children.value(r.innerPath)) {
        const ArchiveEntry& a = idx->entries[size_t(i)];
        if (!opt.showHidden && a.name.startsWith(QLatin1Char('.')))
            continue;
        FileEntry e;
        e.name = a.name;
        e.isDir = a.isDir;
        e.isLink = a.isLink;
        e.size = a.size;
        e.mtime = a.mtime;
        e.mode = a.mode;
        e.key = makeSortKey(e.name);
        r.entries.push_back(std::move(e));
    }
    struct statvfs vs;
    if (::statvfs(toNative(parentPath(archive)).constData(), &vs) == 0) {
        r.freeBytes = int64_t(vs.f_bavail) * int64_t(vs.f_frsize);
        r.totalBytes = int64_t(vs.f_blocks) * int64_t(vs.f_frsize);
    }
    sortEntries(r.entries, opt.sort);
    r.sortedBy = opt.sort;
    r.elapsedMs = timer.elapsed();
    return r;
}

}  // namespace tandem
