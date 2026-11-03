#pragma once

#include "core/Job.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>

namespace tandem {

// Permissions as Linux has them: mode bits, owner/group, POSIX ACLs (extra
// users and groups, and for folders the defaults new items inherit) and
// filesystem flags (chattr). Blocking: call from workers.

enum Perm : uint32_t { PermRead = 4, PermWrite = 2, PermExec = 1 };

struct AclEntry {
    bool isGroup = false;
    uint32_t id = 0;
    QString name;    // user or group name (or the number if unknown)
    uint32_t perms = 0;  // Perm bits
    bool operator==(const AclEntry& o) const { return isGroup == o.isGroup && id == o.id && perms == o.perms; }
};

// Filesystem flags people can set themselves (chattr), and the two that need root.
enum FileFlag : uint32_t {
    FlagNoCow = 1u << 0,     // C: no copy-on-write (btrfs: VM images, databases)
    FlagCompress = 1u << 1,  // c: compress (btrfs)
    FlagNoAtime = 1u << 2,   // A: don't update access time
    FlagNoDump = 1u << 3,    // d: skip in dump backups
    FlagAppend = 1u << 4,    // a: append only (root)
    FlagImmutable = 1u << 5, // i: can't be changed or deleted (root)
};

struct PermInfo {
    int error = 0;
    bool isDir = false;
    bool isLink = false;
    uint32_t mode = 0;  // 07777 bits
    uint32_t uid = 0, gid = 0;
    QString owner, group;
    bool aclSupported = false;
    QList<AclEntry> acl;         // named entries (extra users and groups)
    bool hasDefault = false;     // folders: a default ACL exists
    uint32_t defaultBase = 0;    // its user/group/other part, as mode bits (0777)
    QList<AclEntry> defaultAcl;  // its named entries
    bool flagsSupported = false;
    uint32_t flags = 0;          // FileFlag bits
    uint32_t flagsOffered = 0;   // the flags this filesystem knows (and that are worth offering)
    QString fsName;              // "btrfs", "ext4", "vfat", …
    // false where the filesystem has no Linux permissions (FAT, exFAT, ISO):
    // what ls shows comes from mount options and can't be changed per file.
    bool posixPerms = true;
};

PermInfo readPermissions(const QString& path);

// What to change. Bits and entries that aren't mentioned stay as they are on
// each item, so one change can apply to many different items.
struct PermChange {
    uint32_t setBits = 0, clearBits = 0;  // 07777
    std::optional<uint32_t> uid, gid;
    QList<AclEntry> aclSet;               // add or update (by user/group + id)
    QList<AclEntry> aclRemove;            // remove (perms ignored)
    bool clearAcl = false;                // drop all extra entries first
    // Folders: the defaults new items inside inherit.
    std::optional<bool> defaultEnabled;   // false removes the default ACL
    QList<AclEntry> defaultSet, defaultRemove;
    uint32_t flagsSet = 0, flagsClear = 0;

    bool recursive = false;
    // Applying "read" to a tree: folders that can be read stay openable (x),
    // files only get x where they had it (like chmod's X).
    bool smartExec = true;

    bool isEmpty() const
    {
        return !setBits && !clearBits && !uid && !gid && aclSet.isEmpty() && aclRemove.isEmpty() && !clearAcl &&
               !defaultEnabled && defaultSet.isEmpty() && defaultRemove.isEmpty() && !flagsSet && !flagsClear;
    }
};

// Applies change to one item (not recursive). Returns 0 or an errno; *what
// says which part failed.
int applyPermissions(const QString& path, const PermChange& change, QString* what = nullptr);

// "rwxr-x---" (with s/S/t/T for the special bits).
QString symbolicMode(uint32_t mode, bool isDir);

// Users and groups for pickers: (name, id), sorted; system accounts last.
QList<QPair<QString, uint32_t>> listUsers();
QList<QPair<QString, uint32_t>> listGroups();
// Groups the current user may hand files to (all of them for root).
QList<uint32_t> ownGroups();

// Many items, and with change.recursive everything inside folders. Symlinks
// are left alone (changing one changes its target).
class PermissionsJob : public Job {
public:
    PermissionsJob(QStringList paths, PermChange change);

protected:
    void run() override;

private:
    void apply(const QString& path, bool top);
    QStringList m_paths;
    PermChange m_change;
};

}  // namespace tandem
