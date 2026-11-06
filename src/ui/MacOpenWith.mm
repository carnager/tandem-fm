#import <AppKit/AppKit.h>

#include "ui/MacOpenWith.h"

#include "core/Util.h"
#include "ui/MacFileIcons.h"

#include <QApplication>
#include <QHash>
#include <QSet>
#include <QThread>

#include <memory>

namespace tandem {

QList<MacApp> macAppsForAll(const QStringList& paths)
{
    @autoreleasepool {
        NSWorkspace* workspace = NSWorkspace.sharedWorkspace;
        QList<MacApp> apps;
        bool first = true;
        for (const QString& path : paths) {
            NSURL* url = [NSURL fileURLWithPath:path.toNSString()];
            QSet<QString> these;
            for (NSURL* app in [workspace URLsForApplicationsToOpenURL:url])
                these.insert(QString::fromNSString(app.path));
            if (!first) {
                apps.removeIf([&](const MacApp& app) { return !these.contains(app.path); });
                continue;
            }
            first = false;
            NSURL* preferred = [workspace URLForApplicationToOpenURL:url];
            const QString defaultPath = preferred ? QString::fromNSString(preferred.path) : QString();
            // Launch Services' order: the apps that claim the type most strongly first.
            for (NSURL* app in [workspace URLsForApplicationsToOpenURL:url]) {
                MacApp entry;
                entry.path = QString::fromNSString(app.path);
                if (std::any_of(apps.cbegin(), apps.cend(), [&](const MacApp& a) { return a.path == entry.path; }))
                    continue;
                entry.name = QString::fromNSString([NSFileManager.defaultManager displayNameAtPath:app.path]);
                if (entry.name.endsWith(QLatin1String(".app")))
                    entry.name.chop(4);
                entry.isDefault = entry.path == defaultPath;
                apps.append(entry);
            }
            std::stable_partition(apps.begin(), apps.end(), [](const MacApp& a) { return a.isDefault; });
        }
        return apps;
    }
}

QIcon macAppIcon(const QString& appPath)
{
    Q_ASSERT(QThread::currentThread() == qApp->thread());
    static QHash<QString, QIcon> icons;
    auto it = icons.find(appPath);
    if (it == icons.end())
        it = icons.insert(appPath, macPathIcon(appPath));
    return *it;
}

void macOpenWith(const QString& appPath, const QStringList& paths, std::function<void(const QString& error)> done)
{
    @autoreleasepool {
        NSMutableArray<NSURL*>* urls = [NSMutableArray arrayWithCapacity:NSUInteger(paths.size())];
        for (const QString& path : paths)
            [urls addObject:[NSURL fileURLWithPath:path.toNSString()]];
        NSURL* app = [NSURL fileURLWithPath:appPath.toNSString()];
        auto callback = std::make_shared<std::function<void(const QString&)>>(std::move(done));
        [NSWorkspace.sharedWorkspace openURLs:urls
                         withApplicationAtURL:app
                                configuration:[NSWorkspaceOpenConfiguration configuration]
                            completionHandler:^(NSRunningApplication*, NSError* error) {
                                const QString message = error ? QString::fromNSString(error.localizedDescription) : QString();
                                postToUi([callback, message] {
                                    if (*callback)
                                        (*callback)(message);
                                });
                            }];
    }
}

}  // namespace tandem
