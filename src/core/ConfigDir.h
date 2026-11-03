#pragma once

#include <QString>
#include <QStringList>

namespace tandem {

// Where Tandem keeps its settings, bookmarks and folder history: the
// platform's usual place, or a folder chosen with --config-dir DIR or
// TANDEM_CONFIG_DIR (e.g. one that Dropbox or Syncthing keeps in sync).
// A chosen folder holds plain INI files on every platform, so Linux and
// macOS can share it. Files go to DIR/tandem-fm/.

// Takes --config-dir DIR / --config-dir=DIR out of `args`, falls back to
// TANDEM_CONFIG_DIR, and sets it up. Call before anything reads settings.
void setupConfigDir(QStringList& args);
void useConfigDir(const QString& dir);  // empty: the platform default
QString configDir();                    // the chosen folder, or empty

}  // namespace tandem
