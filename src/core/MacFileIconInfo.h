#pragma once

#include <QString>

namespace tandem {

// Blocking directory metadata helpers, only called by listing workers.
bool macLocalIconMetadata(int dirfd);
bool macIsPackage(const QString& name);
bool macHasCustomIcon(int dirfd, const char* name);

}  // namespace tandem
