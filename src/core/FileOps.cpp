#include "core/FileOps.h"

#include "core/Util.h"

#include <QCoreApplication>

#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tandem {

namespace {

constexpr size_t kChunk = 4 * 1024 * 1024;   // copy_file_range step: keeps cancel/pause responsive
constexpr size_t kBufSize = 1024 * 1024;     // read/write fallback buffer

bool isDotOrDotDot(const char* n) { return n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0)); }

// Names of a directory, read fully up front. Keeps no fd open while we
// recurse, and stays correct while entries are moved out of the directory.
int readNames(const QByteArray& dir, std::vector<QByteArray>& out)
{
    DIR* d = opendir(dir.constData());
    if (!d)
        return errno;
    while (dirent* de = readdir(d))
        if (!isDotOrDotDot(de->d_name))
            out.emplace_back(de->d_name);
    closedir(d);
    return 0;
}

// rename without clobbering. Falls back to check+rename on filesystems
// that do not support RENAME_NOREPLACE.
int moveNoReplace(const QByteArray& src, const QByteArray& dst)
{
    if (renameat2(AT_FDCWD, src.constData(), AT_FDCWD, dst.constData(), RENAME_NOREPLACE) == 0)
        return 0;
    const int e = errno;
    if (e != EINVAL && e != ENOSYS && e != ENOTSUP)
        return e;
    struct stat st;
    if (lstat(dst.constData(), &st) == 0)
        return EEXIST;
    if (::rename(src.constData(), dst.constData()) == 0)
        return 0;
    return errno;
}

bool isInside(const QByteArray& child, const QByteArray& parent)
{
    return child == parent || child.startsWith(parent.endsWith('/') ? parent : parent + '/');
}

void setTimes(const QByteArray& path, const struct stat& st, int flags = 0)
{
    const struct timespec ts[2] = {st.st_atim, st.st_mtim};
    utimensat(AT_FDCWD, path.constData(), ts, flags);
}

}  // namespace

QByteArray uniqueName(const QByteArray& path)
{
    const int slash = path.lastIndexOf('/');
    const QByteArray dir = path.left(slash + 1);
    const QByteArray name = path.mid(slash + 1);
    int dot = name.lastIndexOf('.');
    if (dot <= 0)
        dot = name.size();
    const QByteArray stem = name.left(dot);
    const QByteArray ext = name.mid(dot);
    struct stat st;
    for (int i = 2;; ++i) {
        QByteArray candidate = dir + stem + " (" + QByteArray::number(i) + ")" + ext;
        if (lstat(candidate.constData(), &st) != 0)
            return candidate;
    }
}

// ---------------------------------------------------------------- CopyJob

CopyJob::CopyJob(Mode mode, QStringList sources, QString destination)
    : Job(mode == Mode::Copy ? tr("Copy") : tr("Move")),
      m_mode(mode),
      m_sources(std::move(sources)),
      m_destination(std::move(destination))
{
}

void CopyJob::run()
{
    setState(State::Scanning);
    const QByteArray dest = toNative(m_destination);
    struct stat ds;
    bool destIsDir = ::stat(dest.constData(), &ds) == 0 && S_ISDIR(ds.st_mode);

    if (!destIsDir && m_sources.size() > 1) {
        for (;;) {
            const int e = makeDirectories(m_destination);
            if (e == 0)
                break;
            if (askError(tr("Cannot create folder"), m_destination, e) != Answer::Retry) {
                setState(State::Failed);
                return;
            }
        }
        destIsDir = true;
    } else if (!destIsDir) {
        makeDirectories(parentPath(m_destination));  // "copy as" into a new folder
    }

    std::vector<std::pair<QByteArray, QByteArray>> items;
    for (const QString& s : std::as_const(m_sources)) {
        const QByteArray src = toNative(s);
        const QByteArray dst = destIsDir ? joinPath(dest, toNative(baseName(s))) : dest;
        if (src != dst && isInside(dst, src)) {
            addError(tr("Cannot copy a folder into itself: %1").arg(s));
            continue;
        }
        items.emplace_back(src, dst);
    }

    if (m_mode == Mode::Move) {
        // Same filesystem: a rename per top-level item, no scanning needed.
        std::vector<std::pair<QByteArray, QByteArray>> rest;
        for (auto& it : items) {
            if (checkpoint())
                return;
            setCurrent(fromNative(it.first));
            for (;;) {
                const int e = moveNoReplace(it.first, it.second);
                if (e == 0) {
                    m_filesDone += 1;
                    recordUndo(fromNative(it.second), fromNative(it.first));
                } else if (e == EXDEV || e == EEXIST) {
                    if (e == EXDEV)
                        m_crossDevice = true;
                    rest.push_back(it);
                } else if (askError(tr("Cannot move"), fromNative(it.first), e) == Answer::Retry) {
                    continue;
                }
                break;
            }
            if (isCancelled())
                return;
        }
        items = std::move(rest);
    }

    if (items.empty())
        return;
    // Data has to move: wait for the drives involved to be free.
    {
        QStringList involved;
        for (const auto& it : items)
            involved << fromNative(it.first) << parentPath(fromNative(it.second));
        if (!waitForTurn(involved))
            return;
    }
    int64_t files = 0, bytes = 0;
    for (const auto& it : items) {
        scan(it.first, files, bytes);
        if (isCancelled())
            return;
    }
    m_filesTotal = m_filesDone + files;
    m_bytesTotal = bytes;
    setState(State::Running);

    for (const auto& it : items) {
        // Only things that didn't exist before are undoable: undoing a merge
        // or an overwrite could destroy data that was there first.
        struct stat st;
        const bool existed = ::lstat(it.second.constData(), &st) == 0;
        const bool ok = processItem(it.first, it.second);
        if (!existed && ::lstat(it.second.constData(), &st) == 0) {
            if (m_mode == Mode::Copy)
                recordUndo(fromNative(it.second));
            else if (ok)
                recordUndo(fromNative(it.second), fromNative(it.first));
        }
        if (isCancelled())
            return;
    }
    m_bytesDone = m_bytesTotal.load();
    m_filesDone = m_filesTotal.load();
}

void CopyJob::scan(const QByteArray& path, int64_t& files, int64_t& bytes)
{
    if (isCancelled())
        return;
    struct stat st;
    if (lstat(path.constData(), &st) != 0)
        return;
    files += 1;
    if (S_ISREG(st.st_mode)) {
        bytes += st.st_size;
    } else if (S_ISDIR(st.st_mode)) {
        std::vector<QByteArray> names;
        readNames(path, names);
        for (const QByteArray& n : names)
            scan(joinPath(path, n), files, bytes);
    }
}

bool CopyJob::processItem(const QByteArray& src, const QByteArray& dst)
{
    if (checkpoint())
        return false;
    struct stat st;
    while (lstat(src.constData(), &st) != 0) {
        if (askError(tr("Cannot read"), fromNative(src), errno) != Answer::Retry)
            return false;
    }
    setCurrent(fromNative(src));

    if (m_mode == Mode::Move && !m_crossDevice) {
        const int e = moveNoReplace(src, dst);
        if (e == 0) {
            m_filesDone += 1;
            return true;
        }
        if (e == EXDEV) {
            m_crossDevice = true;
        } else if (e == EEXIST && !S_ISDIR(st.st_mode)) {
            // Same filesystem, target exists: resolve, then rename over it.
            struct stat dstSt;
            if (lstat(dst.constData(), &dstSt) == 0) {
                QByteArray target = dst;
                const auto r = resolveConflict(src, st, target, dstSt);
                if (!r || *r == Resolution::Skip) {
                    m_filesDone += 1;
                    return false;
                }
                if (*r == Resolution::Overwrite && S_ISDIR(dstSt.st_mode)) {
                    addError(tr("Cannot replace a folder with a file: %1").arg(fromNative(dst)));
                    return false;
                }
                const int re = (*r == Resolution::Rename) ? moveNoReplace(src, target)
                                                          : (::rename(src.constData(), target.constData()) ? errno : 0);
                if (re == 0) {
                    m_filesDone += 1;
                    return true;
                }
                if (re != EXDEV) {
                    askError(tr("Cannot move"), fromNative(src), re);
                    return false;
                }
                m_crossDevice = true;
            }
        } else if (e != EEXIST) {
            askError(tr("Cannot move"), fromNative(src), e);
            return false;
        }
    }

    if (S_ISDIR(st.st_mode))
        return copyDirectory(src, dst, st);
    if (S_ISLNK(st.st_mode))
        return copySymlink(src, dst, st);
    if (S_ISREG(st.st_mode))
        return copyRegular(src, dst, st);

    addError(tr("Skipped special file: %1").arg(fromNative(src)));
    m_filesDone += 1;
    return false;
}

std::optional<CopyJob::Resolution> CopyJob::resolveConflict(const QByteArray& src, const struct stat& srcSt,
                                                            QByteArray& dst, const struct stat& dstSt)
{
    auto side = [](const QByteArray& p, const struct stat& st) {
        return ConflictSide{fromNative(p), S_ISDIR(st.st_mode) ? -1 : int64_t(st.st_size), int64_t(st.st_mtime)};
    };
    const bool same = srcSt.st_dev == dstSt.st_dev && srcSt.st_ino == dstSt.st_ino;
    return Job::resolveConflict(side(src, srcSt), dst, side(dst, dstSt), same);
}

// Shared by every job that writes files (copy, extract, pack).
std::optional<Job::Resolution> Job::resolveConflict(const ConflictSide& src, QByteArray& dst,
                                                    const ConflictSide& existing, bool sameFile)
{
    if (sameFile) {
        addError(tr("Source and target are the same file: %1").arg(src.path));
        return Resolution::Skip;
    }
    auto olderRule = [&] { return existing.mtime < src.mtime ? Resolution::Overwrite : Resolution::Skip; };
    switch (m_conflictPolicy) {
    case ConflictPolicy::OverwriteAll:
        return Resolution::Overwrite;
    case ConflictPolicy::SkipAll:
        return Resolution::Skip;
    case ConflictPolicy::OverwriteOlder:
        return olderRule();
    case ConflictPolicy::RenameAll:
        dst = uniqueName(dst);
        return Resolution::Rename;
    case ConflictPolicy::Ask:
        break;
    }

    PromptRequest req;
    req.kind = PromptRequest::Kind::Conflict;
    req.message = tr("The target already exists.");
    req.srcPath = src.path;
    req.dstPath = fromNative(dst);
    req.srcSize = src.size;
    req.dstSize = existing.size;
    req.srcTime = src.mtime;
    req.dstTime = existing.mtime;

    switch (prompt(req)) {
    case Answer::Overwrite:
        return Resolution::Overwrite;
    case Answer::OverwriteAll:
        m_conflictPolicy = ConflictPolicy::OverwriteAll;
        return Resolution::Overwrite;
    case Answer::OverwriteOlderAll:
        m_conflictPolicy = ConflictPolicy::OverwriteOlder;
        return olderRule();
    case Answer::Skip:
        return Resolution::Skip;
    case Answer::SkipAll:
        m_conflictPolicy = ConflictPolicy::SkipAll;
        return Resolution::Skip;
    case Answer::RenameAll:
        m_conflictPolicy = ConflictPolicy::RenameAll;
        [[fallthrough]];
    case Answer::Rename:
        dst = uniqueName(dst);
        return Resolution::Rename;
    default:
        cancel();
        return std::nullopt;
    }
}

bool CopyJob::copyDirectory(const QByteArray& src, QByteArray dst, const struct stat& st)
{
    struct stat dstSt;
    bool exists = lstat(dst.constData(), &dstSt) == 0;
    if (exists && !S_ISDIR(dstSt.st_mode)) {
        const auto r = resolveConflict(src, st, dst, dstSt);
        if (!r || *r == Resolution::Skip)
            return false;
        if (*r == Resolution::Overwrite && ::unlink(dst.constData()) != 0) {
            askError(tr("Cannot replace"), fromNative(dst), errno);
            return false;
        }
        exists = false;
    }
    if (!exists) {
        while (::mkdir(dst.constData(), 0700) != 0) {
            if (askError(tr("Cannot create folder"), fromNative(dst), errno) != Answer::Retry)
                return false;
        }
    }

    std::vector<QByteArray> names;
    for (;;) {
        const int e = readNames(src, names);
        if (e == 0)
            break;
        if (askError(tr("Cannot read folder"), fromNative(src), e) != Answer::Retry)
            return false;
        names.clear();
    }

    bool ok = true;
    for (const QByteArray& n : names) {
        if (!processItem(joinPath(src, n), joinPath(dst, n)))
            ok = false;
        if (isCancelled())
            return false;
    }
    ::chmod(dst.constData(), st.st_mode & 07777);
    setTimes(dst, st);
    m_filesDone += 1;

    if (ok && m_mode == Mode::Move && ::rmdir(src.constData()) != 0) {
        addError(tr("Cannot remove source folder: %1 (%2)").arg(fromNative(src), errnoString(errno)));
        ok = false;
    }
    return ok;
}

bool CopyJob::copySymlink(const QByteArray& src, QByteArray dst, const struct stat& st)
{
    QByteArray target(qMax<qsizetype>(st.st_size, 255) + 1, '\0');
    const ssize_t n = ::readlink(src.constData(), target.data(), target.size());
    if (n < 0) {
        askError(tr("Cannot read link"), fromNative(src), errno);
        return false;
    }
    target.truncate(n);

    for (;;) {
        if (::symlink(target.constData(), dst.constData()) == 0)
            break;
        const int e = errno;
        struct stat dstSt;
        if (e == EEXIST && lstat(dst.constData(), &dstSt) == 0) {
            const auto r = resolveConflict(src, st, dst, dstSt);
            if (!r || *r == Resolution::Skip) {
                m_filesDone += 1;
                return false;
            }
            if (*r == Resolution::Overwrite && ::unlink(dst.constData()) != 0) {
                askError(tr("Cannot replace"), fromNative(dst), errno);
                return false;
            }
            continue;
        }
        if (askError(tr("Cannot create link"), fromNative(dst), e) != Answer::Retry)
            return false;
    }
    setTimes(dst, st, AT_SYMLINK_NOFOLLOW);
    m_filesDone += 1;
    if (m_mode == Mode::Move && ::unlink(src.constData()) != 0) {
        addError(tr("Cannot remove source: %1 (%2)").arg(fromNative(src), errnoString(errno)));
        return false;
    }
    return true;
}

bool CopyJob::copyRegular(const QByteArray& src, QByteArray dst, const struct stat& st)
{
    int in;
    while ((in = ::open(src.constData(), O_RDONLY | O_CLOEXEC)) < 0) {
        if (askError(tr("Cannot open"), fromNative(src), errno) != Answer::Retry)
            return false;
    }

    int out;
    for (;;) {
        out = ::open(dst.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (out >= 0)
            break;
        const int e = errno;
        struct stat dstSt;
        if (e == EEXIST && lstat(dst.constData(), &dstSt) == 0) {
            const auto r = resolveConflict(src, st, dst, dstSt);
            if (!r || *r == Resolution::Skip) {
                ::close(in);
                m_filesDone += 1;
                m_bytesDone += st.st_size;
                return false;
            }
            if (*r == Resolution::Overwrite) {
                if (S_ISDIR(dstSt.st_mode)) {
                    addError(tr("Cannot replace a folder with a file: %1").arg(fromNative(dst)));
                    ::close(in);
                    return false;
                }
                if (::unlink(dst.constData()) != 0) {
                    askError(tr("Cannot replace"), fromNative(dst), errno);
                    ::close(in);
                    return false;
                }
            }
            continue;  // Rename already changed dst
        }
        if (askError(tr("Cannot create"), fromNative(dst), e) != Answer::Retry) {
            ::close(in);
            return false;
        }
    }

    const int64_t before = m_bytesDone.load();
    int err;
    for (;;) {
        err = copyData(in, out, st.st_size);
        if (err <= 0)
            break;
        if (askError(tr("Error while copying"), fromNative(src), err) != Answer::Retry)
            break;
        // Retry the whole file from the start.
        m_bytesDone = before;
        ::lseek(in, 0, SEEK_SET);
        ::lseek(out, 0, SEEK_SET);
        if (::ftruncate(out, 0) != 0) {
            err = errno;
            break;
        }
    }

    bool ok = err == 0;
    if (ok) {
        ::fchmod(out, st.st_mode & 07777);
        if (::fchown(out, st.st_uid, st.st_gid) != 0) {
            // Expected when not root: the copy belongs to us.
        }
        const struct timespec ts[2] = {st.st_atim, st.st_mtim};
        ::futimens(out, ts);
    }
    // Network filesystems may only report write errors on close.
    if (::close(out) != 0 && ok) {
        askError(tr("Error while writing"), fromNative(dst), errno);
        ok = false;
    }
    ::close(in);

    if (!ok) {
        ::unlink(dst.constData());
        return false;
    }
    m_filesDone += 1;
    if (m_mode == Mode::Move && ::unlink(src.constData()) != 0) {
        addError(tr("Cannot remove source: %1 (%2)").arg(fromNative(src), errnoString(errno)));
        return false;
    }
    return true;
}

int CopyJob::copyData(int in, int out, int64_t size)
{
    ::posix_fadvise(in, 0, 0, POSIX_FADV_SEQUENTIAL);

    // Reflink (btrfs, xfs, bcachefs): instant copy-on-write clone.
    if (size > 0 && ::ioctl(out, FICLONE, in) == 0) {
        m_bytesDone += size;
        return 0;
    }

    bool useCfr = true;
    int64_t copied = 0;
    for (;;) {
        if (checkpoint())
            return -1;
        ssize_t n;
        if (useCfr) {
            // In-kernel copy; server-side copy on NFS 4.2 / SMB3.
            n = ::copy_file_range(in, nullptr, out, nullptr, kChunk, 0);
            if (n < 0 && copied == 0 &&
                (errno == EXDEV || errno == EINVAL || errno == ENOSYS || errno == EOPNOTSUPP || errno == EBADF ||
                 errno == EPERM)) {
                useCfr = false;
                continue;
            }
        } else {
            if (m_buffer.empty())
                m_buffer.resize(kBufSize);
            n = ::read(in, m_buffer.data(), m_buffer.size());
            if (n > 0) {
                ssize_t off = 0;
                while (off < n) {
                    const ssize_t w = ::write(out, m_buffer.data() + off, size_t(n - off));
                    if (w < 0) {
                        if (errno == EINTR)
                            continue;
                        return errno;
                    }
                    off += w;
                }
            }
        }
        if (n == 0)
            return 0;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return errno;
        }
        copied += n;
        m_bytesDone += n;
    }
}

// -------------------------------------------------------------- DeleteJob

DeleteJob::DeleteJob(QStringList paths, bool toTrash)
    : Job(toTrash ? tr("Move to trash") : tr("Delete")), m_paths(std::move(paths)), m_toTrash(toTrash)
{
}

void DeleteJob::count(const QByteArray& path, int64_t& files)
{
    if (isCancelled())
        return;
    struct stat st;
    if (lstat(path.constData(), &st) != 0)
        return;
    files += 1;
    if (S_ISDIR(st.st_mode)) {
        std::vector<QByteArray> names;
        readNames(path, names);
        for (const QByteArray& n : names)
            count(joinPath(path, n), files);
    }
}

void DeleteJob::run()
{
    if (m_toTrash) {
        m_filesTotal = m_paths.size();
    } else {
        setState(State::Scanning);
        int64_t files = 0;
        for (const QString& p : std::as_const(m_paths))
            count(toNative(p), files);
        m_filesTotal = files;
    }
    setState(State::Running);

    for (const QString& p : std::as_const(m_paths)) {
        if (checkpoint())
            return;
        setCurrent(p);
        if (!m_toTrash) {
            removeTree(toNative(p));
            continue;
        }
        QString inTrash;
        if (QFile::moveToTrash(p, &inTrash)) {
            recordUndo(inTrash, p);
            m_filesDone += 1;
            continue;
        }
        if (!m_permanentAll) {
            PromptRequest req;
            req.kind = PromptRequest::Kind::TrashFailed;
            req.message = tr("Cannot move to trash. Delete permanently?");
            req.srcPath = p;
            const Answer a = prompt(req);
            if (a == Answer::DeletePermanentlyAll)
                m_permanentAll = true;
            else if (a == Answer::Skip) {
                m_filesDone += 1;
                continue;
            } else if (a != Answer::DeletePermanently)
                return;
        }
        removeTree(toNative(p));
        m_filesDone += 1;
    }
}

bool DeleteJob::removeTree(const QByteArray& path)
{
    if (checkpoint())
        return false;
    struct stat st;
    if (lstat(path.constData(), &st) != 0)
        return errno == ENOENT;
    bool ok = true;
    if (S_ISDIR(st.st_mode)) {
        std::vector<QByteArray> names;
        for (;;) {
            const int e = readNames(path, names);
            if (e == 0)
                break;
            if (askError(tr("Cannot read folder"), fromNative(path), e) != Answer::Retry)
                return false;
            names.clear();
        }
        for (const QByteArray& n : names) {
            if (!removeTree(joinPath(path, n)))
                ok = false;
            if (isCancelled())
                return false;
        }
        if (ok) {
            while (::rmdir(path.constData()) != 0) {
                if (askError(tr("Cannot delete folder"), fromNative(path), errno) != Answer::Retry) {
                    ok = false;
                    break;
                }
            }
        }
    } else {
        setCurrent(fromNative(path));
        while (::unlink(path.constData()) != 0) {
            if (askError(tr("Cannot delete"), fromNative(path), errno) != Answer::Retry) {
                ok = false;
                break;
            }
        }
    }
    if (!m_toTrash)
        m_filesDone += 1;
    return ok;
}

// ---------------------------------------------------------------- helpers

int makeDirectories(const QString& path)
{
    QByteArray p = toNative(path);
    if (p.isEmpty())
        return EINVAL;
    struct stat st;
    if (::stat(p.constData(), &st) == 0)
        return S_ISDIR(st.st_mode) ? EEXIST : ENOTDIR;
    for (qsizetype i = 1; i <= p.size(); ++i) {
        if (i == p.size() || p[i] == '/') {
            const QByteArray part = p.left(i);
            if (::mkdir(part.constData(), 0777) != 0 && errno != EEXIST)
                return errno;
        }
    }
    return 0;
}

int renameNoReplace(const QString& from, const QString& to)
{
    return moveNoReplace(toNative(from), toNative(to));
}

static int64_t sizeAt(int dirfd, const std::atomic<bool>* cancel)
{
    DIR* d = fdopendir(dirfd);
    if (!d) {
        ::close(dirfd);
        return 0;
    }
    int64_t total = 0;
    while (dirent* de = readdir(d)) {
        if (cancel && cancel->load(std::memory_order_relaxed))
            break;
        if (isDotOrDotDot(de->d_name))
            continue;
        struct stat st;
        if (fstatat(::dirfd(d), de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
            continue;
        if (S_ISREG(st.st_mode)) {
            total += st.st_size;
        } else if (S_ISDIR(st.st_mode)) {
            const int sub = ::openat(::dirfd(d), de->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (sub >= 0)
                total += sizeAt(sub, cancel);
        }
    }
    closedir(d);
    return total;
}

int64_t directorySize(const QString& path, const std::atomic<bool>* cancel)
{
    const int fd = ::open(toNative(path).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    const int64_t s = sizeAt(fd, cancel);
    if (cancel && cancel->load())
        return -1;
    return s;
}

}  // namespace tandem
