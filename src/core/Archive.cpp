#include "core/Archive.h"

#include "core/FileOps.h"
#include "core/Util.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QMutex>
#include <QStandardPaths>

#include <archive.h>
#include <archive_entry.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tandem {

namespace {

// Longest first, so ".tar.gz" wins over ".gz".
const QStringList& archiveExtensions()
{
    static const QStringList exts = {
        ".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tar.lz", ".tar.lzma", ".tar.lz4", ".tar.z",
        ".tgz",    ".tbz",     ".tbz2",   ".txz",     ".tzst",   ".tlz",      ".zip",     ".7z",
        ".rar",    ".tar",     ".iso",    ".cab",     ".cpio",   ".rpm",      ".deb",     ".lha",
        ".lzh",    ".xar",     ".ar",     ".gz",      ".bz2",    ".xz",       ".zst",     ".lz4",
        ".lzma",   ".lz",      ".z",
    };
    return exts;
}

// A single compressed file (not a tar): libarchive's "raw" format shows it
// as one member.
bool isSingleCompressed(const QString& name)
{
    const QString n = name.toLower();
    if (n.contains(QLatin1String(".tar.")))
        return false;
    for (const char* e : {".gz", ".bz2", ".xz", ".zst", ".lz4", ".lzma", ".lz", ".z"})
        if (n.endsWith(QLatin1String(e)))
            return true;
    return false;
}

QString archiveErrorText(archive* a, const QString& fallback)
{
    const char* s = archive_error_string(a);
    return s ? QString::fromUtf8(s) : fallback;
}

// Configured reader, or nullptr with *error set.
archive* openReader(const QString& path, QString* error)
{
    archive* a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    if (isSingleCompressed(baseName(path)))
        archive_read_support_format_raw(a);
    if (archive_read_open_filename(a, toNative(path).constData(), 64 * 1024) != ARCHIVE_OK) {
        if (error)
            *error = archiveErrorText(a, QObject::tr("Not a readable archive"));
        archive_read_free(a);
        return nullptr;
    }
    return a;
}

QString memberPath(archive_entry* e, const QString& archivePath)
{
    const char* utf8 = archive_entry_pathname_utf8(e);
    QString p = utf8 ? QString::fromUtf8(utf8) : QFile::decodeName(archive_entry_pathname(e));
    if (archive_entry_filetype(e) == AE_IFREG && p == QLatin1String("data") && isSingleCompressed(baseName(archivePath)))
        p = archiveStem(baseName(archivePath));  // raw format names its member "data"
    return normalizeArchivePath(p);
}

struct CachedIndex {
    QString path;
    int64_t size, mtime;
    std::shared_ptr<const ArchiveIndex> index;
};
QMutex g_cacheMutex;
QList<CachedIndex> g_cache;  // most recently used last
constexpr int kCachedArchives = 6;

QString extractCacheDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/extract");
}

// No component between base and the final name may be a symlink: an archive
// must not be able to write outside the target folder through a link it
// created itself.
bool parentsAreSafe(const QByteArray& base, const QString& rel)
{
    const QStringList parts = rel.split(QLatin1Char('/'));
    QByteArray p = base;
    for (int i = 0; i + 1 < parts.size(); ++i) {
        p = joinPath(p, toNative(parts[i]));
        struct stat st;
        if (::lstat(p.constData(), &st) == 0 && S_ISLNK(st.st_mode))
            return false;
    }
    return true;
}

}  // namespace

const ArchiveEntry* ArchiveIndex::find(const QString& path) const
{
    const auto it = byPath.constFind(path);
    return it == byPath.cend() ? nullptr : &entries[size_t(*it)];
}

bool isArchiveName(const QString& fileName)
{
    const QString n = fileName.toLower();
    for (const QString& e : archiveExtensions())
        if (n.endsWith(e) && n.size() > e.size())
            return true;
    return false;
}

QString archiveStem(const QString& fileName)
{
    const QString n = fileName.toLower();
    for (const QString& e : archiveExtensions())
        if (n.endsWith(e) && n.size() > e.size())
            return fileName.left(fileName.size() - e.size());
    return fileName;
}

QString normalizeArchivePath(QString p)
{
    QStringList out;
    for (const QString& part : p.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (part == QLatin1String("."))
            continue;
        if (part == QLatin1String(".."))
            return {};  // never let an archive climb out of its folder
        out.append(part);
    }
    return out.join(QLatin1Char('/'));
}

std::shared_ptr<const ArchiveIndex> loadArchiveIndex(const QString& archivePath, QString* error,
                                                     const std::atomic<bool>* cancel)
{
    struct stat st;
    if (::stat(toNative(archivePath).constData(), &st) != 0) {
        *error = errnoString(errno);
        return nullptr;
    }
    {
        QMutexLocker lock(&g_cacheMutex);
        for (int i = 0; i < g_cache.size(); ++i) {
            const CachedIndex& c = g_cache[i];
            if (c.path == archivePath && c.size == st.st_size && c.mtime == st.st_mtime) {
                auto idx = c.index;
                g_cache.move(i, g_cache.size() - 1);
                return idx;
            }
        }
    }

    archive* a = openReader(archivePath, error);
    if (!a)
        return nullptr;
    auto idx = std::make_shared<ArchiveIndex>();
    idx->archivePath = archivePath;
    idx->archiveSize = st.st_size;
    idx->archiveMtime = st.st_mtime;

    auto add = [&](ArchiveEntry e) {
        const auto it = idx->byPath.constFind(e.path);
        if (it != idx->byPath.cend()) {
            ArchiveEntry& old = idx->entries[size_t(*it)];
            if (old.isDir && e.isDir && e.mtime == 0)
                return;  // keep the real folder entry
            old = std::move(e);  // later members replace earlier ones (tar appends)
            return;
        }
        idx->byPath.insert(e.path, int(idx->entries.size()));
        idx->entries.push_back(std::move(e));
    };

    archive_entry* entry = nullptr;
    int r;
    while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
        if (cancel && cancel->load()) {
            archive_read_free(a);
            *error = QObject::tr("Cancelled");
            return nullptr;
        }
        const QString path = memberPath(entry, archivePath);
        if (path.isEmpty())
            continue;
        ArchiveEntry e;
        e.path = path;
        e.name = path.section(QLatin1Char('/'), -1);
        const auto type = archive_entry_filetype(entry);
        e.isDir = type == AE_IFDIR;
        e.isLink = type == AE_IFLNK;
        e.size = e.isDir ? 0 : (archive_entry_size_is_set(entry) ? archive_entry_size(entry) : 0);
        e.mtime = archive_entry_mtime(entry);
        e.mode = archive_entry_mode(entry);
        if ((e.mode & S_IFMT) == 0)
            e.mode |= e.isDir ? S_IFDIR : S_IFREG;
        e.encrypted = archive_entry_is_encrypted(entry);
        add(std::move(e));
    }
    if (r == ARCHIVE_FATAL || r == ARCHIVE_FAILED) {
        *error = archiveErrorText(a, QObject::tr("Damaged archive"));
        archive_read_free(a);
        return nullptr;
    }
    archive_read_free(a);

    // Folders that only exist implicitly ("a/b/c.txt" without "a/" and "a/b/").
    const size_t real = idx->entries.size();
    for (size_t i = 0; i < real; ++i) {
        QString p = idx->entries[i].path;
        int slash;
        while ((slash = int(p.lastIndexOf(QLatin1Char('/')))) > 0) {
            p.truncate(slash);
            if (idx->byPath.contains(p))
                break;
            ArchiveEntry d;
            d.path = p;
            d.name = p.section(QLatin1Char('/'), -1);
            d.isDir = true;
            d.mode = S_IFDIR | 0755;
            d.mtime = st.st_mtime;
            idx->byPath.insert(p, int(idx->entries.size()));
            idx->entries.push_back(d);
        }
    }

    // Children lists and folder sizes.
    for (size_t i = 0; i < idx->entries.size(); ++i) {
        const ArchiveEntry& e = idx->entries[i];
        const int slash = int(e.path.lastIndexOf(QLatin1Char('/')));
        idx->children[slash < 0 ? QString() : e.path.left(slash)].append(int(i));
        if (e.isDir)
            continue;
        QString p = e.path;
        int s;
        while ((s = int(p.lastIndexOf(QLatin1Char('/')))) > 0) {
            p.truncate(s);
            if (const auto it = idx->byPath.constFind(p); it != idx->byPath.cend())
                idx->entries[size_t(*it)].size += e.size;
        }
    }

    std::shared_ptr<const ArchiveIndex> result = idx;
    QMutexLocker lock(&g_cacheMutex);
    g_cache.append({archivePath, int64_t(st.st_size), int64_t(st.st_mtime), result});
    while (g_cache.size() > kCachedArchives)
        g_cache.removeFirst();
    return result;
}

// --------------------------------------------------------- single extract

QString extractToCache(const QString& archivePath, const QString& innerPath, QString* error,
                       const std::atomic<bool>* cancel)
{
    struct stat st;
    if (::stat(toNative(archivePath).constData(), &st) != 0) {
        *error = errnoString(errno);
        return {};
    }
    const QString inner = normalizeArchivePath(innerPath);
    const QByteArray key = (archivePath + QLatin1Char('\n') + inner + QLatin1Char('\n') +
                            QString::number(st.st_mtime) + QLatin1Char('/') + QString::number(st.st_size))
                               .toUtf8();
    const QString dir = extractCacheDir() + QLatin1Char('/') +
                        QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex().left(16));
    const QString target = dir + QLatin1Char('/') + inner.section(QLatin1Char('/'), -1);
    if (QFileInfo::exists(target))
        return target;  // already extracted from this exact archive version
    QDir().mkpath(dir);

    archive* a = openReader(archivePath, error);
    if (!a)
        return {};
    archive_entry* entry = nullptr;
    int r;
    while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
        if (cancel && cancel->load())
            break;
        if (memberPath(entry, archivePath) != inner)
            continue;
        const QString part = target + QStringLiteral(".part");
        const int fd = ::open(toNative(part).constData(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0) {
            *error = errnoString(errno);
            archive_read_free(a);
            return {};
        }
        QByteArray buf(256 * 1024, Qt::Uninitialized);
        bool ok = true;
        for (;;) {
            const la_ssize_t n = archive_read_data(a, buf.data(), size_t(buf.size()));
            if (n == 0)
                break;
            if (n < 0 || (cancel && cancel->load())) {
                *error = archiveErrorText(a, QObject::tr("Cannot extract"));
                ok = false;
                break;
            }
            if (::write(fd, buf.constData(), size_t(n)) != n) {
                *error = errnoString(errno);
                ok = false;
                break;
            }
        }
        ::close(fd);
        archive_read_free(a);
        if (!ok) {
            QFile::remove(part);
            return {};
        }
        // Read-only, so nobody edits a temp copy thinking it's the original.
        ::chmod(toNative(part).constData(), 0400);
        ::rename(toNative(part).constData(), toNative(target).constData());
        return target;
    }
    if (error->isEmpty())
        *error = (r == ARCHIVE_FATAL) ? archiveErrorText(a, QObject::tr("Damaged archive"))
                                      : QObject::tr("Not found in the archive");
    archive_read_free(a);
    return {};
}

void pruneExtractCache()
{
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-1);
    QDirIterator it(extractCacheDir(), QDir::Dirs | QDir::NoDotAndDotDot);
    while (it.hasNext()) {
        const QString d = it.next();
        if (QFileInfo(d).lastModified() < cutoff) {
            QDir sub(d);
            for (const QString& f : sub.entryList(QDir::Files))
                ::chmod(toNative(sub.filePath(f)).constData(), 0600);
            sub.removeRecursively();
        }
    }
}

// --------------------------------------------------------------- ExtractJob

ExtractJob::ExtractJob(QString archivePath, QStringList innerPaths, QString destDir, bool smartFolder)
    : Job(tr("Extract")),
      m_archive(std::move(archivePath)),
      m_inner(std::move(innerPaths)),
      m_destDir(std::move(destDir)),
      m_smart(smartFolder)
{
}

void ExtractJob::run()
{
    if (!waitForTurn({m_archive, m_destDir}))
        return;
    setState(State::Scanning);
    setCurrent(m_archive);
    QString error;
    const auto idx = loadArchiveIndex(m_archive, &error);
    if (!idx) {
        askErrorText(tr("Cannot read archive"), m_archive, error);
        setState(State::Failed);
        return;
    }

    // Selected items, and the folder each one is relative to.
    struct Pick {
        QString path;    // "" = everything
        QString parent;  // stripped from member paths
    };
    QList<Pick> picks;
    bool whole = false;
    for (const QString& p : std::as_const(m_inner)) {
        const QString n = normalizeArchivePath(p);
        if (n.isEmpty())
            whole = true;
        const int slash = int(n.lastIndexOf(QLatin1Char('/')));
        picks.append({n, slash < 0 ? QString() : n.left(slash)});
    }
    auto relativeFor = [&](const QString& member) -> QString {
        for (const Pick& p : std::as_const(picks)) {
            if (p.path.isEmpty())
                return member;
            if (member == p.path || member.startsWith(p.path + QLatin1Char('/')))
                return p.parent.isEmpty() ? member : member.mid(p.parent.size() + 1);
        }
        return {};
    };

    QString base = m_destDir;
    if (whole && m_smart) {
        const QList<int> top = idx->children.value(QString());
        const bool singleFolder = top.size() == 1 && idx->entries[size_t(top.first())].isDir;
        if (!singleFolder)
            base = joinPath(m_destDir, archiveStem(baseName(m_archive)));
    }
    const QByteArray nbase = toNative(base);

    int64_t files = 0, bytes = 0;
    for (const ArchiveEntry& e : idx->entries) {
        if (relativeFor(e.path).isEmpty())
            continue;
        ++files;
        if (!e.isDir)
            bytes += e.size;
    }
    m_filesTotal = files;
    m_bytesTotal = bytes;
    if (int e = makeDirectories(base); e && e != EEXIST) {
        askError(tr("Cannot create folder"), base, e);
        setState(State::Failed);
        return;
    }
    setState(State::Running);

    archive* a = openReader(m_archive, &error);
    if (!a) {
        askErrorText(tr("Cannot read archive"), m_archive, error);
        setState(State::Failed);
        return;
    }
    QByteArray buf(1024 * 1024, Qt::Uninitialized);
    int64_t remaining = files;
    archive_entry* entry = nullptr;
    int r;
    while (remaining > 0 && ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK || r == ARCHIVE_WARN)) {
        if (checkpoint())
            break;
        const QString member = memberPath(entry, m_archive);
        const QString rel = member.isEmpty() ? QString() : relativeFor(member);
        if (rel.isEmpty())
            continue;  // not selected; libarchive skips the data
        --remaining;
        setCurrent(member);
        QByteArray target = joinPath(nbase, toNative(rel));
        if (!parentsAreSafe(nbase, rel)) {
            addError(tr("Skipped (path goes through a link): %1").arg(member));
            m_filesDone += 1;
            continue;
        }
        const auto type = archive_entry_filetype(entry);
        const int64_t size = archive_entry_size(entry);
        const int64_t mtime = archive_entry_mtime(entry);
        const mode_t perm = (archive_entry_perm(entry) & 07777) ?: (type == AE_IFDIR ? 0755 : 0644);

        if (type == AE_IFDIR) {
            makeDirectories(fromNative(target));
            ::chmod(target.constData(), perm | S_IRWXU);
            m_filesDone += 1;
            continue;
        }
        makeDirectories(parentPath(fromNative(target)));

        auto handleExisting = [&]() -> int {  // 1 = try again, 0 = skip, -1 = cancel
            struct stat ds;
            if (::lstat(target.constData(), &ds) != 0)
                return 1;
            const auto res = resolveConflict({m_archive + QLatin1Char('/') + member, size, mtime}, target,
                                             {fromNative(target), S_ISDIR(ds.st_mode) ? -1 : int64_t(ds.st_size),
                                              int64_t(ds.st_mtime)});
            if (!res)
                return -1;
            if (*res == Resolution::Skip)
                return 0;
            if (*res == Resolution::Overwrite) {
                if (S_ISDIR(ds.st_mode)) {
                    addError(tr("Cannot replace a folder with a file: %1").arg(fromNative(target)));
                    return 0;
                }
                ::unlink(target.constData());
            }
            return 1;
        };

        if (type == AE_IFLNK) {
            const char* linkTarget = archive_entry_symlink(entry);
            bool done = false;
            while (!done) {
                if (::symlink(linkTarget ? linkTarget : "", target.constData()) == 0)
                    break;
                if (errno != EEXIST) {
                    askError(tr("Cannot create link"), fromNative(target), errno);
                    break;
                }
                const int h = handleExisting();
                if (h <= 0)
                    done = true;
            }
            m_filesDone += 1;
            if (isCancelled())
                break;
            continue;
        }
        if (const char* hard = archive_entry_hardlink(entry)) {
            const QString linkRel = relativeFor(normalizeArchivePath(QString::fromUtf8(hard)));
            if (!linkRel.isEmpty() && ::link(joinPath(nbase, toNative(linkRel)).constData(), target.constData()) == 0) {
                m_filesDone += 1;
                continue;
            }
        }
        if (type != AE_IFREG) {
            addError(tr("Skipped special file: %1").arg(member));
            m_filesDone += 1;
            continue;
        }

        int fd = -1;
        bool skip = false;
        for (;;) {
            fd = ::open(target.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (fd >= 0)
                break;
            if (errno == EEXIST) {
                const int h = handleExisting();
                if (h < 0) {
                    archive_read_free(a);
                    return;
                }
                if (h == 0) {
                    skip = true;
                    break;
                }
                continue;
            }
            if (askError(tr("Cannot create"), fromNative(target), errno) != Answer::Retry) {
                skip = true;
                break;
            }
        }
        if (skip) {
            m_filesDone += 1;
            m_bytesDone += qMax<int64_t>(0, size);
            if (isCancelled())
                break;
            continue;
        }

        bool ok = true;
        for (;;) {
            if (checkpoint()) {
                ok = false;
                break;
            }
            const la_ssize_t n = archive_read_data(a, buf.data(), size_t(buf.size()));
            if (n == 0)
                break;
            if (n < 0) {
                const QString why = archive_entry_is_encrypted(entry)
                                        ? tr("encrypted archives are not supported yet")
                                        : archiveErrorText(a, tr("damaged data"));
                askErrorText(tr("Cannot extract"), member, why);
                ok = false;
                break;
            }
            if (::write(fd, buf.constData(), size_t(n)) != n) {
                askError(tr("Cannot write"), fromNative(target), errno);
                ok = false;
                break;
            }
            m_bytesDone += n;
        }
        ::fchmod(fd, perm);
        const struct timespec ts[2] = {{mtime, 0}, {mtime, 0}};
        ::futimens(fd, ts);
        ::close(fd);
        if (!ok)
            ::unlink(target.constData());
        m_filesDone += 1;
        if (isCancelled())
            break;
    }
    archive_read_free(a);
    if (!isCancelled()) {
        m_filesDone = m_filesTotal.load();
        m_bytesDone = m_bytesTotal.load();
    }
}

// ------------------------------------------------------------------ PackJob

PackJob::PackJob(QStringList sources, QString archivePath)
    : Job(tr("Pack")), m_sources(std::move(sources)), m_archive(std::move(archivePath))
{
}

void PackJob::run()
{
    if (!waitForTurn(m_sources + QStringList{parentPath(m_archive)}))
        return;
    setState(State::Scanning);
    struct Item {
        QByteArray abs;
        QString rel;
        struct stat st;
    };
    std::vector<Item> items;
    int64_t bytes = 0;
    std::function<void(const QByteArray&, const QString&)> walk = [&](const QByteArray& abs, const QString& rel) {
        if (isCancelled())
            return;
        Item it{abs, rel, {}};
        if (::lstat(abs.constData(), &it.st) != 0) {
            addError(tr("Cannot read: %1 (%2)").arg(fromNative(abs), errnoString(errno)));
            return;
        }
        items.push_back(it);
        if (S_ISREG(it.st.st_mode))
            bytes += it.st.st_size;
        if (!S_ISDIR(it.st.st_mode))
            return;
        QDir dir(fromNative(abs));
        const QStringList names = dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                                                QDir::Name);
        for (const QString& n : names)
            walk(joinPath(abs, toNative(n)), rel + QLatin1Char('/') + n);
    };
    for (const QString& s : std::as_const(m_sources))
        walk(toNative(s), baseName(s));
    if (isCancelled())
        return;
    m_filesTotal = int64_t(items.size());
    m_bytesTotal = bytes;

    QByteArray target = toNative(m_archive);
    struct stat existing;
    if (::lstat(target.constData(), &existing) == 0) {
        const auto res = resolveConflict({tr("New archive"), bytes, QDateTime::currentSecsSinceEpoch()}, target,
                                         {m_archive, int64_t(existing.st_size), int64_t(existing.st_mtime)});
        if (!res || *res == Resolution::Skip)
            return;
    }
    const QByteArray part = target + ".part";

    archive* a = archive_write_new();
    if (archive_write_set_format_filter_by_ext(a, target.constData()) != ARCHIVE_OK) {
        askErrorText(tr("Unknown archive type"), m_archive,
                     tr("use .zip, .7z, .tar, .tar.gz, .tar.xz, .tar.zst or .tar.bz2"));
        archive_write_free(a);
        setState(State::Failed);
        return;
    }
    if (archive_write_open_filename(a, part.constData()) != ARCHIVE_OK) {
        askErrorText(tr("Cannot create archive"), m_archive, archiveErrorText(a, tr("write error")));
        archive_write_free(a);
        setState(State::Failed);
        return;
    }
    archive* disk = archive_read_disk_new();
    archive_read_disk_set_standard_lookup(disk);
    setState(State::Running);

    QByteArray buf(1024 * 1024, Qt::Uninitialized);
    bool ok = true;
    for (const Item& it : items) {
        if (checkpoint()) {
            ok = false;
            break;
        }
        setCurrent(fromNative(it.abs));
        archive_entry* e = archive_entry_new();
        archive_entry_copy_sourcepath(e, it.abs.constData());
        const int fd = S_ISREG(it.st.st_mode) ? ::open(it.abs.constData(), O_RDONLY | O_CLOEXEC) : -1;
        archive_read_disk_entry_from_file(disk, e, fd, &it.st);
        archive_entry_set_pathname_utf8(e, it.rel.toUtf8().constData());
        if (archive_write_header(a, e) != ARCHIVE_OK) {
            const QString why = archiveErrorText(a, tr("write error"));
            archive_entry_free(e);
            if (fd >= 0)
                ::close(fd);
            if (askErrorText(tr("Cannot add"), fromNative(it.abs), why) == Answer::Skip)
                continue;
            ok = false;
            break;
        }
        if (fd >= 0) {
            for (;;) {
                const ssize_t n = ::read(fd, buf.data(), size_t(buf.size()));
                if (n <= 0)
                    break;
                if (archive_write_data(a, buf.constData(), size_t(n)) < 0) {
                    ok = false;
                    break;
                }
                m_bytesDone += n;
                if (checkpoint()) {
                    ok = false;
                    break;
                }
            }
            ::close(fd);
        }
        archive_entry_free(e);
        m_filesDone += 1;
        if (!ok)
            break;
    }
    archive_read_free(disk);
    if (ok && archive_write_close(a) != ARCHIVE_OK) {
        askErrorText(tr("Cannot finish archive"), m_archive, archiveErrorText(a, tr("write error")));
        ok = false;
    }
    archive_write_free(a);
    if (!ok) {
        ::unlink(part.constData());
        if (!isCancelled())
            setState(State::Failed);
        return;
    }
    if (::rename(part.constData(), target.constData()) != 0) {
        askError(tr("Cannot create archive"), m_archive, errno);
        ::unlink(part.constData());
        setState(State::Failed);
    }
}

}  // namespace tandem
