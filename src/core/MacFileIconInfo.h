#pragma once

namespace tandem {

// Blocking directory metadata helpers, only called by listing workers.
bool macLocalIconMetadata(int dirfd);
bool macHasCustomIcon(int dirfd, const char* name);

}  // namespace tandem
