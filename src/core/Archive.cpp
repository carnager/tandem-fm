#include "core/Archive.h"

#include "core/FileOps.h"
#include "core/Util.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QMutex>
#include <QSet>
#include <QStandardPaths>

#include <archive.h>
#include <archive_entry.h>

#include <filesystem>

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

// libarchive supports zstd but not every release maps .tar.zst itself.
int setWriterFormat(archive* writer, const QByteArray& name)
{
    const QByteArray lower = name.toLower();
    if (lower.endsWith(".tar.zst") || lower.endsWith(".tzst")) {
        const int result = archive_write_set_format_pax_restricted(writer);
        return result == ARCHIVE_OK ? archive_write_add_filter_zstd(writer) : result;
    }
    return archive_write_set_format_filter_by_ext(writer, name.constData());
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
    if (setWriterFormat(a, target) != ARCHIVE_OK) {
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
    const bool replaced = ::lstat(target.constData(), &existing) == 0;
    if (::rename(part.constData(), target.constData()) != 0) {
        askError(tr("Cannot create archive"), m_archive, errno);
        ::unlink(part.constData());
        setState(State::Failed);
    } else if (!replaced) {
        recordUndo(fromNative(target));
    }
}

// ------------------------------------------------------------ archive update

void forgetArchiveIndex(const QString& archivePath)
{
    QMutexLocker lock(&g_cacheMutex);
    g_cache.removeIf([&](const CachedIndex& c) { return c.path == archivePath; });
}

bool canUpdateArchive(const QString& archivePath, QString* why)
{
    const QString n = baseName(archivePath).toLower();
    if (isSingleCompressed(n)) {
        if (why)
            *why = QObject::tr("This is one compressed file, not an archive: unpack it, then pack again");
        return false;
    }
    for (const char* e : {".rar", ".iso", ".cab", ".rpm", ".deb", ".lha", ".lzh", ".xar", ".ar"}) {
        if (n.endsWith(QLatin1String(e))) {
            if (why)
                *why = QObject::tr("Tandem can't write %1 archives").arg(QString::fromLatin1(e + 1).toUpper());
            return false;
        }
    }
    return true;  // zip, 7z, the tar family, cpio, and zip-based files (jar, odt, …)
}

namespace {

bool isUnder(const QString& path, const QString& root)
{
    return path == root || (path.size() > root.size() && path.startsWith(root) && path[root.size()] == QLatin1Char('/'));
}

QString replacePrefix(const QString& path, const QString& from, const QString& to)
{
    return to + path.mid(from.size());
}

// "a/b.txt" -> "a/b (2).txt", the first name nothing in the archive uses.
QString uniqueInner(const QString& path, const std::function<bool(const QString&)>& taken)
{
    const int slash = int(path.lastIndexOf(QLatin1Char('/')));
    const QString dir = path.left(slash + 1);
    const QString name = path.mid(slash + 1);
    int dot = int(name.lastIndexOf(QLatin1Char('.')));
    if (dot <= 0)
        dot = int(name.size());
    for (int i = 2;; ++i) {
        const QString c = dir + name.left(dot) + QStringLiteral(" (%1)").arg(i) + name.mid(dot);
        if (!taken(c))
            return c;
    }
}

// Writer in the same format as the reader (which has read its first header).
archive* writerLike(archive* in, bool haveHeader, const QByteArray& nameForExt)
{
    archive* out = archive_write_new();
    if (haveHeader) {
        int fmt = archive_format(in);
        if ((fmt & ARCHIVE_FORMAT_BASE_MASK) == ARCHIVE_FORMAT_TAR)
            fmt = ARCHIVE_FORMAT_TAR_PAX_RESTRICTED;  // ustar can't hold long names
        bool ok = archive_write_set_format(out, fmt) == ARCHIVE_OK ||
                  archive_write_set_format(out, fmt & ARCHIVE_FORMAT_BASE_MASK) == ARCHIVE_OK;
        for (int i = archive_filter_count(in) - 1; ok && i >= 0; --i) {
            const int code = archive_filter_code(in, i);
            if (code != ARCHIVE_FILTER_NONE)
                ok = archive_write_add_filter(out, code) == ARCHIVE_OK;
        }
        if (ok)
            return out;
        archive_write_free(out);
        out = archive_write_new();
    }
    if (setWriterFormat(out, nameForExt) == ARCHIVE_OK)
        return out;
    archive_write_free(out);
    return nullptr;
}

}  // namespace

ArchiveUpdateJob::ArchiveUpdateJob(QString archivePath, ArchiveChanges changes, QString title)
    : Job(title.isEmpty() ? tr("Update %1").arg(baseName(archivePath)) : title),
      m_archive(std::move(archivePath)),
      m_changes(std::move(changes))
{
}

void ArchiveUpdateJob::run()
{
    QStringList touched{m_archive};
    for (const auto& a : std::as_const(m_changes.add))
        touched << a.first;
    if (!waitForTurn(touched))
        return;
    setState(State::Scanning);
    auto fail = [&](const QString& what, const QString& why) {
        askErrorText(what, m_archive, why);
        setState(State::Failed);
    };
    QString why;
    if (!canUpdateArchive(m_archive, &why))
        return fail(tr("Cannot change the archive"), why);
    QString error;
    const auto idx = loadArchiveIndex(m_archive, &error);
    if (!idx)
        return fail(tr("Cannot read the archive"), error);
    for (const ArchiveEntry& e : idx->entries)
        if (e.encrypted)
            return fail(tr("Cannot change the archive"),
                        tr("It has encrypted files, which Tandem can't write back"));

    // What goes and what moves.
    QStringList removed;
    for (const QString& r : std::as_const(m_changes.remove))
        if (const QString n = normalizeArchivePath(r); !n.isEmpty())
            removed << n;
    QList<QPair<QString, QString>> renames;
    for (const auto& rn : std::as_const(m_changes.rename)) {
        const QString from = normalizeArchivePath(rn.first), to = normalizeArchivePath(rn.second);
        if (from.isEmpty() || to.isEmpty() || from == to)
            continue;
        if (!idx->find(from))
            addError(tr("%1 is not in the archive").arg(from));
        else if (idx->find(to))
            addError(tr("%1 exists already").arg(to));
        else if (isUnder(to, from))
            addError(tr("Cannot move %1 into itself").arg(from));
        else
            renames.append({from, to});
    }
    QStringList replaced;  // members (with contents) that added files replace
    auto mapped = [&](const QString& p) -> QString {  // a member's new path; empty: it goes
        for (const QString& r : std::as_const(removed))
            if (isUnder(p, r))
                return {};
        QString to = p;
        for (const auto& rn : std::as_const(renames))
            if (isUnder(p, rn.first)) {
                to = replacePrefix(p, rn.first, rn.second);
                break;
            }
        for (const QString& r : std::as_const(replaced))
            if (isUnder(to, r))
                return {};
        return to;
    };
    QHash<QString, const ArchiveEntry*> members;  // by their path after removes and renames
    for (const ArchiveEntry& e : idx->entries)
        if (const QString m = mapped(e.path); !m.isEmpty())
            members.insert(m, &e);

    // What comes in: the local items, folders walked.
    struct Item {
        QByteArray abs;
        QString inner;
        struct stat st;
    };
    std::vector<Item> items;
    std::function<void(const QByteArray&, const QString&)> walk = [&](const QByteArray& abs, const QString& inner) {
        if (isCancelled())
            return;
        Item it{abs, inner, {}};
        if (::lstat(abs.constData(), &it.st) != 0) {
            addError(tr("Cannot read: %1 (%2)").arg(fromNative(abs), errnoString(errno)));
            return;
        }
        items.push_back(it);
        if (!S_ISDIR(it.st.st_mode))
            return;
        const QStringList names = QDir(fromNative(abs)).entryList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
        for (const QString& n : names)
            walk(joinPath(abs, toNative(n)), inner + QLatin1Char('/') + n);
    };
    for (const auto& a : std::as_const(m_changes.add))
        if (const QString inner = normalizeArchivePath(a.second); !inner.isEmpty())
            walk(toNative(a.first), inner);
    if (isCancelled())
        return;

    // Clashes with members that stay.
    QSet<QString> newPaths;
    auto taken = [&](const QString& p) { return members.contains(p) || newPaths.contains(p); };
    QList<QPair<QString, QString>> renamedRoots;
    QStringList skippedRoots;  // inner paths
    QList<QByteArray> skippedSources;
    std::vector<Item> adding;
    int64_t bytes = 0;
    for (Item& it : items) {
        bool skip = false;
        for (const QString& s : std::as_const(skippedRoots))
            skip = skip || isUnder(it.inner, s);
        if (skip)
            continue;
        for (const auto& rr : std::as_const(renamedRoots))
            if (isUnder(it.inner, rr.first)) {
                it.inner = replacePrefix(it.inner, rr.first, rr.second);
                break;
            }
        const bool dir = S_ISDIR(it.st.st_mode);
        if (const ArchiveEntry* old = members.value(it.inner)) {
            if (dir && old->isDir)
                continue;  // folders merge
            if (m_changes.replaceExisting) {
                replaced << it.inner;
            } else {
                QByteArray dst = toNative(joinPath(m_archive, it.inner));
                const auto res = resolveConflict({fromNative(it.abs), dir ? -1 : int64_t(it.st.st_size), int64_t(it.st.st_mtime)},
                                                 dst, {joinPath(m_archive, it.inner), old->isDir ? -1 : old->size, old->mtime});
                if (!res)
                    return;  // cancelled
                if (*res == Resolution::Skip) {
                    skippedRoots << it.inner;
                    skippedSources << it.abs;
                    continue;
                }
                if (*res == Resolution::Rename) {
                    const QString fresh = uniqueInner(it.inner, taken);
                    renamedRoots.append({it.inner, fresh});
                    it.inner = fresh;
                } else {
                    replaced << it.inner;
                }
            }
        }
        newPaths.insert(it.inner);
        if (S_ISREG(it.st.st_mode))
            bytes += it.st.st_size;
        adding.push_back(it);
    }
    QStringList mkdirs;
    for (const QString& d : std::as_const(m_changes.mkdir)) {
        const QString n = normalizeArchivePath(d);
        if (n.isEmpty())
            continue;
        if (taken(n))
            addError(tr("%1 exists already").arg(n));
        else {
            mkdirs << n;
            newPaths.insert(n);
        }
    }
    int64_t files = int64_t(adding.size() + size_t(mkdirs.size()));
    for (const ArchiveEntry& e : idx->entries) {
        if (mapped(e.path).isEmpty())
            continue;
        files += 1;
        if (!e.isDir)
            bytes += e.size;
    }
    m_filesTotal = files;
    m_bytesTotal = bytes;

    // Write the new archive next to the old one, then swap.
    archive* in = openReader(m_archive, &error);
    if (!in)
        return fail(tr("Cannot read the archive"), error);
    archive_entry* entry = nullptr;
    int r = archive_read_next_header(in, &entry);
    const QByteArray target = toNative(m_archive);
    const QByteArray temp = joinPath(toNative(parentPath(m_archive)),
                                     "." + toNative(baseName(m_archive)) + ".tandem-" +
                                         QByteArray::number(QDateTime::currentMSecsSinceEpoch()));
    archive* out = writerLike(in, r == ARCHIVE_OK || r == ARCHIVE_WARN, target);
    if (!out) {
        archive_read_free(in);
        return fail(tr("Cannot change the archive"), tr("Tandem can't write this kind of archive"));
    }
    if (archive_write_open_filename(out, temp.constData()) != ARCHIVE_OK) {
        const QString w = archiveErrorText(out, tr("write error"));
        archive_write_free(out);
        archive_read_free(in);
        return fail(tr("Cannot change the archive"), w);
    }
    setState(State::Running);

    std::vector<char> buf(1 << 20);
    bool ok = true;
    for (; ok && (r == ARCHIVE_OK || r == ARCHIVE_WARN); r = archive_read_next_header(in, &entry)) {
        if (checkpoint()) {
            ok = false;
            break;
        }
        const QString path = memberPath(entry, m_archive);
        if (!path.isEmpty()) {  // unsafe names ("../x") stay as they are
            const QString to = mapped(path);
            if (to.isEmpty())
                continue;  // removed or replaced: the next header skips its data
            setCurrent(to);
            if (to != path)
                archive_entry_set_pathname_utf8(
                    entry, (to + (archive_entry_filetype(entry) == AE_IFDIR ? QStringLiteral("/") : QString())).toUtf8().constData());
            if (const char* hl = archive_entry_hardlink_utf8(entry)) {
                const QString h = normalizeArchivePath(QString::fromUtf8(hl));
                if (const QString hm = h.isEmpty() ? h : mapped(h); !hm.isEmpty() && hm != h)
                    archive_entry_set_hardlink_utf8(entry, hm.toUtf8().constData());
            }
        }
        if (archive_write_header(out, entry) < ARCHIVE_WARN) {
            askErrorText(tr("Cannot write"), path, archiveErrorText(out, tr("write error")));
            ok = false;
            break;
        }
        for (;;) {
            const la_ssize_t n = archive_read_data(in, buf.data(), buf.size());
            if (n == 0)
                break;
            if (n < 0) {
                askErrorText(tr("Cannot read"), path, archiveErrorText(in, tr("Damaged archive")));
                ok = false;
                break;
            }
            if (archive_write_data(out, buf.data(), size_t(n)) < 0) {
                askErrorText(tr("Cannot write"), path, archiveErrorText(out, tr("write error")));
                ok = false;
                break;
            }
            m_bytesDone += n;
            if (checkpoint()) {
                ok = false;
                break;
            }
        }
        m_filesDone += 1;
    }
    if (ok && r < ARCHIVE_WARN && r != ARCHIVE_EOF) {
        askErrorText(tr("Cannot read the archive"), m_archive, archiveErrorText(in, tr("Damaged archive")));
        ok = false;
    }
    archive_read_free(in);

    const int64_t now = QDateTime::currentSecsSinceEpoch();
    for (const QString& d : std::as_const(mkdirs)) {
        if (!ok)
            break;
        archive_entry* e = archive_entry_new();
        archive_entry_set_pathname_utf8(e, (d + QLatin1Char('/')).toUtf8().constData());
        archive_entry_set_filetype(e, AE_IFDIR);
        archive_entry_set_perm(e, 0755);
        archive_entry_set_mtime(e, now, 0);
        if (archive_write_header(out, e) < ARCHIVE_WARN) {
            askErrorText(tr("Cannot create folder"), d, archiveErrorText(out, tr("write error")));
            ok = false;
        }
        archive_entry_free(e);
        m_filesDone += 1;
    }

    archive* disk = archive_read_disk_new();
    archive_read_disk_set_standard_lookup(disk);
    for (const Item& it : adding) {
        if (!ok || checkpoint()) {
            ok = false;
            break;
        }
        setCurrent(fromNative(it.abs));
        archive_entry* e = archive_entry_new();
        archive_entry_copy_sourcepath(e, it.abs.constData());
        const int fd = S_ISREG(it.st.st_mode) ? ::open(it.abs.constData(), O_RDONLY | O_CLOEXEC) : -1;
        archive_read_disk_entry_from_file(disk, e, fd, &it.st);
        archive_entry_set_pathname_utf8(e, it.inner.toUtf8().constData());
        if (S_ISREG(it.st.st_mode) && fd < 0) {
            const int err = errno;
            archive_entry_free(e);
            if (askError(tr("Cannot read"), fromNative(it.abs), err) == Answer::Skip) {
                skippedSources << it.abs;
                continue;
            }
            ok = false;
            break;
        }
        if (archive_write_header(out, e) < ARCHIVE_WARN) {
            const QString w = archiveErrorText(out, tr("write error"));
            archive_entry_free(e);
            if (fd >= 0)
                ::close(fd);
            askErrorText(tr("Cannot add"), fromNative(it.abs), w);
            ok = false;
            break;
        }
        for (; fd >= 0;) {
            const ssize_t n = ::read(fd, buf.data(), buf.size());
            if (n <= 0)
                break;
            if (archive_write_data(out, buf.data(), size_t(n)) < 0) {
                askErrorText(tr("Cannot add"), fromNative(it.abs), archiveErrorText(out, tr("write error")));
                ok = false;
                break;
            }
            m_bytesDone += n;
            if (checkpoint()) {
                ok = false;
                break;
            }
        }
        if (fd >= 0)
            ::close(fd);
        archive_entry_free(e);
        m_filesDone += 1;
    }
    archive_read_free(disk);
    if (ok && archive_write_close(out) != ARCHIVE_OK) {
        askErrorText(tr("Cannot finish the archive"), m_archive, archiveErrorText(out, tr("write error")));
        ok = false;
    }
    archive_write_free(out);
    if (!ok) {
        ::unlink(temp.constData());
        if (!isCancelled())
            setState(State::Failed);
        return;
    }
    struct stat orig;
    if (::stat(target.constData(), &orig) == 0)
        ::chmod(temp.constData(), orig.st_mode & 07777);
    if (::rename(temp.constData(), target.constData()) != 0) {
        const int err = errno;
        ::unlink(temp.constData());
        askError(tr("Cannot replace the archive"), m_archive, err);
        setState(State::Failed);
        return;
    }
    forgetArchiveIndex(m_archive);

    if (m_changes.moveSources) {  // only what fully went in
        for (const auto& a : std::as_const(m_changes.add)) {
            const QByteArray src = toNative(a.first);
            bool partial = false;
            for (const QByteArray& s : std::as_const(skippedSources))
                partial = partial || s == src || s.startsWith(src + '/');
            if (partial) {
                addError(tr("%1 was kept: not all of it went into the archive").arg(a.first));
                continue;
            }
            std::error_code ec;
            std::filesystem::remove_all(std::filesystem::path(src.toStdString()), ec);
            if (ec)
                addError(tr("Cannot remove %1 (%2)").arg(a.first, QString::fromStdString(ec.message())));
        }
    }
}

}  // namespace tandem
