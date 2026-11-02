#include "core/DirLister.h"

#include "core/DirWatcher.h"
#include "core/Util.h"

#include <QElapsedTimer>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace tandem {

namespace {

constexpr unsigned kStatMask = STATX_TYPE | STATX_MODE | STATX_SIZE | STATX_MTIME | STATX_UID | STATX_GID;

// AT_STATX_DONT_SYNC: on network filesystems use cached attributes instead of
// a server round trip per file. Listings stay fast on NFS/SMB.
bool fillEntry(int dirfd, const char* name, FileEntry& e)
{
    struct statx sx;
    if (statx(dirfd, name, AT_SYMLINK_NOFOLLOW | AT_STATX_DONT_SYNC, kStatMask, &sx) != 0)
        return false;
    e.mode = sx.stx_mode;
    e.size = int64_t(sx.stx_size);
    e.mtime = sx.stx_mtime.tv_sec;
    e.uid = sx.stx_uid;
    e.gid = sx.stx_gid;
    if (S_ISLNK(sx.stx_mode)) {
        e.isLink = true;
        struct statx tx;
        if (statx(dirfd, name, AT_STATX_DONT_SYNC, kStatMask, &tx) == 0) {
            e.isDir = S_ISDIR(tx.stx_mode);
            e.size = int64_t(tx.stx_size);
        } else {
            e.brokenLink = true;
        }
    } else {
        e.isDir = S_ISDIR(sx.stx_mode);
    }
    if (e.isDir)
        e.size = -1;
    return true;
}

inline bool isDotOrDotDot(const char* n)
{
    return n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0));
}

}  // namespace

ListResult listDirectory(const QString& path, const ListOptions& opt, const std::atomic<bool>* cancel)
{
    QElapsedTimer timer;
    timer.start();

    ListResult r;
    r.path = path;
    const QByteArray np = toNative(path);

    const int fd = ::open(np.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        r.error = errno;
        return r;
    }
    struct statvfs vs;
    if (fstatvfs(fd, &vs) == 0) {
        r.freeBytes = int64_t(vs.f_bavail) * int64_t(vs.f_frsize);
        r.totalBytes = int64_t(vs.f_blocks) * int64_t(vs.f_frsize);
    }
    DIR* d = fdopendir(fd);
    if (!d) {
        r.error = errno;
        ::close(fd);
        return r;
    }
    const int dfd = dirfd(d);
    r.entries.reserve(128);

    // Watch before reading: anything that changes while we read is reported
    // afterwards and simply re-stat'ed, so no change can slip through.
    if (opt.watch)
        r.watch = DirWatcher::instance()->addWatch(np);

    while (dirent* de = readdir(d)) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            closedir(d);
            DirWatcher::instance()->removeWatch(r.watch);
            r.watch = -1;
            r.entries.clear();
            r.error = ECANCELED;
            return r;
        }
        const char* n = de->d_name;
        if (isDotOrDotDot(n) || (!opt.showHidden && n[0] == '.'))
            continue;
        FileEntry e;
        e.name = QFile::decodeName(n);
        if (!fillEntry(dfd, n, e)) {
            // Vanished or unreadable: still show it, with what readdir told us.
            e.isDir = de->d_type == DT_DIR;
            e.size = e.isDir ? -1 : 0;
        }
        e.key = makeSortKey(e.name);
        r.entries.push_back(std::move(e));
    }
    closedir(d);

    sortEntries(r.entries, opt.sort);
    r.sortedBy = opt.sort;
    r.elapsedMs = timer.elapsed();
    return r;
}

std::vector<EntryUpdate> statNames(const QString& dir, const QStringList& names, bool showHidden)
{
    std::vector<EntryUpdate> out;
    out.reserve(names.size());
    const QByteArray np = toNative(dir);
    const int dfd = ::open(np.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    for (const QString& name : names) {
        if (!showHidden && name.startsWith(QLatin1Char('.')))
            continue;
        FileEntry e;
        e.name = name;
        const QByteArray nn = toNative(name);
        if (dfd >= 0 && fillEntry(dfd, nn.constData(), e)) {
            e.key = makeSortKey(e.name);
            out.emplace_back(name, std::move(e));
        } else {
            out.emplace_back(name, std::nullopt);
        }
    }
    if (dfd >= 0)
        ::close(dfd);
    return out;
}

}  // namespace tandem
