#pragma once

class QUrl;

namespace tandem {

// QDesktopServices::openUrl, through the launch guard (core/Launch.h).
bool openExternally(const QUrl& url);

}  // namespace tandem
