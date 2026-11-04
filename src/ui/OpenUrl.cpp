#include "ui/OpenUrl.h"

#include "core/Launch.h"

#include <QDesktopServices>
#include <QUrl>

namespace tandem {

bool openExternally(const QUrl& url)
{
    if (const auto pretend = guardLaunch(QStringLiteral("open-url"), {url.toString()}))
        return *pretend;
    return QDesktopServices::openUrl(url);
}

}  // namespace tandem
