#pragma once

#include <QString>
#include "core/FileEntry.h"
#include <vector>
#include <atomic>
#include <optional>

namespace tandem {

// Blocking directory metadata helpers, only called by listing workers.
bool macLocalIconMetadata(int dirfd);
std::optional<std::vector<FileEntry>> macListEntries(int dirfd, bool showHidden, const std::atomic<bool>* cancel = nullptr);
bool macStatEntry(int dirfd, const char* name, FileEntry& entry);
bool macIsPackage(const QString& name);
bool macHasCustomIcon(int dirfd, const char* name);

}  // namespace tandem
