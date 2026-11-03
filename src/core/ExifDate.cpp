#include "core/ExifDate.h"

#include <QDateTime>
#include <QFile>

namespace tandem {

namespace {

struct Tiff {
    const uchar* data;
    qsizetype size;
    bool little;

    bool has(qsizetype at, qsizetype n) const { return at >= 0 && n >= 0 && at + n <= size; }
    uint16_t u16(qsizetype at) const
    {
        return little ? uint16_t(data[at] | data[at + 1] << 8) : uint16_t(data[at] << 8 | data[at + 1]);
    }
    uint32_t u32(qsizetype at) const
    {
        return little ? uint32_t(data[at] | data[at + 1] << 8 | data[at + 2] << 16 | uint32_t(data[at + 3]) << 24)
                      : uint32_t(uint32_t(data[at]) << 24 | data[at + 1] << 16 | data[at + 2] << 8 | data[at + 3]);
    }
    // Value of an ASCII tag or a pointer tag in the IFD at offset.
    bool find(uint32_t ifd, uint16_t tag, uint32_t* value, uint32_t* count) const
    {
        if (!has(ifd, 2))
            return false;
        const int n = u16(ifd);
        for (int i = 0; i < n && i < 512; ++i) {
            const qsizetype e = ifd + 2 + qsizetype(i) * 12;
            if (!has(e, 12))
                return false;
            if (u16(e) == tag) {
                *count = u32(e + 4);
                *value = *count <= 4 && u16(e + 2) == 2 ? uint32_t(e + 8) : u32(e + 8);  // short strings sit inline
                return true;
            }
        }
        return false;
    }
    int64_t date(uint32_t ifd, uint16_t tag) const
    {
        uint32_t at = 0, count = 0;
        if (!find(ifd, tag, &at, &count) || count < 19 || !has(at, 19))
            return 0;
        const QString s = QString::fromLatin1(reinterpret_cast<const char*>(data + at), 19);
        const QDateTime t = QDateTime::fromString(s, QStringLiteral("yyyy:MM:dd HH:mm:ss"));
        return t.isValid() ? t.toSecsSinceEpoch() : 0;
    }
};

int64_t fromTiff(const uchar* p, qsizetype size)
{
    if (size < 8)
        return 0;
    Tiff t{p, size, p[0] == 'I'};
    if (!(p[0] == 'I' && p[1] == 'I') && !(p[0] == 'M' && p[1] == 'M'))
        return 0;
    const uint32_t ifd0 = t.u32(4);
    uint32_t exif = 0, count = 0;
    if (t.find(ifd0, 0x8769, &exif, &count)) {
        if (const int64_t v = t.date(exif, 0x9003))  // DateTimeOriginal
            return v;
        if (const int64_t v = t.date(exif, 0x9004))  // DateTimeDigitized
            return v;
    }
    return t.date(ifd0, 0x0132);  // DateTime
}

}  // namespace

int64_t exifTakenTime(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    const QByteArray head = f.read(256 * 1024);
    const auto* p = reinterpret_cast<const uchar*>(head.constData());
    const qsizetype n = head.size();
    if (n >= 4 && (p[0] == 'I' || p[0] == 'M'))
        return fromTiff(p, n);
    if (n < 4 || p[0] != 0xFF || p[1] != 0xD8)
        return 0;
    qsizetype at = 2;
    while (at + 4 <= n && p[at] == 0xFF) {
        const uchar marker = p[at + 1];
        const qsizetype len = qsizetype(p[at + 2]) << 8 | p[at + 3];
        if (marker == 0xDA || len < 2)  // image data starts: no more headers
            break;
        if (marker == 0xE1 && at + 4 + 6 <= n && memcmp(p + at + 4, "Exif\0\0", 6) == 0)
            return fromTiff(p + at + 10, qMin(len - 8, n - at - 10));
        at += 2 + len;
    }
    return 0;
}

}  // namespace tandem
