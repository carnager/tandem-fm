#pragma once

#include <QColor>
#include <QList>
#include <QPair>
#include <QString>

#include <optional>

namespace tandem {

struct FileEntry;

// File names coloured by type (Settings › Appearance). The colours come from
// the theme's "files" section: kinds ("folder", "executable", "image",
// "video", "audio", "archive", "document") and own rules by name pattern
// ("*.rs") or MIME type ("text/x-python"). Types are told by the name, so
// painting never reads a file.

void setFileColorsEnabled(bool on);
bool fileColorsEnabled();

// The colour for a name, if any. UI thread only (cached).
std::optional<QColor> fileColor(const FileEntry& e);

// The kinds, for themes that leave them out: derived from their colours.
QList<QPair<QString, QColor>> defaultFileColors(const struct Theme& t);

}  // namespace tandem
