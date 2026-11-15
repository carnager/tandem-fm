#pragma once

#include "core/FileEntry.h"

#include <QColor>
#include <QIcon>
#include <QList>

namespace tandem {

// Icons by extension, resolved once per extension from filename globs only.
// Never sniffs file contents (that would mean disk I/O on the UI thread).
class IconCache {
public:
    static const QIcon& forEntry(const FileEntry& e);
    static QIcon themed(const char* name, const char* fallback = nullptr);
    // One colour, for the window's own controls: the -symbolic version
    // (else the icon itself) as a silhouette in color, so it shows whether
    // the icon theme is made for light or dark backgrounds.
    static QIcon glyph(const char* name, const char* fallback, const QColor& color);

    // Switch icon theme ("" = the system's) and drop cached icons.
    static void setTheme(const QString& name);
    static QString systemTheme();
    static bool usesSystemTheme();
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
