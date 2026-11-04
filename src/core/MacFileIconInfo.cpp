#include "core/MacFileIconInfo.h"

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
