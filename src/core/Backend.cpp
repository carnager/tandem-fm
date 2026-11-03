#include "core/Backend.h"

#include "core/FileOps.h"
#include "core/Util.h"

#include <QHash>
#include <QMutex>
#include <QReadWriteLock>
#include <QRegularExpression>
#include <QStandardPaths>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tandem {

// ------------------------------------------------------------ local backend

namespace {

class LocalReader : public Reader {
public:
    explicit LocalReader(int fd) : m_fd(fd) {}
    ~LocalReader() override { ::close(m_fd); }
    int64_t read(char* buf, int64_t max) override
    {
        for (;;) {
            const ssize_t n = ::read(m_fd, buf, size_t(max));
            if (n >= 0)
                return n;
            if (errno != EINTR)
                return -errno;
        }
    }

private:
    int m_fd;
};

class LocalWriter : public Writer {
public:
    explicit LocalWriter(int fd) : m_fd(fd) {}
    ~LocalWriter() override
    {
        if (m_fd >= 0)
            ::close(m_fd);
    }
    int64_t write(const char* buf, int64_t len) override
    {
        int64_t done = 0;
        while (done < len) {
            const ssize_t n = ::write(m_fd, buf + done, size_t(len - done));
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return -errno;
            }
            done += n;
        }
        return done;
    }
    int finish(int64_t mtime) override
    {
        if (mtime > 0) {
            struct timespec ts[2] = {{0, UTIME_OMIT}, {time_t(mtime), 0}};
            ::futimens(m_fd, ts);
        }
        const int r = ::close(m_fd);
        m_fd = -1;
        return r == 0 ? 0 : errno;
    }

private:
    int m_fd;
};

class LocalBackend : public Backend {
public:
    bool isLocal() const override { return true; }
    ListResult list(const QString& path, const ListOptions& opt, const std::atomic<bool>* cancel) override
    {
        return listDirectory(path, opt, cancel);
    }
    int stat(const QString& path, FileStat& out) override
    {
        struct stat st;
        if (::lstat(toNative(path).constData(), &st) != 0)
            return errno;
        out.isLink = S_ISLNK(st.st_mode);
        out.isDir = S_ISDIR(st.st_mode);
        out.size = st.st_size;
        out.mtime = st.st_mtime;
        out.mode = st.st_mode;
        out.uid = st.st_uid;
        out.gid = st.st_gid;
        return 0;
    }
    std::unique_ptr<Reader> openRead(const QString& path, int& err) override
    {
        const int fd = ::open(toNative(path).constData(), O_RDONLY | O_CLOEXEC);
        err = fd < 0 ? errno : 0;
        return fd < 0 ? nullptr : std::make_unique<LocalReader>(fd);
    }
    std::unique_ptr<Writer> openWrite(const QString& path, bool exclusive, uint32_t mode, int& err) override
    {
        const int flags = O_WRONLY | O_CREAT | O_CLOEXEC | (exclusive ? O_EXCL : O_TRUNC);
        const int fd = ::open(toNative(path).constData(), flags, mode & 07777 ? mode & 07777 : 0644);
        err = fd < 0 ? errno : 0;
        return fd < 0 ? nullptr : std::make_unique<LocalWriter>(fd);
    }
    int mkdir(const QString& path, uint32_t mode) override
    {
        return ::mkdir(toNative(path).constData(), mode & 07777 ? mode & 07777 : 0777) == 0 ? 0 : errno;
    }
    int rename(const QString& from, const QString& to) override { return renameNoReplace(from, to); }
    int removeFile(const QString& path) override { return ::unlink(toNative(path).constData()) == 0 ? 0 : errno; }
    int removeDir(const QString& path) override { return ::rmdir(toNative(path).constData()) == 0 ? 0 : errno; }
    int readLink(const QString& path, QString& target) override
    {
        QByteArray buf(4096, '\0');
        const ssize_t n = ::readlink(toNative(path).constData(), buf.data(), size_t(buf.size()));
        if (n < 0)
            return errno;
        target = fromNative(buf.left(n));
        return 0;
    }
    int symlink(const QString& target, const QString& path) override
    {
        return ::symlink(toNative(target).constData(), toNative(path).constData()) == 0 ? 0 : errno;
    }
    int chmod(const QString& path, uint32_t mode) override
    {
        return ::chmod(toNative(path).constData(), mode & 07777) == 0 ? 0 : errno;
    }
    QString queueKey(const QString&) const override { return {}; }
};

struct Registry {
    QReadWriteLock lock;
    QHash<QString, std::shared_ptr<Backend>> backends;
};

Registry& registry()
{
    static Registry r;
    return r;
}

const QRegularExpression& uriPattern()
{
    // scheme://authority[/path]
    static const QRegularExpression rx(QStringLiteral("^([a-zA-Z][a-zA-Z0-9+.-]*)://([^/]*)(/.*)?$"));
    return rx;
}

}  // namespace

QString Backend::homeLocation(const QString& path)
{
    return locationRoot(path);
}

QString Backend::queueKey(const QString& path) const
{
    return locationAuthority(path);
}

namespace {
QMutex g_authMutex;
AuthHandler g_auth;
}  // namespace

void setAuthHandler(AuthHandler handler)
{
    QMutexLocker locker(&g_authMutex);
    g_auth = std::move(handler);
}

namespace {
thread_local bool t_remember = false;
}

bool lastAnswerRemember()
{
    return t_remember;
}

std::optional<QString> askAuth(const AuthQuestion& q)
{
    t_remember = false;
    AuthHandler h;
    {
        QMutexLocker locker(&g_authMutex);
        h = g_auth;
    }
    std::optional<QString> a = h ? h(q) : std::nullopt;
    if (a && a->endsWith(QLatin1String(kRememberMark))) {
        a->chop(int(qstrlen(kRememberMark)));
        t_remember = true;
    }
    return a;
}

Backend* localBackend()
{
    static LocalBackend local;
    return &local;
}

void registerBackend(const QString& scheme, std::shared_ptr<Backend> backend)
{
    Registry& r = registry();
    QWriteLocker locker(&r.lock);
    if (backend)
        r.backends.insert(scheme.toLower(), std::move(backend));
    else
        r.backends.remove(scheme.toLower());
}

bool hasBackend(const QString& scheme)
{
    Registry& r = registry();
    QReadLocker locker(&r.lock);
    return r.backends.contains(scheme.toLower());
}

Backend* backendFor(const QString& path)
{
    if (!isRemoteLocation(path))
        return nullptr;
    const QString scheme = path.left(path.indexOf(QLatin1Char(':'))).toLower();
    Registry& r = registry();
    QReadLocker locker(&r.lock);
    return r.backends.value(scheme).get();
}

Backend* anyBackendFor(const QString& path)
{
    if (!isRemoteLocation(path))
        return localBackend();
    return backendFor(path);
}

// --------------------------------------------------------------- locations

bool isRemoteLocation(const QString& path)
{
    if (path.isEmpty() || path.startsWith(QLatin1Char('/')))
        return false;
    return uriPattern().match(path).hasMatch();
}

QString locationAuthority(const QString& path)
{
    const auto m = uriPattern().match(path);
    return m.hasMatch() ? m.captured(1).toLower() + QStringLiteral("://") + m.captured(2) : QString();
}

QString locationRoot(const QString& path)
{
    const QString a = locationAuthority(path);
    return a.isEmpty() ? QStringLiteral("/") : a + QLatin1Char('/');
}

QString locationPath(const QString& path)
{
    const auto m = uriPattern().match(path);
    if (!m.hasMatch())
        return path;
    const QString p = m.captured(3);
    return p.isEmpty() ? QStringLiteral("/") : p;
}

QString normalizeLocation(const QString& uri)
{
    const auto m = uriPattern().match(uri.trimmed());
    if (!m.hasMatch())
        return uri;
    QStringList out;
    for (const QString& part : m.captured(3).split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (part == QLatin1String("."))
            continue;
        if (part == QLatin1String("..")) {
            if (!out.isEmpty())
                out.removeLast();
            continue;
        }
        out << part;
    }
    return m.captured(1).toLower() + QStringLiteral("://") + m.captured(2) + QLatin1Char('/') + out.join(QLatin1Char('/'));
}

QString remoteCachePath(const QString& uri)
{
    static const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) +
                                QStringLiteral("/tandem-fm/remote/");
    QString host = locationAuthority(uri);
    host.replace(QStringLiteral("://"), QStringLiteral("_"));
    host.replace(QLatin1Char('/'), QLatin1Char('_'));
    return base + host + locationPath(uri);
}

bool cachedCopyIsFresh(const QString& local, int64_t size, int64_t mtime)
{
    struct stat st;
    return ::stat(toNative(local).constData(), &st) == 0 && st.st_size == size && st.st_mtime == mtime;
}

// ----------------------------------------------------------------- helpers

int statAt(const QString& path, FileStat& out)
{
    Backend* b = anyBackendFor(path);
    return b ? b->stat(path, out) : ENOTSUP;
}

int makeDirectoriesAt(const QString& path)
{
    Backend* b = backendFor(path);
    if (!b)
        return isRemoteLocation(path) ? ENOTSUP : makeDirectories(path);
    FileStat st;
    if (b->stat(path, st) == 0)
        return st.isDir ? EEXIST : ENOTDIR;
    // Create from the deepest existing parent down.
    QStringList missing;
    QString p = path;
    const QString root = locationRoot(path);
    while (p.size() > root.size() && b->stat(p, st) != 0) {
        missing.prepend(p);
        p = parentPath(p);
    }
    for (const QString& m : std::as_const(missing))
        if (const int e = b->mkdir(m, 0777); e != 0 && e != EEXIST)
            return e;
    return 0;
}

int renameNoReplaceAt(const QString& from, const QString& to)
{
    Backend* b = backendFor(from);
    if (!b && !isRemoteLocation(from) && !isRemoteLocation(to))
        return renameNoReplace(from, to);
    if (!b || locationAuthority(from) != locationAuthority(to))
        return EXDEV;
    return b->rename(from, to);
}

int createEmptyFile(const QString& path)
{
    Backend* b = anyBackendFor(path);
    if (!b)
        return ENOTSUP;
    int err = 0;
    auto w = b->openWrite(path, true, 0644, err);
    if (!w)
        return err;
    return w->finish(0);
}

int64_t directorySizeAt(const QString& path, const std::atomic<bool>* cancel)
{
    Backend* b = backendFor(path);
    if (!b)
        return isRemoteLocation(path) ? -1 : directorySize(path, cancel);
    if (Backend::Tree tree; b->listTree(path, true, tree, cancel)) {  // one request for all of it
        int64_t total = 0;
        for (const auto& entries : std::as_const(tree))
            for (const FileEntry& e : entries)
                if (!e.isDir && !e.isLink)
                    total += qMax<int64_t>(0, e.size);
        return total;
    }
    ListOptions opt;
    opt.showHidden = true;
    const ListResult l = b->list(path, opt, cancel);
    if (l.error)
        return -1;
    int64_t total = 0;
    for (const FileEntry& e : l.entries) {
        if (cancel && cancel->load())
            return -1;
        if (e.isDir && !e.isLink) {
            const int64_t sub = directorySizeAt(joinPath(path, e.name), cancel);
            if (sub < 0)
                return -1;
            total += sub;
        } else if (!e.isLink) {
            total += qMax<int64_t>(0, e.size);
        }
    }
    return total;
}

int downloadFile(const QString& from, const QString& to, const std::atomic<bool>* cancel,
                 const std::function<void(int64_t, int64_t)>& progress)
{
    Backend* src = anyBackendFor(from);
    if (!src)
        return ENOTSUP;
    FileStat st;
    if (const int e = src->stat(from, st))
        return e;
    int err = 0;
    auto in = src->openRead(from, err);
    if (!in)
        return err;
    makeDirectories(parentPath(to));
    auto out = localBackend()->openWrite(to, false, 0644, err);
    if (!out)
        return err;
    std::vector<char> buf(1 << 20);
    int64_t done = 0;
    for (;;) {
        if (cancel && cancel->load()) {
            out.reset();
            ::unlink(toNative(to).constData());
            return ECANCELED;
        }
        const int64_t n = in->read(buf.data(), int64_t(buf.size()));
        if (n < 0) {
            out.reset();
            ::unlink(toNative(to).constData());
            return int(-n);
        }
        if (n == 0)
            break;
        if (const int64_t w = out->write(buf.data(), n); w < 0) {
            out.reset();
            ::unlink(toNative(to).constData());
            return int(-w);
        }
        done += n;
        if (progress)
            progress(done, st.size);
    }
    return out->finish(st.mtime);
}

}  // namespace tandem
