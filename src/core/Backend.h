#pragma once

#include <cerrno>

#include <QHash>

#include "core/DirLister.h"

#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace tandem {

// Files that aren't on the local filesystem (sftp://, smb://, …) are reached
// through a Backend. Locations are plain strings: "scheme://authority/path",
// the root always written with its slash ("sftp://bob@host/").
//
// Local paths never go through here, so everything that is fast today (reflink,
// copy_file_range, inotify) stays as it is; generic code that has to work with
// both sides uses localBackend() for the local one.

QString locationRoot(const QString& path);

struct FileStat {
    bool isDir = false;
    bool isLink = false;
    int64_t size = 0;
    int64_t mtime = 0;
    uint32_t mode = 0;  // st_mode style (type + permission bits)
    uint32_t uid = 0, gid = 0;
};

class Reader {
public:
    virtual ~Reader() = default;
    // Bytes read, 0 at the end, or -errno.
    virtual int64_t read(char* buf, int64_t max) = 0;
};

class Writer {
public:
    virtual ~Writer() = default;
    // Bytes written or -errno.
    virtual int64_t write(const char* buf, int64_t len) = 0;
    // Closes the file and sets its modification time (if mtime > 0). 0 or errno.
    virtual int finish(int64_t mtime) = 0;
};

class Backend {
public:
    virtual ~Backend() = default;

    // All calls block and may be made from any worker thread at the same
    // time (backends keep a connection per thread or a pool). Never call
    // them on the UI thread. They return 0 or an errno value.
    virtual ListResult list(const QString& path, const ListOptions& opt, const std::atomic<bool>* cancel) = 0;
    virtual int stat(const QString& path, FileStat& out) = 0;  // does not follow symlinks
    virtual std::unique_ptr<Reader> openRead(const QString& path, int& err) = 0;
    // exclusive: fail with EEXIST if the file exists; otherwise truncate.
    virtual std::unique_ptr<Writer> openWrite(const QString& path, bool exclusive, uint32_t mode, int& err) = 0;
    virtual int mkdir(const QString& path, uint32_t mode) = 0;
    // Must not replace an existing target (EEXIST).
    virtual int rename(const QString& from, const QString& to) = 0;
    virtual int removeFile(const QString& path) = 0;
    virtual int removeDir(const QString& path) = 0;  // empty folders only
    virtual int readLink(const QString& path, QString& target) { (void)path; (void)target; return ENOTSUP; }
    virtual int symlink(const QString& target, const QString& path) { (void)target; (void)path; return ENOTSUP; }
    virtual int chmod(const QString& path, uint32_t mode) { (void)path; (void)mode; return 0; }

    // Everything under path in one go: folder (relative, "" = path itself)
    // -> its entries. For backends where this beats listing folder by folder
    // by far (one request instead of thousands of round trips). false: can't
    // here, walk instead.
    using Tree = QHash<QString, std::vector<FileEntry>>;
    virtual bool listTree(const QString& path, bool hidden, Tree& out, const std::atomic<bool>* cancel)
    {
        (void)path, (void)hidden, (void)out, (void)cancel;
        return false;
    }

    // Permissions the way the server sees them (its users, its ACLs); false:
    // can't here, the plain mode from stat() is all there is.
    virtual bool readPermissions(const QString& path, struct PermInfo& out)
    {
        (void)path, (void)out;
        return false;
    }
    // ENOTSUP when readPermissions can't either.
    virtual int applyPermissions(const QString& path, const struct PermChange& change, QString* what)
    {
        (void)path, (void)change, (void)what;
        return ENOTSUP;
    }
    // The server's users and groups (name, id), for pickers.
    virtual bool listAccounts(const QString& location, QList<QPair<QString, uint32_t>>& users,
                              QList<QPair<QString, uint32_t>>& groups)
    {
        (void)location, (void)users, (void)groups;
        return false;
    }

    // A password typed into a location ("smb://bob:secret@nas"): used for
    // this session's login instead of asking; locations never keep it.
    virtual void offerPassword(const QString& location, const QString& user, const QString& password)
    {
        (void)location, (void)user, (void)password;
    }

    // Where "sftp://host" without a path should open (the login's home).
    virtual QString homeLocation(const QString& path);
    // Transfers to the same key share a queue (one server = one key).
    virtual QString queueKey(const QString& path) const;
    virtual bool isLocal() const { return false; }
};

// Questions a backend has to ask while connecting (host key, password, key
// passphrase). Asked from worker threads; blocks until answered. The UI
// installs the handler, tests install their own.
struct AuthQuestion {
    // Login: user and password in one go; the answer is "user\npassword".
    enum class Kind { HostKey, Password, Passphrase, Prompt, Login };
    Kind kind = Kind::Password;
    QString location;  // "sftp://bob@host"
    QString message;
    QString detail;    // e.g. the key fingerprint
    QString user;      // Login: the suggested user name
    bool secret = true;
    bool offerRemember = false;  // logins and passwords: "remember in the keyring"
};
// nullopt: the user cancelled. For HostKey any answer means "trust it".
using AuthHandler = std::function<std::optional<QString>(const AuthQuestion&)>;
void setAuthHandler(AuthHandler handler);
std::optional<QString> askAuth(const AuthQuestion& q);
// Whether the last answer on this thread asked to be remembered (keyring).
bool lastAnswerRemember();
// Handlers add this to an answer when "remember" was ticked.
inline constexpr char kRememberMark[] = "\x1Fremember";

// Built-in backends (sftp, smb, …) that this build has.
void registerBuiltinBackends();

// nullptr for local paths. Unknown schemes return nullptr too; check
// isRemoteLocation() for "looks like a URL".
Backend* backendFor(const QString& path);
// The local filesystem as a Backend (for generic copy code).
Backend* localBackend();
// Backend for any location, local included.
Backend* anyBackendFor(const QString& path);
void registerBackend(const QString& scheme, std::shared_ptr<Backend> backend);
bool hasBackend(const QString& scheme);

bool isRemoteLocation(const QString& path);  // "scheme://…"
// "sftp://bob@host" for "sftp://bob@host/srv/x"; empty for local paths.
QString locationAuthority(const QString& path);
QString locationRoot(const QString& path);   // "sftp://bob@host/"
QString locationPath(const QString& path);   // "/srv/x" (always starts with /)
// Collapses "//", "." and ".." in the path part and adds the root slash.
QString normalizeLocation(const QString& uri);

// Generic helpers (any backend, local included). Blocking.
int makeDirectoriesAt(const QString& path);
int renameNoReplaceAt(const QString& from, const QString& to);
int createEmptyFile(const QString& path);
int64_t directorySizeAt(const QString& path, const std::atomic<bool>* cancel);
int statAt(const QString& path, FileStat& out);

// Copies a (possibly remote) file to a local path, for opening or previewing.
// progress(done, total) may be null. 0 or errno.
int downloadFile(const QString& from, const QString& to, const std::atomic<bool>* cancel,
                 const std::function<void(int64_t, int64_t)>& progress = {});

// Where a downloaded copy of a remote file lives (~/.cache/tandem-fm/remote/…),
// and whether that copy still matches the server's size and time.
QString remoteCachePath(const QString& uri);
bool cachedCopyIsFresh(const QString& local, int64_t size, int64_t mtime);

}  // namespace tandem
