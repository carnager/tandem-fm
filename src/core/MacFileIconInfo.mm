#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "core/MacFileIconInfo.h"
#include "core/Sorting.h"

#include <sys/attr.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <unistd.h>
#include <cstdint>
#include <QHash>
#include <QFile>
#include <QScopeGuard>
#include <fcntl.h>
#include <array>
#include <cstring>

namespace tandem {

bool macLocalIconMetadata(int dirfd)
{
    struct statfs fs;
    return fstatfs(dirfd, &fs) == 0 && (fs.f_flags & MNT_LOCAL);
}

bool macStatEntry(int dirfd, const char* name, FileEntry& e)
{
    struct stat st;
    if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
        return false;
    e.mode = st.st_mode;
    e.size = st.st_size;
    e.mtime = st.st_mtime;
    e.iconRevision = int64_t(st.st_ctimespec.tv_sec) * 1000000000 + st.st_ctimespec.tv_nsec;
    e.uid = st.st_uid;
    e.gid = st.st_gid;
    e.isLink = S_ISLNK(st.st_mode);
    e.isDir = S_ISDIR(st.st_mode);
    if (e.isLink) {
        struct stat target;
        if (fstatat(dirfd, name, &target, 0) == 0) {
            e.isDir = S_ISDIR(target.st_mode);
            e.size = target.st_size;
        } else {
            e.brokenLink = true;
        }
    }
    if (e.isDir)
        e.size = -1;
    return true;
}

std::optional<std::vector<FileEntry>> macListEntries(int dirfd, bool showHidden, const std::atomic<bool>* cancel)
{
    // A separate descriptor leaves readdir available if bulk attributes fail.
    const int fd = openat(dirfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return std::nullopt;
    auto close = qScopeGuard([fd] { ::close(fd); });
    struct attrlist attrs = {};
    attrs.bitmapcount = ATTR_BIT_MAP_COUNT;
    attrs.commonattr = ATTR_BULK_REQUIRED | ATTR_CMN_ERROR | ATTR_CMN_OBJTYPE | ATTR_CMN_MODTIME |
                       ATTR_CMN_CHGTIME | ATTR_CMN_FNDRINFO | ATTR_CMN_OWNERID | ATTR_CMN_GRPID | ATTR_CMN_ACCESSMASK;
    attrs.fileattr = ATTR_FILE_DATALENGTH;
    // getattrlist packs even 64-bit fields at four-byte alignment.
#pragma pack(push, 4)
    struct Record {
        uint32_t length;
        attribute_set_t returned;
        uint32_t error;
        attrreference_t name;
        fsobj_type_t type;
        timespec mtime, ctime;
        unsigned char info[32];
        uint32_t uid, gid, mode;
        off_t size;
    };
#pragma pack(pop)
    alignas(8) std::array<char, 65536> buffer;
    std::vector<FileEntry> entries;
    entries.reserve(128);
    while (!cancel || !cancel->load(std::memory_order_relaxed)) {
        const int count = getattrlistbulk(fd, &attrs, buffer.data(), buffer.size(), FSOPT_PACK_INVAL_ATTRS);
        if (count < 0)
            return std::nullopt;
        if (!count)
            return entries;
        size_t offset = 0;
        for (int i = 0; i < count; ++i) {
            Record record;
            if (offset + sizeof(record) > buffer.size())
                return std::nullopt;
            std::memcpy(&record, buffer.data() + offset, sizeof(record));
            if (record.length < sizeof(record) || record.length > buffer.size() - offset)
                return std::nullopt;
            const char* start = buffer.data() + offset;
            offset += record.length;
            if (record.error)
                return std::nullopt;  // preserve unreadable rows through the stat/readdir fallback
            const auto required = attrs.commonattr & ~(ATTR_CMN_ERROR | ATTR_CMN_FNDRINFO | ATTR_CMN_RETURNED_ATTRS);
            if ((record.returned.commonattr & required) != required)
                return std::nullopt;
            const int64_t nameOffset = int64_t(offsetof(Record, name)) + record.name.attr_dataoffset;
            const uint32_t length = record.name.attr_length;
            if (nameOffset < int64_t(sizeof(record)) || !length || uint64_t(nameOffset) + length > record.length)
                return std::nullopt;
            const char* name = start + nameOffset;
            if (name[length - 1] != '\0')
                return std::nullopt;
            if ((!showHidden && name[0] == '.') || !strcmp(name, ".") || !strcmp(name, ".."))
                continue;
            FileEntry e;
            e.name = QFile::decodeName(QByteArray(name, length - 1));
            e.mode = record.mode & ~S_IFMT;
            switch (record.type) {
            case VREG: e.mode |= S_IFREG; break;
            case VDIR: e.mode |= S_IFDIR; break;
            case VLNK: e.mode |= S_IFLNK; break;
            case VBLK: e.mode |= S_IFBLK; break;
            case VCHR: e.mode |= S_IFCHR; break;
            case VFIFO: e.mode |= S_IFIFO; break;
            case VSOCK: e.mode |= S_IFSOCK; break;
            default: break;
            }
            e.isDir = record.type == VDIR;
            e.mtime = record.mtime.tv_sec;
            e.iconRevision = int64_t(record.ctime.tv_sec) * 1000000000 + record.ctime.tv_nsec;
            e.uid = record.uid;
            e.gid = record.gid;
            e.size = e.isDir ? -1 : record.size;
            if (record.type == VLNK || (!e.isDir && !(record.returned.fileattr & ATTR_FILE_DATALENGTH))) {
                if (!macStatEntry(dirfd, name, e))
                    return std::nullopt;
            }
            e.hasCustomIcon = (record.returned.commonattr & ATTR_CMN_FNDRINFO) ? (record.info[8] & 0x04) != 0
                                                                                  : macHasCustomIcon(dirfd, name);
            e.isPackage = e.isDir && macIsPackage(e.name);
            e.key = makeSortKey(e.name);
            entries.push_back(std::move(e));
        }
    }
    return entries;
}

bool macIsPackage(const QString& name)
{
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0 || dot == name.size() - 1)
        return false;
    const QString extension = name.mid(dot + 1).toLower();
    // Only workers call this; cache type declarations without locking or path I/O.
    thread_local QHash<QString, bool> packages;
    if (auto it = packages.constFind(extension); it != packages.cend())
        return *it;
    bool package = false;
    @autoreleasepool {
        NSString* ext = [[NSString alloc] initWithCharacters:reinterpret_cast<const unichar*>(extension.utf16())
                                                    length:extension.size()];
        UTType* type = [UTType typeWithFilenameExtension:ext conformingToType:UTTypeDirectory];
        package = [type conformsToType:UTTypePackage] || [type conformsToType:UTTypeBundle];
    }
    if (packages.size() >= 512)
        packages.clear();
    packages.insert(extension, package);
    return package;
}

bool macHasCustomIcon(int dirfd, const char* name)
{
    struct attrlist attrs = {};
    attrs.bitmapcount = ATTR_BIT_MAP_COUNT;
    attrs.commonattr = ATTR_CMN_FNDRINFO;
    struct {
        uint32_t length;
        unsigned char info[32];
    } result = {};
    if (getattrlistat(dirfd, name, &attrs, &result, sizeof(result), FSOPT_NOFOLLOW) != 0 ||
        result.length < sizeof(result))
        return false;
    // Finder flags are a big-endian word at byte 8; kHasCustomIcon is 0x0400.
    return (result.info[8] & 0x04) != 0;
}

}  // namespace tandem
