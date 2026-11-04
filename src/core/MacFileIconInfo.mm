#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "core/MacFileIconInfo.h"

#include <QHash>
#include <sys/attr.h>
#include <sys/mount.h>
#include <unistd.h>
#include <cstdint>

namespace tandem {

bool macLocalIconMetadata(int dirfd)
{
    struct statfs fs;
    return fstatfs(dirfd, &fs) == 0 && (fs.f_flags & MNT_LOCAL);
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
