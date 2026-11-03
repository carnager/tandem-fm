// sftp://user@host:port/path through libssh.
//
// Connections: a small pool per server (user@host:port). Each connection is
// used by one thread at a time; transfers keep up to kWindow requests in
// flight (libssh's sftp_aio API), which is what makes SFTP fast on links
// with latency. Logins honour ~/.ssh/config, the agent, default keys and
// known_hosts, like the ssh command does. Passwords typed once are kept in
// memory for the session, so further connections don't ask again.

#include "core/Backend.h"
#include "core/Keyring.h"
#include "core/Permissions.h"
#include "core/Util.h"

#ifdef TANDEM_HAVE_LIBSSH

#include <QHash>
#include <QElapsedTimer>
#include <QUrl>

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <libssh/callbacks.h>
#include <libssh/libssh.h>
#include <libssh/sftp.h>
#include <sys/stat.h>
#include <sys/time.h>

namespace tandem {

namespace {

constexpr int kMaxConnections = 6;  // per server
const int kWindow = qEnvironmentVariableIntValue("TANDEM_SFTP_WINDOW") > 0 ? qEnvironmentVariableIntValue("TANDEM_SFTP_WINDOW") : 32;  // requests in flight per lane

thread_local int t_leases = 0;  // connections this thread holds right now

struct Target {
    QString user, host;
    int port = 0;
};

Target parseAuthority(const QString& location)
{
    const QUrl url(locationRoot(location));
    return {url.userName(), url.host(), url.port(0)};
}

QByteArray remotePath(const QString& location)
{
    return locationPath(location).toUtf8();
}

int sftpErrno(int fx)
{
    switch (fx) {
    case SSH_FX_NO_SUCH_FILE:
    case SSH_FX_NO_SUCH_PATH: return ENOENT;
    case SSH_FX_PERMISSION_DENIED: return EACCES;
    case SSH_FX_FILE_ALREADY_EXISTS: return EEXIST;
    case SSH_FX_WRITE_PROTECT: return EROFS;
    case SSH_FX_NO_MEDIA: return ENODEV;
    case SSH_FX_OP_UNSUPPORTED: return ENOTSUP;
    case SSH_FX_INVALID_HANDLE: return EBADF;
    case SSH_FX_BAD_MESSAGE: return EBADMSG;
    case SSH_FX_NO_CONNECTION:
    case SSH_FX_CONNECTION_LOST: return ECONNRESET;
    default: return EIO;
    }
}

struct Conn {
    ssh_session ssh = nullptr;
    sftp_session sftp = nullptr;
    size_t maxRead = 32768;
    size_t maxWrite = 32768;
    bool dead = false;
    bool statvfs = false;
    struct ssh_callbacks_struct callbacks {};
    QString location;  // for passphrase questions

    ~Conn()
    {
        if (sftp)
            sftp_free(sftp);
        if (ssh) {
            if (!dead)
                ssh_disconnect(ssh);
            ssh_free(ssh);
        }
    }
    // errno for the last failure; marks the connection dead when it's gone.
    int error()
    {
        if (!ssh_is_connected(ssh)) {
            dead = true;
            return ECONNRESET;
        }
        const int fx = sftp ? sftp_get_error(sftp) : SSH_FX_OK;
        if (fx == SSH_FX_NO_CONNECTION || fx == SSH_FX_CONNECTION_LOST) {
            dead = true;
            return ECONNRESET;
        }
        return fx == SSH_FX_OK ? EIO : sftpErrno(fx);  // OK: failed below SFTP
    }
};

struct Server {
    std::mutex mutex;
    std::condition_variable freed;
    std::vector<std::unique_ptr<Conn>> idle;
    int open = 0;
    std::mutex loginMutex;  // one login dialog at a time per server
    std::optional<QString> password;
    bool keyringTried = false;  // asked the keyring once
    QString home;
};

class SftpBackend;

class Lease {
public:
    Lease() = default;
    // counted: part of this thread's "holds a connection" count (see acquire).
    Lease(Server* s, std::unique_ptr<Conn> c, bool counted = true) : m_server(s), m_conn(std::move(c)), m_counted(counted)
    {
        if (m_counted)
            ++t_leases;
    }
    Lease(Lease&& o) noexcept : m_server(o.m_server), m_conn(std::move(o.m_conn)), m_counted(o.m_counted)
    {
        o.m_server = nullptr;
    }
    Lease& operator=(Lease&&) = delete;
    ~Lease()
    {
        if (!m_server)
            return;
        if (m_counted)
            --t_leases;
        std::lock_guard<std::mutex> lock(m_server->mutex);
        if (m_conn && !m_conn->dead && m_server->idle.size() < size_t(kMaxConnections))
            m_server->idle.push_back(std::move(m_conn));
        else
            m_server->open -= 1;
        m_conn.reset();
        m_server->freed.notify_one();
    }
    explicit operator bool() const { return bool(m_conn); }
    Conn* operator->() const { return m_conn.get(); }
    Conn* get() const { return m_conn.get(); }

private:
    Server* m_server = nullptr;
    std::unique_ptr<Conn> m_conn;
    bool m_counted = true;
};

int passphraseCallback(const char* prompt, char* buf, size_t len, int, int, void* userdata)
{
    static std::mutex asking;  // key logins run in parallel: one question at a time
    std::lock_guard<std::mutex> lock(asking);
    auto* conn = static_cast<Conn*>(userdata);
    AuthQuestion q;
    q.kind = AuthQuestion::Kind::Passphrase;
    q.location = conn->location;
    q.message = QString::fromUtf8(prompt).trimmed();
    if (q.message.isEmpty())
        q.message = QObject::tr("Passphrase for your SSH key");
    const auto answer = askAuth(q);
    if (!answer)
        return -1;
    const QByteArray utf8 = answer->toUtf8();
    if (size_t(utf8.size()) + 1 > len)
        return -1;
    memcpy(buf, utf8.constData(), size_t(utf8.size()) + 1);
    return 0;
}

QString fingerprintOf(ssh_session s, QString* type)
{
    ssh_key key = nullptr;
    if (ssh_get_server_publickey(s, &key) != SSH_OK)
        return {};
    unsigned char* hash = nullptr;
    size_t hlen = 0;
    QString out;
    if (ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &hlen) == 0) {
        char* fp = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hlen);
        out = QString::fromLatin1(fp);
        ssh_string_free_char(fp);
        ssh_clean_pubkey_hash(&hash);
    }
    if (type)
        *type = QString::fromLatin1(ssh_key_type_to_char(ssh_key_type(key)));
    ssh_key_free(key);
    return out;
}

class SftpBackend : public Backend {
public:
    SftpBackend() { ssh_init(); }

    // ------------------------------------------------------------ connections

    Server* serverFor(const QString& location)
    {
        const QString key = locationAuthority(location);
        std::lock_guard<std::mutex> lock(m_mutex);
        auto& s = m_servers[key];
        if (!s)
            s = std::make_unique<Server>();
        return s.get();
    }

    void offerPassword(const QString& location, const QString&, const QString& password) override
    {
        Server* server = serverFor(location);
        std::lock_guard<std::mutex> lock(server->mutex);
        server->password = password;
    }

    // spare: an extra connection for a big transfer. Never waits and never
    // goes over the limit; empty if none is free. Not counted per thread, so
    // it may be opened on a helper thread and used on another.
    Lease acquire(const QString& location, int& err, QString* errText, bool spare = false)
    {
        Server* server = serverFor(location);
        {
            std::unique_lock<std::mutex> lock(server->mutex);
            for (;;) {
                if (!server->idle.empty()) {
                    auto c = std::move(server->idle.back());
                    server->idle.pop_back();
                    return Lease(server, std::move(c), !spare);
                }
                if (server->open < kMaxConnections)
                    break;
                if (spare)
                    return {};
                // A thread that already holds one may always open another
                // (copying on one server needs two), so nothing deadlocks.
                if (t_leases > 0)
                    break;
                server->freed.wait(lock);
            }
            server->open += 1;
        }
        auto conn = connect(location, server, err, errText);
        if (!conn) {
            std::lock_guard<std::mutex> lock(server->mutex);
            server->open -= 1;
            server->freed.notify_one();
            return {};
        }
        return Lease(server, std::move(conn), !spare);
    }

    std::unique_ptr<Conn> connect(const QString& location, Server* server, int& err, QString* errText)
    {
        auto fail = [&](int e, const QString& text) {
            err = e;
            if (errText)
                *errText = text;
            return nullptr;
        };
        const Target t = parseAuthority(location);
        if (t.host.isEmpty())
            return fail(EINVAL, QObject::tr("No host in %1").arg(location));
        auto c = std::make_unique<Conn>();
        c->location = locationAuthority(location);
        c->ssh = ssh_new();
        const QByteArray host = t.host.toUtf8();
        ssh_options_set(c->ssh, SSH_OPTIONS_HOST, host.constData());
        // ~/.ssh/config first (Host aliases, User, Port, IdentityFile,
        // ProxyJump), then what the location says explicitly.
        const QByteArray config = qgetenv("TANDEM_SSH_CONFIG");
        ssh_options_parse_config(c->ssh, config.isEmpty() ? nullptr : config.constData());
        if (!t.user.isEmpty()) {
            const QByteArray user = t.user.toUtf8();
            ssh_options_set(c->ssh, SSH_OPTIONS_USER, user.constData());
        }
        if (t.port > 0) {
            const unsigned int port = unsigned(t.port);
            ssh_options_set(c->ssh, SSH_OPTIONS_PORT, &port);
        }
        if (const QByteArray kh = qgetenv("TANDEM_SSH_KNOWN_HOSTS"); !kh.isEmpty())
            ssh_options_set(c->ssh, SSH_OPTIONS_KNOWNHOSTS, kh.constData());
        if (const QByteArray id = qgetenv("TANDEM_SSH_IDENTITY"); !id.isEmpty())
            ssh_options_set(c->ssh, SSH_OPTIONS_ADD_IDENTITY, id.constData());
        const long timeout = 15;
        ssh_options_set(c->ssh, SSH_OPTIONS_TIMEOUT, &timeout);
        ssh_callbacks_init(&c->callbacks);
        c->callbacks.auth_function = passphraseCallback;
        c->callbacks.userdata = c.get();
        ssh_set_callbacks(c->ssh, &c->callbacks);

        if (ssh_connect(c->ssh) != SSH_OK)
            return fail(EHOSTUNREACH, QString::fromUtf8(ssh_get_error(c->ssh)));

        // Known host and key login need no questions: those run in parallel
        // (extra lanes log in at the same time). Questions are asked by one
        // connection at a time; the others then find the host key known and
        // the password remembered.
        if (ssh_session_is_known_server(c->ssh) != SSH_KNOWN_HOSTS_OK) {
            std::lock_guard<std::mutex> login(server->loginMutex);
            if (const int e = verifyHost(c.get(), errText); e != 0) {
                err = e;
                return nullptr;
            }
        }
        int methods = 0;
        if (!loginWithoutQuestions(c.get(), &methods)) {
            std::lock_guard<std::mutex> login(server->loginMutex);
            if (const int e = authenticate(c.get(), server, methods, errText); e != 0) {
                err = e;
                return nullptr;
            }
        }
        c->sftp = sftp_new(c->ssh);
        if (!c->sftp || sftp_init(c->sftp) != SSH_OK)
            return fail(EPROTO, QObject::tr("The server has no SFTP: %1").arg(QString::fromUtf8(ssh_get_error(c->ssh))));
        if (sftp_limits_t lim = sftp_limits(c->sftp)) {
            if (qEnvironmentVariableIsSet("TANDEM_SFTP_DEBUG"))
                fprintf(stderr, "sftp limits: read %llu write %llu packet %llu handles %llu\n", (unsigned long long)lim->max_read_length,
                      (unsigned long long)lim->max_write_length, (unsigned long long)lim->max_packet_length,
                      (unsigned long long)lim->max_open_handles);
            c->maxRead = size_t(qBound<uint64_t>(4096, lim->max_read_length, 256 * 1024));
            c->maxWrite = size_t(qBound<uint64_t>(4096, lim->max_write_length, 256 * 1024));
            sftp_limits_free(lim);
        }
        c->statvfs = sftp_extension_supported(c->sftp, "statvfs@openssh.com", "2");
        return c;
    }

    int verifyHost(Conn* c, QString* errText)
    {
        QString type;
        switch (ssh_session_is_known_server(c->ssh)) {
        case SSH_KNOWN_HOSTS_OK:
            return 0;
        case SSH_KNOWN_HOSTS_CHANGED:
        case SSH_KNOWN_HOSTS_OTHER: {
            const QString fp = fingerprintOf(c->ssh, &type);
            if (errText)
                *errText = QObject::tr("The host key of %1 has CHANGED (now %2 %3). This can mean someone is "
                                       "intercepting the connection. If the server was reinstalled, remove its old "
                                       "key from ~/.ssh/known_hosts.")
                               .arg(c->location, type, fp);
            return EACCES;
        }
        case SSH_KNOWN_HOSTS_NOT_FOUND:
        case SSH_KNOWN_HOSTS_UNKNOWN: {
            AuthQuestion q;
            q.kind = AuthQuestion::Kind::HostKey;
            q.location = c->location;
            q.secret = false;
            const QString fp = fingerprintOf(c->ssh, &type);
            q.message = QObject::tr("First connection to %1. Is this its key?").arg(c->location);
            q.detail = QStringLiteral("%1  %2").arg(type, fp);
            if (!askAuth(q)) {
                if (errText)
                    *errText = QObject::tr("Cancelled");
                return ECANCELED;
            }
            ssh_session_update_known_hosts(c->ssh);
            return 0;
        }
        case SSH_KNOWN_HOSTS_ERROR:
        default:
            if (errText)
                *errText = QString::fromUtf8(ssh_get_error(c->ssh));
            return EIO;
        }
    }

    // "none", then the agent and keys (a key passphrase may still be asked).
    bool loginWithoutQuestions(Conn* c, int* methods)
    {
        if (ssh_userauth_none(c->ssh, nullptr) == SSH_AUTH_SUCCESS)
            return true;
        *methods = ssh_userauth_list(c->ssh, nullptr);
        return (*methods & SSH_AUTH_METHOD_PUBLICKEY) &&
               ssh_userauth_publickey_auto(c->ssh, nullptr, nullptr) == SSH_AUTH_SUCCESS;
    }

    // Password and keyboard-interactive logins, which ask questions.
    // Where a login is kept in the keyring: like GVfs (sftp, host, user, port).
    struct KeyringKey {
        QString host, user;
        int port;
    };
    static KeyringKey keyringKey(const QString& location)
    {
        const QUrl u(location);
        const QString user = u.userName().isEmpty() ? QString::fromLocal8Bit(qgetenv("USER")) : u.userName();
        return {u.host(), user, u.port(0) == 22 ? 0 : u.port(0)};
    }
    // A password the keyring has for this server, unless one is known already.
    static void passwordFromKeyring(Conn* c, Server* server)
    {
        if (server->password || server->keyringTried)
            return;
        server->keyringTried = true;
        const KeyringKey k = keyringKey(c->location);
        if (const auto login = findLogin(QStringLiteral("sftp"), k.host, k.user, k.port))
            server->password = login->password;
    }
    static void rememberPassword(Conn* c, const QString& password)
    {
        const KeyringKey k = keyringKey(c->location);
        storeLogin(QStringLiteral("sftp"), k.host, {k.user, QString(), password}, k.port);
    }

    int authenticate(Conn* c, Server* server, int methods, QString* errText)
    {
        int rc = SSH_AUTH_DENIED;
        if (methods & (SSH_AUTH_METHOD_INTERACTIVE | SSH_AUTH_METHOD_PASSWORD))
            passwordFromKeyring(c, server);
        if (methods & SSH_AUTH_METHOD_INTERACTIVE) {
            rc = keyboardInteractive(c, server);
            if (rc == SSH_AUTH_SUCCESS)
                return 0;
            if (rc == -2) {
                if (errText)
                    *errText = QObject::tr("Cancelled");
                return ECANCELED;
            }
        }
        if (methods & SSH_AUTH_METHOD_PASSWORD) {
            for (int attempt = 0; attempt < 3; ++attempt) {
                std::optional<QString> pw = attempt == 0 ? server->password : std::nullopt;
                bool remember = false;
                if (!pw) {
                    AuthQuestion q;
                    q.kind = AuthQuestion::Kind::Password;
                    q.offerRemember = true;
                    q.location = c->location;
                    q.message = attempt == 0 ? QObject::tr("Password for %1").arg(c->location)
                                             : QObject::tr("Wrong password. Password for %1").arg(c->location);
                    pw = askAuth(q);
                    remember = lastAnswerRemember();
                    if (!pw) {
                        if (errText)
                            *errText = QObject::tr("Cancelled");
                        return ECANCELED;
                    }
                }
                rc = ssh_userauth_password(c->ssh, nullptr, pw->toUtf8().constData());
                if (rc == SSH_AUTH_SUCCESS) {
                    server->password = pw;
                    if (remember)  // only once it worked
                        rememberPassword(c, *pw);
                    return 0;
                }
                server->password.reset();
                if (rc == SSH_AUTH_ERROR)
                    break;
            }
        }
        if (errText)
            *errText = QObject::tr("Login to %1 failed (tried keys%2)")
                           .arg(c->location, methods & SSH_AUTH_METHOD_PASSWORD ? QObject::tr(" and password") : QString());
        return EACCES;
    }

    // SSH_AUTH_* or -2 when the user cancelled.
    int keyboardInteractive(Conn* c, Server* server)
    {
        bool usedRemembered = false;
        std::optional<QString> toRemember;
        int rc = ssh_userauth_kbdint(c->ssh, nullptr, nullptr);
        while (rc == SSH_AUTH_INFO) {
            const int n = ssh_userauth_kbdint_getnprompts(c->ssh);
            for (int i = 0; i < n; ++i) {
                char echo = 0;
                const QString prompt = QString::fromUtf8(ssh_userauth_kbdint_getprompt(c->ssh, unsigned(i), &echo)).trimmed();
                std::optional<QString> answer;
                const bool passwordLike = !echo && prompt.contains(QLatin1String("assword"), Qt::CaseInsensitive);
                if (passwordLike && server->password && !usedRemembered) {
                    answer = server->password;
                    usedRemembered = true;
                } else {
                    AuthQuestion q;
                    q.kind = passwordLike ? AuthQuestion::Kind::Password : AuthQuestion::Kind::Prompt;
                    q.offerRemember = passwordLike;
                    q.location = c->location;
                    q.message = prompt.isEmpty() ? QObject::tr("Answer for %1").arg(c->location) : prompt;
                    q.secret = !echo;
                    answer = askAuth(q);
                    if (!answer)
                        return -2;
                    if (passwordLike) {
                        server->password = answer;
                        if (lastAnswerRemember())
                            toRemember = answer;
                    }
                }
                ssh_userauth_kbdint_setanswer(c->ssh, unsigned(i), answer->toUtf8().constData());
            }
            rc = ssh_userauth_kbdint(c->ssh, nullptr, nullptr);
        }
        if (rc != SSH_AUTH_SUCCESS && usedRemembered)
            server->password.reset();
        if (rc == SSH_AUTH_SUCCESS && toRemember)
            rememberPassword(c, *toRemember);
        return rc;
    }

    // One dead connection means the others to that server are most likely
    // dead too (server restart, network change, idle timeout): drop them all
    // so the retry opens a fresh one.
    void dropIdle(const QString& location)
    {
        Server* server = serverFor(location);
        std::vector<std::unique_ptr<Conn>> old;
        {
            std::lock_guard<std::mutex> lock(server->mutex);
            old.swap(server->idle);
            server->open -= int(old.size());
        }
        for (auto& c : old)
            c->dead = true;  // don't wait for a polite goodbye from a dead server
    }

    // Runs f with a connection; once more with a fresh one if the old one
    // turned out to be dead.
    template <typename F>
    int withConn(const QString& location, F&& f, QString* errText = nullptr)
    {
        for (int attempt = 0; attempt < 2; ++attempt) {
            int err = 0;
            Lease l = acquire(location, err, errText);
            if (!l)
                return err;
            const int r = f(l.get());
            if (r == 0 || !l->dead)
                return r;
            dropIdle(location);
        }
        return ECONNRESET;
    }

    // ---------------------------------------------------------------- calls

    ListResult list(const QString& path, const ListOptions& opt, const std::atomic<bool>* cancel) override
    {
        ListResult r;
        r.path = path;
        QString errText;
        r.error = withConn(path, [&](Conn* c) {
            r.entries.clear();
            const QByteArray dirPath = remotePath(path);
            sftp_dir dir = sftp_opendir(c->sftp, dirPath.constData());
            if (!dir)
                return c->error();
            std::vector<QByteArray> links;
            while (sftp_attributes a = sftp_readdir(c->sftp, dir)) {
                const QString name = QString::fromUtf8(a->name);
                if (name == QLatin1String(".") || name == QLatin1String("..") ||
                    (!opt.showHidden && name.startsWith(QLatin1Char('.')))) {
                    sftp_attributes_free(a);
                    continue;
                }
                FileEntry e;
                e.name = name;
                fill(e, a);
                if (e.isLink)
                    links.push_back(name.toUtf8());
                r.entries.push_back(std::move(e));
                sftp_attributes_free(a);
                if (cancel && cancel->load())
                    break;
            }
            const bool complete = sftp_dir_eof(dir);
            sftp_closedir(dir);
            if (!complete && !(cancel && cancel->load()))
                return c->error();
            // Links: one more round trip each, to know if they lead to a folder.
            for (FileEntry& e : r.entries) {
                if (!e.isLink)
                    continue;
                const QByteArray full = joinPath(dirPath, e.name.toUtf8());
                if (sftp_attributes t = sftp_stat(c->sftp, full.constData())) {
                    e.isDir = t->type == SSH_FILEXFER_TYPE_DIRECTORY;
                    if (!e.isDir)
                        e.size = int64_t(t->size);
                    sftp_attributes_free(t);
                } else {
                    e.brokenLink = true;
                }
            }
            for (FileEntry& e : r.entries)
                if (e.isDir)
                    e.size = -1;
            if (c->statvfs) {
                if (sftp_statvfs_t vfs = sftp_statvfs(c->sftp, dirPath.constData())) {
                    r.freeBytes = int64_t(vfs->f_bavail * vfs->f_frsize);
                    r.totalBytes = int64_t(vfs->f_blocks * vfs->f_frsize);
                    sftp_statvfs_free(vfs);
                }
            }
            return 0;
        }, &errText);
        if (cancel && cancel->load())
            r.error = ECANCELED;
        if (r.error && !errText.isEmpty())
            r.errorText = errText;
        return r;
    }

    // One "find" on the server instead of a listing per folder: what rsync
    // does too. Needs a shell login with GNU find; sftp-only accounts and
    // BusyBox say no (or stay silent), and the caller walks instead.
    // A command on the server over the same SSH connection (when it lets us:
    // sftp-only accounts don't, and a forced sftp-server never answers).
    struct RunResult {
        bool ran = false;
        int status = -1;
        QByteArray out, err;
    };
    RunResult runCommand(const QString& location, const QByteArray& cmd, const std::atomic<bool>* cancel = nullptr)
    {
        RunResult r;
        if (qEnvironmentVariableIsSet("TANDEM_SFTP_NO_EXEC"))
            return r;
        withConn(location, [&](Conn* c) {
            ssh_channel ch = ssh_channel_new(c->ssh);
            if (!ch)
                return EIO;
            if (ssh_channel_open_session(ch) != SSH_OK) {
                ssh_channel_free(ch);
                return ENOTSUP;
            }
            if (ssh_channel_request_exec(ch, cmd.constData()) != SSH_OK) {
                ssh_channel_close(ch);
                ssh_channel_free(ch);
                return ENOTSUP;
            }
            char buf[65536];
            QElapsedTimer silent;
            silent.start();
            while (!(cancel && cancel->load())) {
                const int n = ssh_channel_read_timeout(ch, buf, sizeof buf, 0, 500);
                bool got = n > 0;
                if (n > 0)
                    r.out.append(buf, n);
                for (int e; (e = ssh_channel_read_timeout(ch, buf, sizeof buf, 1, 0)) > 0;) {
                    r.err.append(buf, e);
                    got = true;
                }
                if (got)
                    silent.restart();
                else if (n == SSH_ERROR || ssh_channel_is_eof(ch))
                    break;
                else if (r.out.isEmpty() && r.err.isEmpty() && silent.elapsed() > 8000)
                    break;
            }
            const bool eof = ssh_channel_is_eof(ch);
            ssh_channel_send_eof(ch);
            ssh_channel_close(ch);
            uint32_t code = 0;
            char* signal = nullptr;
            int core = 0;
            r.status = eof && ssh_channel_get_exit_state(ch, &code, &signal, &core) == SSH_OK && !signal ? int(code) : -1;
            ssh_string_free_char(signal);
            ssh_channel_free(ch);
            r.ran = eof;
            return 0;
        });
        return r;
    }
    static QByteArray shellQuote(QByteArray s)
    {
        s.replace('\'', "'\\''");
        return '\'' + s + '\'';
    }

    // ---------------------------------------------------------- permissions
    // stat and getfacl/setfacl run on the server: its users, its ACLs.

    static uint32_t rwxBits(const QByteArray& s)
    {
        return (s.contains('r') ? PermRead : 0) | (s.contains('w') ? PermWrite : 0) | (s.contains('x') ? PermExec : 0);
    }
    static QByteArray rwxText(uint32_t p, bool smartX = false)
    {
        QByteArray t;
        t += (p & PermRead) ? 'r' : '-';
        t += (p & PermWrite) ? 'w' : '-';
        t += (p & PermExec) ? (smartX ? 'X' : 'x') : '-';
        return t;
    }

    bool readPermissions(const QString& path, PermInfo& info) override
    {
        const QByteArray q = shellQuote(remotePath(path));
        const QByteArray script = "LC_ALL=C; export LC_ALL; stat -L -c '%a|%u|%g|%U|%G|%F' -- " + q +
                                  " || exit 3; command -v getfacl >/dev/null 2>&1 || exit 0; "
                                  "echo ::A; getfacl -cpn -- " + q + "; echo ::a; getfacl -cp -- " + q +
                                  "; echo ::D; getfacl -cpdn -- " + q + "; echo ::d; getfacl -cpd -- " + q;
        const RunResult r = runCommand(path, "sh -c " + shellQuote(script));
        if (!r.ran || r.status != 0)
            return false;
        const QList<QByteArray> lines = r.out.split('\n');
        const QList<QByteArray> st = lines.value(0).split('|');
        if (st.size() < 6)
            return false;
        info = PermInfo{};
        info.mode = st[0].toUInt(nullptr, 8) & 07777;
        info.uid = st[1].toUInt();
        info.gid = st[2].toUInt();
        info.owner = QString::fromUtf8(st[3]);
        info.group = QString::fromUtf8(st[4]);
        info.isDir = st[5].contains("directory");
        info.fsName = QStringLiteral("sftp");
        // Sections: ::A numeric, ::a names (same lines), ::D/::d the defaults.
        QHash<QByteArray, QList<QByteArray>> sec;
        QByteArray cur;
        for (int i = 1; i < lines.size(); ++i) {
            const QByteArray l = lines[i].trimmed();
            if (l.startsWith("::"))
                cur = l.mid(2);
            else if (!l.isEmpty() && !cur.isEmpty())
                sec[cur] << l;
        }
        info.aclSupported = sec.contains("A") || sec.contains("a");
        auto parse = [](const QList<QByteArray>& num, const QList<QByteArray>& names, uint32_t* base, int* mask) {
            QList<AclEntry> named;
            *mask = -1;
            for (int i = 0; i < num.size(); ++i) {
                const QList<QByteArray> f = num[i].split(':');
                if (f.size() < 3)
                    continue;
                const uint32_t p = rwxBits(f[2]);
                if (f[0] == "mask") {
                    *mask = int(p);
                } else if (f[1].isEmpty()) {
                    if (f[0] == "user")
                        *base |= p << 6;
                    else if (f[0] == "group")
                        *base |= p << 3;
                    else if (f[0] == "other")
                        *base |= p;
                } else if (f[0] == "user" || f[0] == "group") {
                    AclEntry e;
                    e.isGroup = f[0] == "group";
                    e.id = f[1].toUInt();
                    const QList<QByteArray> n = names.value(i).split(':');
                    e.name = QString::fromUtf8(n.value(1, f[1]));
                    e.perms = p;
                    named << e;
                }
            }
            if (*mask >= 0) {  // as it applies
                *base = (*base & ~0070u) | (*base & (uint32_t(*mask) << 3));
                for (AclEntry& e : named)
                    e.perms &= uint32_t(*mask);
            }
            return named;
        };
        uint32_t base = 0;
        int mask = -1;
        info.acl = parse(sec.value("A"), sec.value("a"), &base, &mask);
        if (!info.acl.isEmpty())
            info.mode = (info.mode & ~0070u) | (base & 0070);
        if (info.isDir && !sec.value("D").isEmpty()) {
            uint32_t dbase = 0;
            int dmask = -1;
            info.defaultAcl = parse(sec.value("D"), sec.value("d"), &dbase, &dmask);
            info.hasDefault = true;
            info.defaultBase = dbase;
        }
        return true;
    }

    int applyPermissions(const QString& path, const PermChange& c, QString* what) override
    {
        const QByteArray q = shellQuote(remotePath(path));
        const QByteArray R = c.recursive ? "-R " : "";
        const bool smart = c.recursive && c.smartExec;
        QList<QByteArray> steps;
        if (c.uid)
            steps << "chown " + R + QByteArray::number(*c.uid) + " -- " + q;
        if (c.gid)
            steps << "chgrp " + R + QByteArray::number(*c.gid) + " -- " + q;
        // Bits as chmod's symbolic form, so each item keeps what isn't mentioned.
        QList<QByteArray> sym;
        const char who[] = {'u', 'g', 'o'};
        for (int k = 0; k < 3; ++k) {
            const int shift = 6 - 3 * k;
            for (const auto& [bits, sign] : {std::pair{c.setBits, '+'}, std::pair{c.clearBits, '-'}}) {
                QByteArray part;
                if (bits & (4u << shift))
                    part += 'r';
                if (bits & (2u << shift))
                    part += 'w';
                if (bits & (1u << shift))
                    part += (smart && sign == '+') ? 'X' : 'x';
                if (!part.isEmpty())
                    sym << QByteArray(1, who[k]) + sign + part;
            }
        }
        for (const auto& [bit, spec] : {std::pair{04000u, "u%cs"}, std::pair{02000u, "g%cs"}, std::pair{01000u, "%ct"}}) {
            if (c.setBits & bit)
                sym << QByteArray(spec).replace("%c", "+");
            if (c.clearBits & bit)
                sym << QByteArray(spec).replace("%c", "-");
        }
        if (!sym.isEmpty())
            steps << "chmod " + R + sym.join(',') + " -- " + q;
        auto spec = [&](const QList<AclEntry>& list, bool withPerms) {
            QList<QByteArray> parts;
            for (const AclEntry& e : list)
                parts << QByteArray(e.isGroup ? "g:" : "u:") + QByteArray::number(e.id) +
                             (withPerms ? ":" + rwxText(e.perms, smart) : QByteArray());
            return parts.join(',');
        };
        if (c.clearAcl)
            steps << "setfacl " + R + "-b -- " + q;
        if (!c.aclRemove.isEmpty())
            steps << "setfacl " + R + "-x " + shellQuote(spec(c.aclRemove, false)) + " -- " + q;
        if (!c.aclSet.isEmpty())
            steps << "setfacl " + R + "-m " + shellQuote(spec(c.aclSet, true)) + " -- " + q;
        // Defaults belong to folders: in a tree, to every folder in it.
        auto onFolders = [&](const QByteArray& args) {
            return c.recursive ? "find " + q + " -type d -exec setfacl " + args + " {} +" : "setfacl " + args + " -- " + q;
        };
        if (c.defaultEnabled && !*c.defaultEnabled)
            steps << onFolders("-k");
        if (!c.defaultRemove.isEmpty())
            steps << onFolders("-d -x " + shellQuote(spec(c.defaultRemove, false)));
        if (!c.defaultSet.isEmpty())
            steps << onFolders("-d -m " + shellQuote(spec(c.defaultSet, true)));
        if (steps.isEmpty())
            return 0;
        const RunResult r = runCommand(path, "sh -c " + shellQuote("LC_ALL=C; export LC_ALL; " + steps.join(" && ")));
        if (!r.ran)
            return ENOTSUP;
        if (r.status == 0)
            return 0;
        const QString err = QString::fromUtf8(r.err).trimmed().section(QLatin1Char('\n'), 0, 0);
        if (what)
            *what = err;
        if (err.contains(QLatin1String("not permitted")))
            return EPERM;
        if (err.contains(QLatin1String("not supported")))
            return EOPNOTSUPP;
        return EIO;
    }

    bool listAccounts(const QString& location, QList<QPair<QString, uint32_t>>& users,
                      QList<QPair<QString, uint32_t>>& groups) override
    {
        const RunResult r = runCommand(location, "sh -c " + shellQuote("{ getent passwd || cat /etc/passwd; } | cut -d: -f1,3; "
                                                                      "echo ::G; { getent group || cat /etc/group; } | cut -d: -f1,3"));
        if (!r.ran || r.status != 0)
            return false;
        bool inGroups = false;
        for (const QByteArray& line : r.out.split('\n')) {
            if (line == "::G") {
                inGroups = true;
                continue;
            }
            const int colon = int(line.indexOf(':'));
            if (colon <= 0)
                continue;
            (inGroups ? groups : users).append({QString::fromUtf8(line.left(colon)), line.mid(colon + 1).toUInt()});
        }
        auto person = [](uint32_t id) { return id >= 1000 && id < 60000; };
        for (auto* list : {&users, &groups})
            std::sort(list->begin(), list->end(), [&](const auto& a, const auto& b) {
                if (person(a.second) != person(b.second))
                    return person(a.second);
                return a.first.compare(b.first, Qt::CaseInsensitive) < 0;
            });
        return !users.isEmpty();
    }

    bool listTree(const QString& path, bool hidden, Tree& out, const std::atomic<bool>* cancel) override
    {
        const QByteArray cmd = "LC_ALL=C find " + shellQuote(remotePath(path)) + " -mindepth 1 " +
                               (hidden ? QByteArray() : QByteArray("\\( -name '.*' -prune \\) -o ")) +
                               "-printf '%y %Y %s %T@ %m %P\\0'";
        const RunResult run = runCommand(path, cmd, cancel);
        // Unreadable subfolders make find exit 1 but the rest is fine.
        const bool ok = run.ran && (run.status == 0 || (run.status == 1 && !run.out.isEmpty()));
        const QByteArray& data = run.out;
        if (!ok || (cancel && cancel->load()))
            return false;
        out.clear();
        out.insert(QString(), {});
        for (const QByteArray& rec : data.split('\0')) {
            // "y Y size mtime mode rel", rel last (it may hold spaces)
            int at = 0;
            QByteArray f[5];
            bool good = true;
            for (int k = 0; k < 5 && good; ++k) {
                const int sp = int(rec.indexOf(' ', at));
                good = sp > at;
                if (good) {
                    f[k] = rec.mid(at, sp - at);
                    at = sp + 1;
                }
            }
            const QString rel = QString::fromUtf8(rec.mid(at));
            if (!good || rel.isEmpty())
                continue;
            FileEntry e;
            const int slash = int(rel.lastIndexOf(QLatin1Char('/')));
            e.name = rel.mid(slash + 1);
            const char type = f[0].at(0);
            e.isLink = type == 'l';
            e.isDir = type == 'd' || (e.isLink && f[1] == "d");
            e.brokenLink = e.isLink && (f[1] == "N" || f[1] == "L");
            e.size = e.isDir ? -1 : f[2].toLongLong();
            e.mtime = int64_t(f[3].toDouble());
            e.mode = f[4].toUInt(nullptr, 8) | (type == 'd' ? S_IFDIR : e.isLink ? S_IFLNK : S_IFREG);
            if (type == 'd')
                out[rel];  // empty folders too
            out[slash < 0 ? QString() : rel.left(slash)].push_back(std::move(e));
        }
        return true;
    }

    static void fill(FileEntry& e, sftp_attributes a)
    {
        e.isLink = a->type == SSH_FILEXFER_TYPE_SYMLINK;
        e.isDir = a->type == SSH_FILEXFER_TYPE_DIRECTORY;
        e.size = int64_t(a->size);
        e.mtime = int64_t(a->mtime64 ? a->mtime64 : a->mtime);
        e.mode = a->permissions;
        if ((e.mode & S_IFMT) == 0)
            e.mode |= e.isDir ? S_IFDIR : e.isLink ? S_IFLNK : S_IFREG;
        e.uid = a->uid;
        e.gid = a->gid;
    }

    int stat(const QString& path, FileStat& out) override
    {
        return withConn(path, [&](Conn* c) {
            const QByteArray p = remotePath(path);
            sftp_attributes a = sftp_lstat(c->sftp, p.constData());
            if (!a)
                return c->error();
            FileEntry e;
            fill(e, a);
            sftp_attributes_free(a);
            out.isDir = e.isDir;
            out.isLink = e.isLink;
            out.size = e.size;
            out.mtime = e.mtime;
            out.mode = e.mode;
            out.uid = e.uid;
            out.gid = e.gid;
            return 0;
        });
    }

    QString homeLocation(const QString& path) override
    {
        Server* server = serverFor(path);
        {
            std::lock_guard<std::mutex> lock(server->mutex);
            if (!server->home.isEmpty())
                return locationAuthority(path) + server->home;
        }
        QString home;
        withConn(path, [&](Conn* c) {
            char* p = sftp_canonicalize_path(c->sftp, ".");
            if (!p)
                return c->error();
            home = QString::fromUtf8(p);
            ssh_string_free_char(p);
            return 0;
        });
        if (home.isEmpty() || !home.startsWith(QLatin1Char('/')))
            return locationRoot(path);
        std::lock_guard<std::mutex> lock(server->mutex);
        server->home = home;
        return locationAuthority(path) + home;
    }

    int simple(const QString& path, const std::function<int(Conn*, const QByteArray&)>& f)
    {
        return withConn(path, [&](Conn* c) { return f(c, remotePath(path)); });
    }

    int mkdir(const QString& path, uint32_t mode) override
    {
        return simple(path, [&](Conn* c, const QByteArray& p) {
            if (sftp_mkdir(c->sftp, p.constData(), mode_t(mode & 07777 ? mode & 07777 : 0755)) == 0)
                return 0;
            const int e = c->error();
            // Many servers say "failure" for "exists": check.
            if (e == EIO) {
                if (sftp_attributes a = sftp_lstat(c->sftp, p.constData())) {
                    sftp_attributes_free(a);
                    return EEXIST;
                }
            }
            return e;
        });
    }

    int rename(const QString& from, const QString& to) override
    {
        return withConn(from, [&](Conn* c) {
            const QByteArray a = remotePath(from), b = remotePath(to);
            // Some servers overwrite on rename: never replace silently.
            if (sftp_attributes t = sftp_lstat(c->sftp, b.constData())) {
                sftp_attributes_free(t);
                return EEXIST;
            }
            return sftp_rename(c->sftp, a.constData(), b.constData()) == 0 ? 0 : c->error();
        });
    }

    int replace(const QString& from, const QString& to) override
    {
        // Servers that rename over files do it in one step; plain SFTP v3
        // refuses, then the old file is removed first.
        const int e = withConn(from, [&](Conn* c) {
            return sftp_rename(c->sftp, remotePath(from).constData(), remotePath(to).constData()) == 0 ? 0 : c->error();
        });
        return e == 0 ? 0 : Backend::replace(from, to);
    }

    int removeFile(const QString& path) override
    {
        return simple(path, [](Conn* c, const QByteArray& p) { return sftp_unlink(c->sftp, p.constData()) == 0 ? 0 : c->error(); });
    }

    int removeDir(const QString& path) override
    {
        return simple(path, [](Conn* c, const QByteArray& p) { return sftp_rmdir(c->sftp, p.constData()) == 0 ? 0 : c->error(); });
    }

    int readLink(const QString& path, QString& target) override
    {
        return simple(path, [&](Conn* c, const QByteArray& p) {
            char* t = sftp_readlink(c->sftp, p.constData());
            if (!t)
                return c->error();
            target = QString::fromUtf8(t);
            ssh_string_free_char(t);
            return 0;
        });
    }

    int symlink(const QString& target, const QString& path) override
    {
        return simple(path, [&](Conn* c, const QByteArray& p) {
            return sftp_symlink(c->sftp, target.toUtf8().constData(), p.constData()) == 0 ? 0 : c->error();
        });
    }

    int chmod(const QString& path, uint32_t mode) override
    {
        return simple(path, [&](Conn* c, const QByteArray& p) {
            return sftp_chmod(c->sftp, p.constData(), mode_t(mode & 07777)) == 0 ? 0 : c->error();
        });
    }

    std::unique_ptr<Reader> openRead(const QString& path, int& err) override;
    std::unique_ptr<Writer> openWrite(const QString& path, bool exclusive, uint32_t mode, int& err) override;

private:
    std::mutex m_mutex;
    std::map<QString, std::unique_ptr<Server>> m_servers;  // never shrinks; Servers stay put
};

// ------------------------------------------------------------ transfers
//
// One SSH connection can only move about one channel window per round trip
// (libssh's is fixed), so big files are spread over up to kLanes
// connections: chunk i goes over lane i % lanes, and the pieces are put
// back in order. Small files, or a server without spare connections, use
// one lane.

const int kLanes = qEnvironmentVariableIntValue("TANDEM_SFTP_LANES") > 0 ? qEnvironmentVariableIntValue("TANDEM_SFTP_LANES") : 4;
constexpr int64_t kLaneThreshold = 16 * 1024 * 1024;

struct Lane {
    Lease lease;
    sftp_file file = nullptr;
};

// Opens extra lanes in parallel (each may need a login round trip); as many
// as the server has room for, possibly none.
std::vector<Lane> openSpareLanes(SftpBackend* backend, const QString& location, const QByteArray& path, int flags,
                                 int count);

// One lane: requests pipelined on a single connection, on the caller's thread.
class SftpReader : public Reader {
public:
    SftpReader(Lane lane, int64_t size) : m_lane(std::move(lane)), m_size(size) { m_chunk = m_lane.lease->maxRead; }
    ~SftpReader() override
    {
        drain();
        sftp_close(m_lane.file);
    }

    int64_t read(char* buf, int64_t max) override
    {
        int64_t filled = 0;
        topUp();
        while (!m_pending.empty() && filled + int64_t(m_chunk) <= max) {
            Request r = m_pending.front();
            m_pending.pop_front();
            const ssize_t n = sftp_aio_wait_read(&r.aio, buf + filled, m_chunk);
            if (n < 0) {
                const int e = m_lane.lease->error();
                drain();
                return -e;
            }
            filled += n;
            m_done = r.offset + n;
            if (size_t(n) < m_chunk) {
                // Short read: the end, or a server sending less than asked.
                drain();
                if (n == 0 || m_done >= m_size) {
                    m_eof = true;
                    break;
                }
                m_requested = m_done;
            }
            topUp();
        }
        return filled;
    }

private:
    struct Request {
        sftp_aio aio;
        int64_t offset;
    };
    void topUp()
    {
        while (!m_eof && m_pending.size() < size_t(kWindow) && m_requested <= m_size) {
            sftp_seek64(m_lane.file, uint64_t(m_requested));
            sftp_aio aio = nullptr;
            if (sftp_aio_begin_read(m_lane.file, m_chunk, &aio) < 0)
                break;
            m_pending.push_back({aio, m_requested});
            m_requested += int64_t(m_chunk);
        }
    }
    void drain()
    {
        static thread_local std::vector<char> sink;
        sink.resize(m_chunk);
        while (!m_pending.empty()) {
            Request r = m_pending.front();
            m_pending.pop_front();
            sftp_aio_wait_read(&r.aio, sink.data(), m_chunk);
        }
    }

    Lane m_lane;
    int64_t m_size;
    size_t m_chunk;
    std::deque<Request> m_pending;
    int64_t m_requested = 0;
    int64_t m_done = 0;
    bool m_eof = false;
};

// Several lanes, each on its own thread with its own connection, so every
// connection keeps moving; the reader hands the pieces out in order.
class LanedReader : public Reader {
public:
    LanedReader(std::vector<Lane> lanes, int64_t size) : m_lanes(std::move(lanes)), m_size(size)
    {
        m_chunk = m_lanes.front().lease->maxRead;
        for (const Lane& l : m_lanes)
            m_chunk = qMin(m_chunk, l.lease->maxRead);
        for (size_t k = 0; k < m_lanes.size(); ++k)
            m_threads.emplace_back([this, k] { run(k); });
    }
    ~LanedReader() override
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        for (auto& t : m_threads)
            t.join();
        for (Lane& l : m_lanes)
            sftp_close(l.file);
    }

    int64_t read(char* buf, int64_t max) override
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        int64_t filled = 0;
        while (filled < max) {
            if (m_consumed >= m_size || m_shrunk)
                break;
            m_cv.wait(lock, [&] { return m_error || m_ready.count(m_consumed) || filled > 0; });
            if (m_error)
                return filled > 0 ? filled : -m_error;
            auto it = m_ready.find(m_consumed);
            if (it == m_ready.end())
                break;  // hand out what we have; more comes on the next call
            QByteArray& piece = it->second;
            if (piece.isEmpty()) {  // the file got shorter than it was
                m_shrunk = true;
                break;
            }
            const int64_t n = qMin<int64_t>(max - filled, piece.size());
            memcpy(buf + filled, piece.constData(), size_t(n));
            filled += n;
            QByteArray rest = piece.mid(n);
            m_ready.erase(it);
            m_consumed += n;
            if (!rest.isEmpty())
                m_ready.emplace(m_consumed, std::move(rest));
            m_cv.notify_all();  // lanes waiting for room
        }
        return filled;
    }

private:
    void run(size_t k)
    {
        Lane& lane = m_lanes[k];
        const int64_t stride = int64_t(m_chunk) * int64_t(m_lanes.size());
        const int64_t ahead = int64_t(m_chunk) * kWindow * int64_t(m_lanes.size());  // memory bound (~32 MiB)
        struct Request {
            sftp_aio aio;
            int64_t offset;
            size_t length;
        };
        std::deque<Request> pending;
        int64_t next = int64_t(k) * int64_t(m_chunk);
        auto fail = [&](int e) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_error)
                m_error = e;
            m_cv.notify_all();
        };
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                if (m_stop || m_error)
                    break;
                // Nothing in flight and too far ahead of the reader: wait for room.
                if (pending.empty() && next < m_size) {
                    m_cv.wait(lock, [&] { return m_stop || m_error || next < m_consumed + ahead; });
                    if (m_stop || m_error)
                        break;
                }
            }
            while (pending.size() < size_t(kWindow) && next < m_size) {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (next >= m_consumed + ahead)
                        break;
                }
                const size_t len = size_t(qMin<int64_t>(int64_t(m_chunk), m_size - next));
                sftp_seek64(lane.file, uint64_t(next));
                sftp_aio aio = nullptr;
                if (sftp_aio_begin_read(lane.file, len, &aio) < 0) {
                    fail(lane.lease->error());  // never leave the reader waiting for this piece
                    break;
                }
                pending.push_back({aio, next, len});
                next += stride;
            }
            if (pending.empty())
                break;  // all of this lane's pieces are delivered (or it failed)
            Request r = pending.front();
            pending.pop_front();
            QByteArray data(qsizetype(r.length), Qt::Uninitialized);
            ssize_t n = sftp_aio_wait_read(&r.aio, data.data(), r.length);
            if (n < 0) {
                fail(lane.lease->error());
                break;
            }
            // A short piece in the middle: fetch the rest of it right away.
            while (n > 0 && size_t(n) < r.length) {
                sftp_seek64(lane.file, uint64_t(r.offset + n));
                const ssize_t more = sftp_read(lane.file, data.data() + n, r.length - size_t(n));
                if (more <= 0)
                    break;
                n += more;
            }
            data.truncate(n);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_ready.emplace(r.offset, std::move(data));
            m_cv.notify_all();
        }
        // Collect what's still in flight (libssh needs every request waited for).
        std::vector<char> sink(m_chunk);
        for (Request& r : pending)
            sftp_aio_wait_read(&r.aio, sink.data(), r.length);
    }

    std::vector<Lane> m_lanes;
    const int64_t m_size;  // read by the lane threads: never changes
    size_t m_chunk;
    bool m_shrunk = false;
    std::vector<std::thread> m_threads;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::map<int64_t, QByteArray> m_ready;
    int64_t m_consumed = 0;
    int m_error = 0;
    bool m_stop = false;
};

// Writing: one lane on the caller's thread until the file is clearly big,
// then lanes with their own threads (each pipelining its own pieces).
class SftpWriter : public Writer {
public:
    SftpWriter(SftpBackend* backend, QString location, Lane first, QByteArray path)
        : m_backend(backend), m_location(std::move(location)), m_path(std::move(path))
    {
        m_lanes.push_back(std::move(first));
        m_chunk = m_lanes.front().lease->maxWrite;
    }
    ~SftpWriter() override
    {
        if (!m_lanes.empty()) {
            stopLanes();
            waitAll();
            for (Lane& l : m_lanes)
                sftp_close(l.file);
        }
    }

    int64_t write(const char* buf, int64_t len) override
    {
        if (!m_triedLanes && m_offset + len >= kLaneThreshold)
            startLanes();
        if (!m_threads.empty())
            return writeLaned(buf, len);
        int64_t sent = 0;
        while (sent < len) {
            if (m_pending.size() >= size_t(kWindow))
                if (const int e = waitOne())
                    return -e;
            const size_t n = size_t(qMin<int64_t>(len - sent, int64_t(m_chunk)));
            sftp_seek64(m_lanes.front().file, uint64_t(m_offset));
            sftp_aio aio = nullptr;
            if (sftp_aio_begin_write(m_lanes.front().file, buf + sent, n, &aio) < 0)
                return -m_lanes.front().lease->error();
            m_pending.push_back({aio, n});
            sent += int64_t(n);
            m_offset += int64_t(n);
        }
        return sent;
    }

    int finish(int64_t mtime) override
    {
        int err = stopLanes();
        if (const int e = waitAll(); e && !err)
            err = e;
        for (size_t i = 1; i < m_lanes.size(); ++i)
            sftp_close(m_lanes[i].file);
        Lane& main = m_lanes.front();
        if (sftp_close(main.file) != 0 && !err)
            err = main.lease->error();
        if (!err && mtime > 0) {
            struct timeval tv[2] = {{time_t(mtime), 0}, {time_t(mtime), 0}};
            sftp_utimes(main.lease->sftp, m_path.constData(), tv);  // best effort
        }
        m_lanes.clear();
        return err;
    }

private:
    struct Request {
        sftp_aio aio;
        size_t length;
    };
    struct Piece {
        int64_t offset;
        QByteArray data;
    };

    void startLanes()
    {
        m_triedLanes = true;
        std::vector<Lane> extra = openSpareLanes(m_backend, m_location, m_path, O_WRONLY, kLanes - 1);
        if (extra.empty())
            return;
        if (waitAll() != 0)
            return;  // the error shows up on the next write
        for (Lane& l : extra) {
            m_chunk = qMin(m_chunk, l.lease->maxWrite);
            m_lanes.push_back(std::move(l));
        }
        m_queues.resize(m_lanes.size());
        for (size_t k = 0; k < m_lanes.size(); ++k)
            m_threads.emplace_back([this, k] { runLane(k); });
    }

    int64_t writeLaned(const char* buf, int64_t len)
    {
        const int64_t limit = int64_t(m_chunk) * kWindow * int64_t(m_lanes.size());  // bytes queued at most
        int64_t sent = 0;
        while (sent < len) {
            const int64_t n = qMin<int64_t>(len - sent, int64_t(m_chunk));
            const size_t k = size_t((m_offset / int64_t(m_chunk)) % int64_t(m_lanes.size()));
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [&] { return m_error || m_queued < limit; });
            if (m_error)
                return -m_error;
            m_queues[k].push_back({m_offset, QByteArray(buf + sent, qsizetype(n))});
            m_queued += n;
            m_cv.notify_all();
            sent += n;
            m_offset += n;
        }
        return sent;
    }

    void runLane(size_t k)
    {
        Lane& lane = m_lanes[k];
        std::deque<std::pair<Request, int64_t>> pending;  // request, bytes
        auto settle = [&](bool all) {
            while (!pending.empty() && (all || pending.size() >= size_t(kWindow))) {
                auto [r, bytes] = pending.front();
                pending.pop_front();
                const ssize_t n = sftp_aio_wait_write(&r.aio);
                std::lock_guard<std::mutex> lock(m_mutex);
                if ((n < 0 || size_t(n) != r.length) && !m_error)
                    m_error = n < 0 ? lane.lease->error() : EIO;
                m_queued -= bytes;
                m_cv.notify_all();
            }
        };
        for (;;) {
            Piece piece;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                if (m_queues[k].empty() && !pending.empty()) {
                    lock.unlock();
                    settle(true);  // nothing new yet: finish what's in flight
                    continue;
                }
                m_cv.wait(lock, [&] { return !m_queues[k].empty() || m_closing || m_error; });
                if (m_queues[k].empty())
                    break;  // closing (or failed) and nothing left
                piece = std::move(m_queues[k].front());
                m_queues[k].pop_front();
            }
            if (m_error) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_queued -= piece.data.size();
                continue;
            }
            sftp_seek64(lane.file, uint64_t(piece.offset));
            sftp_aio aio = nullptr;
            if (sftp_aio_begin_write(lane.file, piece.data.constData(), size_t(piece.data.size()), &aio) < 0) {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_error)
                    m_error = lane.lease->error();
                m_queued -= piece.data.size();
                m_cv.notify_all();
                continue;
            }
            pending.push_back({{aio, size_t(piece.data.size())}, piece.data.size()});
            settle(false);
        }
        settle(true);
    }

    int stopLanes()
    {
        if (m_threads.empty())
            return 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_closing = true;
        }
        m_cv.notify_all();
        for (auto& t : m_threads)
            t.join();
        m_threads.clear();
        return m_error;
    }

    int waitOne()
    {
        Request r = m_pending.front();
        m_pending.pop_front();
        const ssize_t n = sftp_aio_wait_write(&r.aio);
        if (n < 0 || size_t(n) != r.length)
            return n < 0 ? m_lanes.front().lease->error() : EIO;
        return 0;
    }
    int waitAll()
    {
        int err = 0;
        while (!m_pending.empty())
            if (const int e = waitOne(); e && !err)
                err = e;
        return err;
    }

    SftpBackend* m_backend;
    QString m_location;
    QByteArray m_path;
    std::vector<Lane> m_lanes;
    size_t m_chunk;
    std::deque<Request> m_pending;  // single-lane mode
    int64_t m_offset = 0;
    bool m_triedLanes = false;
    // laned mode
    std::vector<std::thread> m_threads;
    std::vector<std::deque<Piece>> m_queues;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    int64_t m_queued = 0;
    int m_error = 0;
    bool m_closing = false;
};

std::vector<Lane> openSpareLanes(SftpBackend* backend, const QString& location, const QByteArray& path, int flags,
                                 int count)
{
    std::vector<Lane> lanes;
    std::mutex m;
    std::vector<std::thread> threads;
    for (int i = 0; i < count; ++i) {
        threads.emplace_back([&] {
            int err = 0;
            Lease l = backend->acquire(location, err, nullptr, /*spare=*/true);
            if (!l)
                return;
            sftp_file f = sftp_open(l->sftp, path.constData(), flags, 0);
            if (!f)
                return;  // the lease goes back to the pool
            std::lock_guard<std::mutex> lock(m);
            lanes.push_back({std::move(l), f});
        });
    }
    for (auto& t : threads)
        t.join();
    return lanes;
}

std::unique_ptr<Reader> SftpBackend::openRead(const QString& path, int& err)
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        Lease l = acquire(path, err, nullptr);
        if (!l)
            return nullptr;
        const QByteArray p = remotePath(path);
        sftp_attributes a = sftp_stat(l->sftp, p.constData());
        if (!a) {
            err = l->error();
            if (l->dead) {
                dropIdle(path);
                continue;
            }
            return nullptr;
        }
        const int64_t size = int64_t(a->size);
        sftp_attributes_free(a);
        sftp_file f = sftp_open(l->sftp, p.constData(), O_RDONLY, 0);
        if (!f) {
            err = l->error();
            return nullptr;
        }
        err = 0;
        if (size >= kLaneThreshold) {
            std::vector<Lane> extra = openSpareLanes(this, path, p, O_RDONLY, kLanes - 1);
            if (qEnvironmentVariableIsSet("TANDEM_SFTP_DEBUG"))
                fprintf(stderr, "sftp read %s: %d lanes\n", p.constData(), int(extra.size()) + 1);
            if (!extra.empty()) {
                std::vector<Lane> lanes;
                lanes.push_back({std::move(l), f});
                for (Lane& e : extra)
                    lanes.push_back(std::move(e));
                return std::make_unique<LanedReader>(std::move(lanes), size);
            }
        }
        return std::make_unique<SftpReader>(Lane{std::move(l), f}, size);
    }
    return nullptr;
}

std::unique_ptr<Writer> SftpBackend::openWrite(const QString& path, bool exclusive, uint32_t mode, int& err)
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        Lease l = acquire(path, err, nullptr);
        if (!l)
            return nullptr;
        const QByteArray p = remotePath(path);
        const int flags = O_WRONLY | O_CREAT | (exclusive ? O_EXCL : O_TRUNC);
        sftp_file f = sftp_open(l->sftp, p.constData(), flags, mode_t(mode & 07777 ? mode & 07777 : 0644));
        if (!f) {
            err = l->error();
            if (l->dead) {
                dropIdle(path);
                continue;
            }
            if (exclusive && err == EIO) {  // "failure" for "exists" on some servers
                if (sftp_attributes a = sftp_lstat(l->sftp, p.constData())) {
                    sftp_attributes_free(a);
                    err = EEXIST;
                }
            }
            return nullptr;
        }
        err = 0;
        return std::make_unique<SftpWriter>(this, path, Lane{std::move(l), f}, p);
    }
    return nullptr;
}

}  // namespace

void registerSftpBackend()
{
    static auto backend = std::make_shared<SftpBackend>();
    registerBackend(QStringLiteral("sftp"), backend);
}

}  // namespace tandem

#else

namespace tandem {
void registerSftpBackend() {}
}  // namespace tandem

#endif

namespace tandem {

void registerSmbBackend();  // SmbBackend.cpp

void registerBuiltinBackends()
{
    registerSftpBackend();
    registerSmbBackend();
}

}  // namespace tandem
