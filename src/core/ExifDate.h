#pragma once

#include <QString>

#include <cstdint>

namespace tandem {

// When a photo was taken (EXIF DateTimeOriginal, else DateTime), as seconds
// since the epoch in local time; 0 if the file has no such date. Reads only
// the header of JPEG and TIFF-based files (most camera raws). Blocking.
int64_t exifTakenTime(const QString& path);

}  // namespace tandem
