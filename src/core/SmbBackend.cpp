// smb://[user@]server[:port]/share/path through Samba's libsmbclient.
//
// A small pool of client contexts per server; each is used by one thread at
// a time (one libsmbclient context isn't thread-safe; separate ones are). libsmbclient pipelines large reads and writes
// itself, so a transfer just reads and writes big blocks. Logins: Kerberos
// if you have a ticket, then guest, then a user/password asked once per
// server and kept for the session.

#include "core/Backend.h"
#include "core/Keyring.h"
#include "core/Util.h"

#ifdef TANDEM_HAVE_SMBCLIENT

#include <QUrl>

#include <condition_variable>
#include <map>
#include <mutex>
#include <vector>

#include <fcntl.h>
#include <libsmbclient.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>

namespace tandem {

namespace {

constexpr int kMaxContexts = 6;  // per server
thread_local int t_smbLeases = 0;

struct Credentials {
    QString domain, user, password;
};

struct SmbServer {
    std::mutex mutex;
    std::condition_variable freed;
    std::vector<SMBCCTX*> idle;
    int open = 0;
    std::mutex loginMutex;
    std::optional<Credentials> creds;  // nullopt: Kerberos / guest
    bool verified = false;             // something worked with the current login
    int generation = 0;                // bumps when the login changes
    bool keyringTried = false;
    bool rememberWhenVerified = false; // "remember" was ticked: store once it works
};

struct CtxData {
    SmbServer* server;
    int generation;
};

void authCallback(SMBCCTX* c, const char*, const char*, char* wg, int wglen, char* un, int unlen, char* pw, int pwlen)
{
    auto* data = static_cast<CtxData*>(smbc_getOptionUserData(c));
    if (!data || !data->server->creds)
        return;  // leave as is: Kerberos or guest
    const Credentials& cr = *data->server->creds;
    auto put = [](char* dst, int len, const QString& v) {
        const QByteArray u = v.toUtf8();
        if (len > 0) {
            const int n = qMin(int(u.size()), len - 1);
            memcpy(dst, u.constData(), size_t(n));
            dst[n] = 0;
        }
    };
    if (!cr.domain.isEmpty())
        put(wg, wglen, cr.domain);
    put(un, unlen, cr.user);
    put(pw, pwlen, cr.password);
}

// libsmbclient keeps global state (configuration, talloc stack frames) and
// this build has no thread setup (smbc_thread_posix isn't exported): calls
// from two threads at once fail at random with EINVAL. So one call at a time,
// process-wide; a call is a listing or one block of a transfer, so listings
// still get through while a copy runs.
// First come, first served: a copy can't starve a listing.
class FairLock {
public:
    void lock()
    {
        std::unique_lock<std::mutex> l(m_mutex);
        const uint64_t ticket = m_next++;
        m_turn.wait(l, [&] { return m_serving == ticket; });
    }
    void unlock()
    {
        {
            std::lock_guard<std::mutex> l(m_mutex);
            ++m_serving;
        }
        m_turn.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_turn;
    uint64_t m_next = 0;
    uint64_t m_serving = 0;
};
FairLock g_smbCalls;
using CallLock = std::lock_guard<FairLock>;

// Transfers move this much per call: big blocks keep libsmbclient's request
// pipeline full (16 MiB reads ran ~25% faster than 1 MiB), and one block holds
// the lock for well under 100 ms on a LAN.
constexpr int64_t kBlock = 16 << 20;

SMBCCTX* newContext(SmbServer* server)
{
    CallLock lock(g_smbCalls);
    SMBCCTX* c = smbc_new_context();
    if (!c)
        return nullptr;
    smbc_setOptionUseKerberos(c, true);
    smbc_setOptionFallbackAfterKerberos(c, true);
    smbc_setTimeout(c, 15000);
    smbc_setFunctionAuthDataWithContext(c, authCallback);
    smbc_setOptionUserData(c, new CtxData{server, server->generation});
    if (!smbc_init_context(c)) {
        delete static_cast<CtxData*>(smbc_getOptionUserData(c));
        smbc_free_context(c, 1);
        return nullptr;
    }
    return c;
}

void freeContext(SMBCCTX* c)
{
    CallLock lock(g_smbCalls);
    delete static_cast<CtxData*>(smbc_getOptionUserData(c));
    smbc_free_context(c, 1);
}

class SmbLease {
public:
    SmbLease() = default;
    SmbLease(SmbServer* s, SMBCCTX* c) : m_server(s), m_ctx(c) { ++t_smbLeases; }
    SmbLease(SmbLease&& o) noexcept : m_server(o.m_server), m_ctx(o.m_ctx), m_dead(o.m_dead)
    {
        o.m_server = nullptr;
        o.m_ctx = nullptr;
    }
    SmbLease& operator=(SmbLease&& o) noexcept
    {
        if (this != &o) {
            this->~SmbLease();
            new (this) SmbLease(std::move(o));
        }
        return *this;
    }
    ~SmbLease()
    {
        if (!m_server)
            return;
        --t_smbLeases;
        std::lock_guard<std::mutex> lock(m_server->mutex);
        const auto* data = static_cast<CtxData*>(smbc_getOptionUserData(m_ctx));
        // A context made with an old login, or one that lost its connection,
        // is not reused.
        if (!m_dead && data->generation == m_server->generation && m_server->idle.size() < size_t(kMaxContexts)) {
            m_server->idle.push_back(m_ctx);
        } else {
            m_server->open -= 1;
            freeContext(m_ctx);
        }
        m_server->freed.notify_one();
    }
    explicit operator bool() const { return m_ctx != nullptr; }
    SMBCCTX* ctx() const { return m_ctx; }
    void markDead() { m_dead = true; }

private:
    SmbServer* m_server = nullptr;
    SMBCCTX* m_ctx = nullptr;
    bool m_dead = false;
};

bool connectionLost(int e)
{
    return e == ECONNRESET || e == ETIMEDOUT || e == EPIPE || e == ENOTCONN || e == ECONNABORTED || e == EHOSTUNREACH;
}

class SmbBackend : public Backend {
public:
    SmbBackend() = default;

    SmbServer* serverFor(const QString& location)
    {
        const QString key = locationAuthority(location).section(QLatin1Char('@'), -1);  // logins are per server
        std::lock_guard<std::mutex> lock(m_mutex);
        auto& s = m_servers[key];
        if (!s)
            s = std::make_unique<SmbServer>();
        return s.get();
    }

    SmbLease acquire(const QString& location, int& err)
    {
        SmbServer* server = serverFor(location);
        std::unique_lock<std::mutex> lock(server->mutex);
        for (;;) {
            if (!server->idle.empty()) {
                SMBCCTX* c = server->idle.back();
                server->idle.pop_back();
                return SmbLease(server, c);
            }
            if (server->open < kMaxContexts || t_smbLeases > 0)
                break;
            server->freed.wait(lock);
        }
        server->open += 1;
        lock.unlock();
        if (!server->creds && !server->verified && !server->keyringTried) {
            server->keyringTried = true;
            // First contact: what the keyring has for this server (GVfs's too),
            // else a user in the location means "log in as".
            const QUrl root(locationRoot(location));
            if (const auto login = findLogin(QStringLiteral("smb"), root.host(), root.userName())) {
                Credentials c;
                c.user = login->user;
                c.domain = login->domain;
                c.password = login->password;
                server->creds = c;
            } else if (!root.userName().isEmpty()) {
                server->creds = splitUser(root.userName(), QString());
            }
        }
        SMBCCTX* c = newContext(server);
        if (!c) {
            lock.lock();
            server->open -= 1;
            server->freed.notify_one();
            err = errno ? errno : ENOMEM;
            return {};
        }
        return SmbLease(server, c);
    }

    static Credentials splitUser(const QString& text, const QString& password)
    {
        Credentials c;
        c.password = password;
        QString user = text;
        for (QChar sep : {QLatin1Char('\\'), QLatin1Char(';')}) {
            if (const int i = int(user.indexOf(sep)); i > 0) {
                c.domain = user.left(i);
                user = user.mid(i + 1);
            }
        }
        c.user = user;
        return c;
    }

    void offerPassword(const QString& location, const QString& user, const QString& password) override
    {
        SmbServer* server = serverFor(location);
        std::vector<SMBCCTX*> old;
        {
            std::lock_guard<std::mutex> lock(server->mutex);
            const Credentials c = splitUser(user, password);
            if (server->creds && server->creds->user == c.user && server->creds->domain == c.domain &&
                server->creds->password == c.password)
                return;
            server->creds = c;
            server->verified = false;
            server->generation += 1;
            old.swap(server->idle);
            server->open -= int(old.size());
        }
        for (SMBCCTX* c : old)
            freeContext(c);
    }

    // Asks for a login (one dialog per server at a time). false: cancelled.
    bool askLogin(const QString& location, int seenGeneration)
    {
        SmbServer* server = serverFor(location);
        std::lock_guard<std::mutex> login(server->loginMutex);
        {
            std::lock_guard<std::mutex> lock(server->mutex);
            if (server->generation != seenGeneration)
                return true;  // someone else just logged in: try with that
        }
        const QString where = locationRoot(location);
        AuthQuestion q;
        q.kind = AuthQuestion::Kind::Login;
        q.offerRemember = true;
        q.location = where;
        q.message = QObject::tr("Log in to %1. Write the user as DOMAIN\\user if needed; leave both empty for guest.")
                        .arg(QUrl(where).host());
        {
            std::lock_guard<std::mutex> lock(server->mutex);
            q.user = server->creds ? (server->creds->domain.isEmpty() ? server->creds->user
                                                                      : server->creds->domain + QLatin1Char('\\') + server->creds->user)
                                   : QUrl(where).userName();
        }
        if (q.user.isEmpty())
            q.user = QString::fromLocal8Bit(qgetenv("USER"));
        const auto answer = askAuth(q);
        if (!answer)
            return false;
        const bool remember = lastAnswerRemember();
        const std::optional<QString> user = answer->section(QLatin1Char('\n'), 0, 0);
        const std::optional<QString> password = answer->section(QLatin1Char('\n'), 1);
        std::vector<SMBCCTX*> old;
        {
            std::lock_guard<std::mutex> lock(server->mutex);
            server->creds = splitUser(*user, *password);
            server->rememberWhenVerified = remember && !user->isEmpty();
            server->verified = false;
            server->generation += 1;
            old.swap(server->idle);
            server->open -= int(old.size());
        }
        for (SMBCCTX* c : old)
            freeContext(c);
        return true;
    }

    // Runs f(ctx, url) and handles logins and lost connections:
    // access denied before anything worked → ask for a login and retry.
    template <typename F>
    int withCtx(const QString& location, F&& f)
    {
        SmbServer* server = serverFor(location);
        const QByteArray url = smbUrl(location);
        for (int attempt = 0; attempt < 4; ++attempt) {
            int err = 0;
            int generation = 0;
            {
                std::lock_guard<std::mutex> lock(server->mutex);
                generation = server->generation;
            }
            SmbLease l = acquire(location, err);
            if (!l)
                return err;
            int r;
            {
                CallLock calls(g_smbCalls);
                errno = 0;
                r = f(l.ctx(), url);
            }
            if (r == 0) {
                loginWorked(location, server);
                return 0;
            }
            if ((r == EACCES || r == EPERM) && !server->verified) {
                l.markDead();
                if (!askLogin(location, generation))
                    return ECANCELED;
                continue;
            }
            if (connectionLost(r) && attempt == 0) {
                l.markDead();
                dropIdle(server);
                continue;
            }
            return r;
        }
        return EACCES;
    }

    // Something succeeded: the login is good; keep it if that was asked for.
    void loginWorked(const QString& location, SmbServer* server)
    {
        std::optional<Credentials> keep;
        {
            std::lock_guard<std::mutex> lock(server->mutex);
            server->verified = true;
            if (server->rememberWhenVerified && server->creds)
                keep = server->creds;
            server->rememberWhenVerified = false;
        }
        if (keep)
            storeLogin(QStringLiteral("smb"), QUrl(locationRoot(location)).host(), {keep->user, keep->domain, keep->password});
    }

    void dropIdle(SmbServer* server)
    {
        std::vector<SMBCCTX*> old;
        {
            std::lock_guard<std::mutex> lock(server->mutex);
            old.swap(server->idle);
            server->open -= int(old.size());
        }
        for (SMBCCTX* c : old)
            freeContext(c);
    }

    // libsmbclient wants percent-encoded names, and the login comes from the
    // callback rather than the URL.
    static QByteArray smbUrl(const QString& location)
    {
        const QUrl root(locationRoot(location));
        QByteArray out = "smb://" + QUrl::toPercentEncoding(root.host());
        if (root.port() > 0)
            out += ':' + QByteArray::number(root.port());
        for (const QString& part : locationPath(location).split(QLatin1Char('/'), Qt::SkipEmptyParts))
            out += '/' + QUrl::toPercentEncoding(part);
        if (out.count('/') == 2)
            out += '/';
        return out;
    }

    static void fill(FileEntry& e, const struct stat& st)
    {
        e.isDir = S_ISDIR(st.st_mode);
        e.isLink = S_ISLNK(st.st_mode);
        e.size = e.isDir ? -1 : int64_t(st.st_size);
        e.mtime = int64_t(st.st_mtime);
        e.mode = st.st_mode;
        e.uid = st.st_uid;
        e.gid = st.st_gid;
    }

    // ---------------------------------------------------------------- calls

    ListResult list(const QString& path, const ListOptions& opt, const std::atomic<bool>* cancel) override
    {
        ListResult r;
        r.path = path;
        const bool shares = locationPath(path) == QLatin1String("/");  // the server: its shares
        r.error = withCtx(path, [&](SMBCCTX* c, const QByteArray& url) {
            r.entries.clear();
            SMBCFILE* dir = smbc_getFunctionOpendir(c)(c, url.constData());
            if (!dir)
                return errno ? errno : EIO;
            if (shares) {
                while (struct smbc_dirent* d = smbc_getFunctionReaddir(c)(c, dir)) {
                    if (d->smbc_type != SMBC_FILE_SHARE)
                        continue;  // printers, IPC$
                    FileEntry e;
                    e.name = QString::fromUtf8(static_cast<const char*>(d->name));  // name is declared char[1]
                    if (!opt.showHidden && e.name.endsWith(QLatin1Char('$')))
                        continue;  // admin shares (C$, ADMIN$)
                    e.isDir = true;
                    e.size = -1;
                    e.mode = S_IFDIR | 0755;
                    r.entries.push_back(std::move(e));
                }
            } else {
                struct stat st;
                while (const struct libsmb_file_info* info = smbc_getFunctionReaddirPlus2(c)(c, dir, &st)) {
                    const QString name = QString::fromUtf8(info->name);
                    if (name == QLatin1String(".") || name == QLatin1String(".."))
                        continue;
                    const bool hidden = name.startsWith(QLatin1Char('.')) || (info->attrs & 0x2);  // DOS "hidden"
                    if (hidden && !opt.showHidden)
                        continue;
                    FileEntry e;
                    e.name = name;
                    fill(e, st);
                    r.entries.push_back(std::move(e));
                    if (cancel && cancel->load())
                        break;
                }
                struct statvfs vfs;
                if (smbc_getFunctionStatVFS(c)(c, const_cast<char*>(url.constData()), &vfs) == 0) {
                    r.freeBytes = int64_t(vfs.f_bavail) * int64_t(vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize);
                    r.totalBytes = int64_t(vfs.f_blocks) * int64_t(vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize);
                }
            }
            smbc_getFunctionClosedir(c)(c, dir);
            return 0;
        });
        if (cancel && cancel->load())
            r.error = ECANCELED;
        if (r.error == ECANCELED && !(cancel && cancel->load()))
            r.errorText = QObject::tr("Cancelled");
        else if (r.error == EACCES)
            r.errorText = QObject::tr("Access denied by %1").arg(locationAuthority(path));
        return r;
    }

    int stat(const QString& path, FileStat& out) override
    {
        if (locationPath(path) == QLatin1String("/")) {  // the server itself
            out = FileStat{};
            out.isDir = true;
            out.mode = S_IFDIR | 0755;
            return 0;
        }
        const QStringList parts = locationPath(path).split(QLatin1Char('/'), Qt::SkipEmptyParts);
        return withCtx(path, [&](SMBCCTX* c, const QByteArray& url) {
            if (parts.size() == 1) {  // stat on a share fails with EINVAL: opening it tells
                SMBCFILE* dir = smbc_getFunctionOpendir(c)(c, url.constData());
                if (!dir)
                    return errno ? errno : EIO;
                smbc_getFunctionClosedir(c)(c, dir);
                out = FileStat{};
                out.isDir = true;
                out.mode = S_IFDIR | 0755;
                return 0;
            }
            struct stat st;
            if (smbc_getFunctionStat(c)(c, url.constData(), &st) != 0)
                return errno ? errno : EIO;
            FileEntry e;
            fill(e, st);
            out.isDir = e.isDir;
            out.isLink = e.isLink;
            out.size = e.isDir ? 0 : e.size;
            out.mtime = e.mtime;
            out.mode = e.mode;
            out.uid = e.uid;
            out.gid = e.gid;
            return 0;
        });
    }

    int mkdir(const QString& path, uint32_t mode) override
    {
        return withCtx(path, [&](SMBCCTX* c, const QByteArray& url) {
            return smbc_getFunctionMkdir(c)(c, url.constData(), mode_t(mode & 07777 ? mode & 07777 : 0755)) == 0 ? 0
                                                                                                            : errno;
        });
    }

    int rename(const QString& from, const QString& to) override
    {
        const QByteArray target = smbUrl(to);
        return withCtx(from, [&](SMBCCTX* c, const QByteArray& url) {
            struct stat st;
            if (smbc_getFunctionStat(c)(c, target.constData(), &st) == 0)
                return EEXIST;  // never replace
            return smbc_getFunctionRename(c)(c, url.constData(), c, target.constData()) == 0 ? 0 : errno;
        });
    }

    int removeFile(const QString& path) override
    {
        return withCtx(path, [](SMBCCTX* c, const QByteArray& url) {
            return smbc_getFunctionUnlink(c)(c, url.constData()) == 0 ? 0 : errno;
        });
    }

    int removeDir(const QString& path) override
    {
        return withCtx(path, [](SMBCCTX* c, const QByteArray& url) {
            return smbc_getFunctionRmdir(c)(c, url.constData()) == 0 ? 0 : errno;
        });
    }

    std::unique_ptr<Reader> openRead(const QString& path, int& err) override;
    std::unique_ptr<Writer> openWrite(const QString& path, bool exclusive, uint32_t mode, int& err) override;

    // Keeps the lease the open file belongs to.
    SMBCFILE* openFile(const QString& path, int flags, mode_t mode, int& err, SmbLease& out)
    {
        SMBCFILE* file = nullptr;
        SmbServer* server = serverFor(path);
        const QByteArray url = smbUrl(path);
        for (int attempt = 0; attempt < 4; ++attempt) {
            int generation = 0;
            {
                std::lock_guard<std::mutex> lock(server->mutex);
                generation = server->generation;
            }
            SmbLease l = acquire(path, err);
            if (!l)
                return nullptr;
            {
                CallLock calls(g_smbCalls);
                errno = 0;
                file = smbc_getFunctionOpen(l.ctx())(l.ctx(), url.constData(), flags, mode);
                err = errno ? errno : EIO;
            }
            if (file) {
                loginWorked(path, server);
                err = 0;
                out = std::move(l);
                return file;
            }
            if ((err == EACCES || err == EPERM) && !server->verified) {
                l.markDead();
                if (!askLogin(path, generation)) {
                    err = ECANCELED;
                    return nullptr;
                }
                continue;
            }
            if (connectionLost(err) && attempt == 0) {
                l.markDead();
                dropIdle(server);
                continue;
            }
            return nullptr;
        }
        return nullptr;
    }

private:
    std::mutex m_mutex;
    std::map<QString, std::unique_ptr<SmbServer>> m_servers;
};

class SmbReader : public Reader {
public:
    SmbReader(SmbLease lease, SMBCFILE* f) : m_lease(std::move(lease)), m_file(f) {}
    ~SmbReader() override
    {
        CallLock calls(g_smbCalls);
        smbc_getFunctionClose(m_lease.ctx())(m_lease.ctx(), m_file);
    }
    int64_t read(char* buf, int64_t max) override
    {
        if (m_pos == m_buf.size()) {  // refill with one big block
            m_buf.resize(size_t(kBlock));
            ssize_t n;
            {
                CallLock calls(g_smbCalls);
                errno = 0;
                n = smbc_getFunctionRead(m_lease.ctx())(m_lease.ctx(), m_file, m_buf.data(), m_buf.size());
            }
            if (n < 0) {
                const int e = errno ? errno : EIO;
                m_buf.clear();
                m_pos = 0;
                if (connectionLost(e))
                    m_lease.markDead();
                return -e;
            }
            m_buf.resize(size_t(n));
            m_pos = 0;
            if (n == 0)
                return 0;
        }
        const int64_t n = qMin<int64_t>(max, int64_t(m_buf.size() - m_pos));
        memcpy(buf, m_buf.data() + m_pos, size_t(n));
        m_pos += size_t(n);
        return n;
    }

private:
    SmbLease m_lease;
    SMBCFILE* m_file;
    std::vector<char> m_buf;
    size_t m_pos = 0;
};

class SmbWriter : public Writer {
public:
    SmbWriter(SmbLease lease, SMBCFILE* f, QByteArray url) : m_lease(std::move(lease)), m_file(f), m_url(std::move(url)) {}
    ~SmbWriter() override
    {
        if (m_file) {
            CallLock calls(g_smbCalls);
            smbc_getFunctionClose(m_lease.ctx())(m_lease.ctx(), m_file);
        }
    }
    int64_t write(const char* buf, int64_t len) override
    {
        m_buf.insert(m_buf.end(), buf, buf + len);
        if (int64_t(m_buf.size()) >= kBlock)
            if (const int e = flush())
                return -e;
        return len;
    }
    int finish(int64_t mtime) override
    {
        const int flushed = flush();
        CallLock calls(g_smbCalls);
        SMBCCTX* c = m_lease.ctx();
        const int r = smbc_getFunctionClose(c)(c, m_file);
        const int err = flushed ? flushed : r == 0 ? 0 : (errno ? errno : EIO);
        m_file = nullptr;
        if (!err && mtime > 0) {
            struct timeval tv[2] = {{time_t(mtime), 0}, {time_t(mtime), 0}};
            smbc_getFunctionUtimes(c)(c, m_url.constData(), tv);  // best effort
        }
        return err;
    }

private:
    int flush()
    {
        CallLock calls(g_smbCalls);
        size_t done = 0;
        while (done < m_buf.size()) {
            errno = 0;
            const ssize_t n = smbc_getFunctionWrite(m_lease.ctx())(m_lease.ctx(), m_file, m_buf.data() + done, m_buf.size() - done);
            if (n <= 0) {
                const int e = n < 0 && errno ? errno : EIO;
                if (connectionLost(e))
                    m_lease.markDead();
                m_buf.clear();
                return e;
            }
            done += size_t(n);
        }
        m_buf.clear();
        return 0;
    }

    SmbLease m_lease;
    SMBCFILE* m_file;
    QByteArray m_url;
    std::vector<char> m_buf;
};

std::unique_ptr<Reader> SmbBackend::openRead(const QString& path, int& err)
{
    SmbLease lease;
    SMBCFILE* f = openFile(path, O_RDONLY, 0, err, lease);
    return f ? std::make_unique<SmbReader>(std::move(lease), f) : nullptr;
}

std::unique_ptr<Writer> SmbBackend::openWrite(const QString& path, bool exclusive, uint32_t mode, int& err)
{
    SmbLease lease;
    const int flags = O_WRONLY | O_CREAT | (exclusive ? O_EXCL : O_TRUNC);
    SMBCFILE* f = openFile(path, flags, mode_t(mode & 07777 ? mode & 07777 : 0644), err, lease);
    return f ? std::make_unique<SmbWriter>(std::move(lease), f, smbUrl(path)) : nullptr;
}

}  // namespace

void registerSmbBackend()
{
    static auto backend = std::make_shared<SmbBackend>();
    registerBackend(QStringLiteral("smb"), backend);
}

}  // namespace tandem

#else

namespace tandem {
void registerSmbBackend() {}
}  // namespace tandem

#endif
