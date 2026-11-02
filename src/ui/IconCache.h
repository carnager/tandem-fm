#pragma once

#include "core/FileEntry.h"

#include <QIcon>
#include <QList>

namespace tandem {

// Icons by extension, resolved once per extension from filename globs only.
// Never sniffs file contents (that would mean disk I/O on the UI thread).
class IconCache {
public:
    static const QIcon& forEntry(const FileEntry& e);
    static QIcon themed(const char* name, const char* fallback = nullptr);

    // Switch icon theme ("" = the system's) and drop cached icons.
    static void setTheme(const QString& name);
    static QString systemTheme();
    static void clear();
};

struct IconTheme {
    QString id;    // directory name, used with QIcon::setThemeName
    QString name;  // display name from index.theme
};

// Installed icon themes (cursor-only themes excluded). Blocking: reads
// index.theme files, so call it from a worker.
QList<IconTheme> scanIconThemes();

}  // namespace tandem
