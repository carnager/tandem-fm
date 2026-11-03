#include "core/Permissions.h"

#include "core/Util.h"

#include <QDir>

#include <algorithm>

#ifdef TANDEM_HAVE_ACL
#include <acl/libacl.h>
#include <sys/acl.h>
#endif
#include <fcntl.h>
#include <grp.h>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/vfs.h>
#else
#include <sys/mount.h>
#endif
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace tandem {

namespace {

QString userName(uint32_t uid)
{
    struct passwd pw, *res = nullptr;
    char buf[4096];
    if (getpwuid_r(uid, &pw, buf, sizeof buf, &res) == 0 && res)
        return QString::fromLocal8Bit(res->pw_name);
    return QString::number(uid);
}

QString groupName(uint32_t gid)
{
    struct group gr, *res = nullptr;
    char buf[8192];
    if (getgrgid_r(gid, &gr, buf, sizeof buf, &res) == 0 && res)
        return QString::fromLocal8Bit(res->gr_name);
    return QString::number(gid);
}

#ifdef TANDEM_HAVE_ACL
uint32_t permsOf(acl_entry_t e)
{
    acl_permset_t ps;
    if (acl_get_permset(e, &ps) != 0)
        return 0;
    uint32_t p = 0;
    if (acl_get_perm(ps, ACL_READ) == 1)
        p |= PermRead;
    if (acl_get_perm(ps, ACL_WRITE) == 1)
        p |= PermWrite;
    if (acl_get_perm(ps, ACL_EXECUTE) == 1)
        p |= PermExec;
    return p;
}

struct ReadAcl {
    bool ok = false;
    int mask = -1;
    QList<AclEntry> named;
    uint32_t base = 0;  // user/group/other as 0777 bits (group = the owning group, not the mask)
    int entries = 0;
};

ReadAcl readAcl(const QByteArray& path, acl_type_t type)
{
    ReadAcl r;
    acl_t acl = acl_get_file(path.constData(), type);
    if (!acl)
        return r;
    r.ok = true;
    acl_entry_t e;
    for (int id = ACL_FIRST_ENTRY; acl_get_entry(acl, id, &e) == 1; id = ACL_NEXT_ENTRY) {
        ++r.entries;
        acl_tag_t tag;
        acl_get_tag_type(e, &tag);
        const uint32_t p = permsOf(e);
        switch (tag) {
        case ACL_USER_OBJ: r.base |= p << 6; break;
        case ACL_GROUP_OBJ: r.base |= p << 3; break;
        case ACL_OTHER: r.base |= p; break;
        case ACL_USER:
        case ACL_GROUP: {
            AclEntry a;
            a.isGroup = tag == ACL_GROUP;
            if (void* q = acl_get_qualifier(e)) {
                a.id = a.isGroup ? *static_cast<gid_t*>(q) : *static_cast<uid_t*>(q);
                acl_free(q);
            }
            a.name = a.isGroup ? groupName(a.id) : userName(a.id);
            a.perms = p;
            r.named << a;
            break;
        }
        case ACL_MASK: r.mask = int(p); break;
        default: break;
        }
    }
    acl_free(acl);
    // What applies is the entry limited by the mask (inherited ACLs can have
    // a narrow one). Writes recalculate the mask, so showing the effective
    // rights keeps them exactly as they are.
    if (r.mask >= 0) {
        r.base = (r.base & ~0070u) | (r.base & (uint32_t(r.mask) << 3));
        for (AclEntry& a : r.named)
            a.perms &= uint32_t(r.mask);
    }
    return r;
}

bool addEntry(acl_t* acl, acl_tag_t tag, uint32_t perms, const uint32_t* qualifier = nullptr)
{
    acl_entry_t e;
    if (acl_create_entry(acl, &e) != 0 || acl_set_tag_type(e, tag) != 0)
        return false;
    if (qualifier) {
        if (tag == ACL_USER) {
            const uid_t u = *qualifier;
            if (acl_set_qualifier(e, &u) != 0)
                return false;
        } else {
            const gid_t g = *qualifier;
            if (acl_set_qualifier(e, &g) != 0)
                return false;
        }
    }
    acl_permset_t ps;
    acl_get_permset(e, &ps);
    acl_clear_perms(ps);
    if (perms & PermRead)
        acl_add_perm(ps, ACL_READ);
    if (perms & PermWrite)
        acl_add_perm(ps, ACL_WRITE);
    if (perms & PermExec)
        acl_add_perm(ps, ACL_EXECUTE);
    return acl_set_permset(e, ps) == 0;
}

int writeAcl(const QByteArray& path, acl_type_t type, uint32_t base, const QList<AclEntry>& named)
{
    acl_t acl = acl_init(4 + int(named.size()));
    bool ok = acl && addEntry(&acl, ACL_USER_OBJ, (base >> 6) & 7) && addEntry(&acl, ACL_GROUP_OBJ, (base >> 3) & 7) &&
              addEntry(&acl, ACL_OTHER, base & 7);
    for (const AclEntry& a : named)
        ok = ok && addEntry(&acl, a.isGroup ? ACL_GROUP : ACL_USER, a.perms, &a.id);
    if (ok && !named.isEmpty())
        ok = acl_calc_mask(&acl) == 0;  // extra entries get exactly what they're given
    int err = ok ? 0 : (errno ? errno : EINVAL);
    if (ok && acl_set_file(path.constData(), type, acl) != 0)
        err = errno;
    if (acl)
        acl_free(acl);
    return err;
}

#endif

void mergeEntries(QList<AclEntry>& list, const QList<AclEntry>& set, const QList<AclEntry>& remove)
{
    auto same = [](const AclEntry& a, const AclEntry& b) { return a.isGroup == b.isGroup && a.id == b.id; };
    for (const AclEntry& r : remove)
        list.removeIf([&](const AclEntry& a) { return same(a, r); });
    for (const AclEntry& s : set) {
        auto it = std::find_if(list.begin(), list.end(), [&](const AclEntry& a) { return same(a, s); });
        if (it != list.end())
            it->perms = s.perms;
        else
            list << s;
    }
}

#ifdef __linux__
struct FlagBit {
    uint32_t ours;
    int fs;
};
constexpr FlagBit kFlags[] = {{FlagNoCow, FS_NOCOW_FL},     {FlagCompress, FS_COMPR_FL}, {FlagNoAtime, FS_NOATIME_FL},
                              {FlagNoDump, FS_NODUMP_FL},   {FlagAppend, FS_APPEND_FL},  {FlagImmutable, FS_IMMUTABLE_FL}};

int openForFlags(const QByteArray& path)
{
    return ::open(path.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
}

#endif

}  // namespace

PermInfo readPermissions(const QString& path)
{
    PermInfo info;
    const QByteArray np = toNative(path);
    struct stat st;
    if (::lstat(np.constData(), &st) != 0) {
        info.error = errno;
        return info;
    }
    info.isLink = S_ISLNK(st.st_mode);
    if (info.isLink && ::stat(np.constData(), &st) != 0) {  // a link: what it points to
        info.error = errno;
        return info;
    }
    info.isDir = S_ISDIR(st.st_mode);
    info.mode = st.st_mode & 07777;
    info.uid = st.st_uid;
    info.gid = st.st_gid;
    info.owner = userName(st.st_uid);
    info.group = groupName(st.st_gid);

#ifdef TANDEM_HAVE_ACL
    errno = 0;
    const ReadAcl access = readAcl(np, ACL_TYPE_ACCESS);
    info.aclSupported = access.ok;
    if (access.ok && !access.named.isEmpty()) {
        info.acl = access.named;
        info.mode = (info.mode & ~0070u) | (access.base & 0070);  // the group's own bits, not the mask
    }
    if (info.isDir && access.ok) {
        const ReadAcl def = readAcl(np, ACL_TYPE_DEFAULT);
        info.hasDefault = def.ok && def.entries > 0;
        if (info.hasDefault) {
            info.defaultBase = def.base;
            info.defaultAcl = def.named;
        }
    }
#endif

#ifdef __APPLE__
    struct statfs sf;
    if (::statfs(np.constData(), &sf) == 0) {
        info.fsName = QString::fromLocal8Bit(sf.f_fstypename);
        info.posixPerms = info.fsName != QLatin1String("msdos") && info.fsName != QLatin1String("exfat");
    }
#else
    struct statfs sf;
    if (::statfs(np.constData(), &sf) == 0) {
        const uint32_t user = FlagNoAtime | FlagNoDump;
        const uint32_t root = ::geteuid() == 0 ? (FlagAppend | FlagImmutable) : 0;
        switch (uint32_t(sf.f_type)) {
        case 0x9123683E: info.fsName = QStringLiteral("btrfs"); info.flagsOffered = FlagNoCow | FlagCompress | user | root; break;
        case 0xEF53: info.fsName = QStringLiteral("ext4"); info.flagsOffered = user | root; break;
        case 0x58465342: info.fsName = QStringLiteral("xfs"); info.flagsOffered = user | root; break;
        case 0xF2F52010: info.fsName = QStringLiteral("f2fs"); info.flagsOffered = user | root; break;
        case 0xCA451A4E: info.fsName = QStringLiteral("bcachefs"); info.flagsOffered = user | root; break;
        case 0x01021994: info.fsName = QStringLiteral("tmpfs"); info.flagsOffered = user | root; break;
        case 0x4D44: info.fsName = QStringLiteral("vfat"); info.posixPerms = false; break;
        case 0x2011BAB0: info.fsName = QStringLiteral("exfat"); info.posixPerms = false; break;
        case 0x9660: info.fsName = QStringLiteral("iso9660"); info.posixPerms = false; break;
        case 0x5346544E: info.fsName = QStringLiteral("ntfs"); break;
        case 0x6969: info.fsName = QStringLiteral("nfs"); break;
        case 0xFF534D42:
        case 0xFE534D42: info.fsName = QStringLiteral("smb"); break;
        case 0x65735546: info.fsName = QStringLiteral("fuse"); break;
        case 0x2FC12FC1: info.fsName = QStringLiteral("zfs"); break;
        default: break;
        }
    }

    // NFSv4 and SMB mounts answer POSIX ACL reads with the mode bits but
    // keep their real rights as NFSv4/Windows ACLs: no extra entries there.
    // FUSE (sshfs, …) usually can't store them either, though it says "none
    // yet" like a filesystem that could: offer them only where some exist.
    const bool fuseWithout = info.fsName == QLatin1String("fuse") &&
                             ::getxattr(np.constData(), "system.posix_acl_access", nullptr, 0) < 0;
    if (info.fsName == QLatin1String("nfs") || info.fsName == QLatin1String("smb") || fuseWithout) {
        info.aclSupported = false;
        info.acl.clear();
        info.hasDefault = false;
    }

    const int fd = info.flagsOffered ? openForFlags(np) : -1;
    if (fd >= 0) {
        int fs = 0;
        if (::ioctl(fd, FS_IOC_GETFLAGS, &fs) == 0) {
            info.flagsSupported = true;
            for (const FlagBit& f : kFlags)
                if (fs & f.fs)
                    info.flags |= f.ours;
        }
        ::close(fd);
    }
#endif
    return info;
}

int applyPermissions(const QString& path, const PermChange& c, QString* what)
{
    const QByteArray np = toNative(path);
    auto fail = [&](int err, const QString& part) {
        if (what)
            *what = part;
        return err;
    };
    PermInfo now = readPermissions(path);
    if (now.error)
        return fail(now.error, QObject::tr("reading"));

    // Owner first: chown clears setuid/setgid, chmod puts them back.
    if (c.uid || c.gid) {
        const uid_t u = c.uid ? uid_t(*c.uid) : uid_t(-1);
        const gid_t g = c.gid ? gid_t(*c.gid) : gid_t(-1);
        if ((c.uid && *c.uid != now.uid) || (c.gid && *c.gid != now.gid))
            if (::chown(np.constData(), u, g) != 0)
                return fail(errno, QObject::tr("owner"));
    }
    const uint32_t mode = ((now.mode & ~c.clearBits) | c.setBits) & 07777;

    QList<AclEntry> named = c.clearAcl ? QList<AclEntry>{} : now.acl;
    mergeEntries(named, c.aclSet, c.aclRemove);
    const bool aclChanges = c.clearAcl || !c.aclSet.isEmpty() || !c.aclRemove.isEmpty();
    if (mode != now.mode || c.uid || c.gid)
        if (::chmod(np.constData(), mode) != 0)
            return fail(errno, QObject::tr("permissions"));
#ifdef TANDEM_HAVE_ACL
    // With extra entries the group bits ls shows are the ACL mask, so the ACL
    // is written again whenever there are (or were) extra entries.
    if (now.aclSupported && (aclChanges || !named.isEmpty() || !now.acl.isEmpty()))
        if (const int e = writeAcl(np, ACL_TYPE_ACCESS, mode & 0777, named))
            return fail(e, QObject::tr("extra users and groups"));

    if (now.isDir && now.aclSupported) {
        if (c.defaultEnabled && !*c.defaultEnabled) {
            if (now.hasDefault && acl_delete_def_file(np.constData()) != 0)
                return fail(errno, QObject::tr("defaults for new items"));
        } else if ((c.defaultEnabled && *c.defaultEnabled) || !c.defaultSet.isEmpty() || !c.defaultRemove.isEmpty()) {
            QList<AclEntry> def = now.hasDefault ? now.defaultAcl : QList<AclEntry>{};
            mergeEntries(def, c.defaultSet, c.defaultRemove);
            const uint32_t base = now.hasDefault ? now.defaultBase : (mode & 0777);
            if (const int e = writeAcl(np, ACL_TYPE_DEFAULT, base, def))
                return fail(e, QObject::tr("defaults for new items"));
        }
    }
#else
    (void)named;
    (void)aclChanges;
#endif

#ifdef __APPLE__
    if (c.flagsSet || c.flagsClear)
        return fail(ENOTSUP, QObject::tr("flags"));
#else
    if (c.flagsSet || c.flagsClear) {
        const int fd = openForFlags(np);
        if (fd < 0)
            return fail(errno, QObject::tr("flags"));
        int fs = 0;
        int err = ::ioctl(fd, FS_IOC_GETFLAGS, &fs) == 0 ? 0 : errno;
        if (!err) {
            int next = fs;
            for (const FlagBit& f : kFlags) {
                if (c.flagsSet & f.ours)
                    next |= f.fs;
                if (c.flagsClear & f.ours)
                    next &= ~f.fs;
            }
            if (next != fs && ::ioctl(fd, FS_IOC_SETFLAGS, &next) != 0)
                err = errno;
        }
        ::close(fd);
        if (err)
            return fail(err, QObject::tr("flags"));
    }
#endif
    return 0;
}

QString symbolicMode(uint32_t mode, bool isDir)
{
    (void)isDir;
    QString s(9, QLatin1Char('-'));
    const char* rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i)
        if (mode & (0400u >> i))
            s[i] = QLatin1Char(rwx[i]);
    auto special = [&](int at, uint32_t bit, char on, char onNoExec) {
        if (mode & bit)
            s[at] = QLatin1Char(s[at] == QLatin1Char('-') ? onNoExec : on);
    };
    special(2, 04000, 's', 'S');
    special(5, 02000, 's', 'S');
    special(8, 01000, 't', 'T');
    return s;
}

namespace {
template <typename Entry>
QList<QPair<QString, uint32_t>> sortedAccounts(QList<QPair<QString, uint32_t>> list)
{
    // People first (ids 1000–59999 by convention), then root and system accounts.
    auto person = [](uint32_t id) { return id >= 1000 && id < 60000; };
    std::sort(list.begin(), list.end(), [&](const auto& a, const auto& b) {
        if (person(a.second) != person(b.second))
            return person(a.second);
        return a.first.compare(b.first, Qt::CaseInsensitive) < 0;
    });
    return list;
}
}  // namespace

QList<QPair<QString, uint32_t>> listUsers()
{
    QList<QPair<QString, uint32_t>> out;
    setpwent();
    while (struct passwd* p = getpwent())
        out.append({QString::fromLocal8Bit(p->pw_name), p->pw_uid});
    endpwent();
    return sortedAccounts<passwd>(out);
}

QList<QPair<QString, uint32_t>> listGroups()
{
    QList<QPair<QString, uint32_t>> out;
    setgrent();
    while (struct group* g = getgrent())
        out.append({QString::fromLocal8Bit(g->gr_name), g->gr_gid});
    endgrent();
    return sortedAccounts<group>(out);
}

QList<uint32_t> ownGroups()
{
    QList<uint32_t> out;
    if (::geteuid() == 0) {
        for (const auto& g : listGroups())
            out << g.second;
        return out;
    }
    const int n = ::getgroups(0, nullptr);
    std::vector<gid_t> gs(size_t(qMax(0, n)));
    if (n > 0)
        ::getgroups(n, gs.data());
    out << ::getegid();
    for (gid_t g : gs)
        if (!out.contains(g))
            out << g;
    return out;
}

// ---------------------------------------------------------- PermissionsJob

PermissionsJob::PermissionsJob(QStringList paths, PermChange change)
    : Job(tr("Permissions of %1").arg(itemsSummary(paths))), m_paths(std::move(paths)), m_change(std::move(change))
{
}

void PermissionsJob::run()
{
    if (!waitForTurn(m_paths))
        return;
    setState(State::Scanning);
    int64_t total = 0;
    std::function<void(const QString&)> count = [&](const QString& p) {
        ++total;
        if (!m_change.recursive || isCancelled())
            return;
        struct stat st;
        if (::lstat(toNative(p).constData(), &st) != 0 || !S_ISDIR(st.st_mode))
            return;
        for (const QString& n : QDir(p).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
            count(joinPath(p, n));
    };
    for (const QString& p : std::as_const(m_paths))
        count(p);
    m_filesTotal = total;
    setState(State::Running);
    for (const QString& p : std::as_const(m_paths)) {
        if (checkpoint())
            return;
        apply(p, true);
    }
}

void PermissionsJob::apply(const QString& path, bool top)
{
    if (checkpoint())
        return;
    struct stat st;
    if (::lstat(toNative(path).constData(), &st) != 0) {
        addError(tr("%1: %2").arg(path, errnoString(errno)));
        return;
    }
    // Links inside a tree are left alone (chmod would change what they point to).
    const bool link = S_ISLNK(st.st_mode);
    if (!link || top) {
        setCurrent(path);
        PermChange c = m_change;
        const bool dir = link ? QFileInfo(path).isDir() : S_ISDIR(st.st_mode);
        if (!top && m_change.smartExec) {
            // Like chmod's X: folders stay openable where readable, files only
            // keep (or lose) x, never gain it from a tree-wide change.
            const uint32_t x = c.setBits & 0111;
            c.setBits &= ~0111u;
            if (dir) {
                const uint32_t r = c.setBits & 0444;
                c.setBits |= (r >> 2) | x;
            } else if (st.st_mode & 0111) {
                c.setBits |= x;
            }
        }
        if (!dir) {
            c.defaultEnabled.reset();
            c.defaultSet.clear();
            c.defaultRemove.clear();
        }
        QString what;
        if (const int e = applyPermissions(path, c, &what))
            addError(tr("%1 (%2): %3").arg(path, what, errnoString(e)));
        m_filesDone += 1;
    }
    if (m_change.recursive && !link && S_ISDIR(st.st_mode))
        for (const QString& n : QDir(path).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
            apply(joinPath(path, n), false);
}

}  // namespace tandem
