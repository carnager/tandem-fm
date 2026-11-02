#pragma once

#include "core/FileEntry.h"

#include <QIcon>

namespace tandem {

// Icons by extension, resolved once per extension from filename globs only.
// Never sniffs file contents (that would mean disk I/O on the UI thread).
class IconCache {
public:
    static const QIcon& forEntry(const FileEntry& e);
    static QIcon themed(const char* name, const char* fallback = nullptr);
};

}  // namespace tandem
