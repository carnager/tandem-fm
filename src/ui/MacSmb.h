#pragma once
#include "ui/Network.h"
#include <QUrl>
#include <optional>

namespace tandem {
struct MacSmbPlan {
    QUrl mountUrl;  // share root, without a password
    QString relativePath;
    QString user;
    QString password;  // transient; passed directly to NetFS, never to settings or a process
};
struct MacSmbVolume {
    QUrl url;
    QString path;
};
struct MacSmbResult {
    int status = 0;
    QStringList paths;
};
struct MacSmbTestHooks {
    std::function<QList<MacSmbVolume>()> volumes;
    std::function<void(const MacSmbPlan&, std::function<void(MacSmbResult)>)> mount;
    int timeoutMs = 120000;
};

class MacSmb {
public:
    static std::optional<MacSmbPlan> plan(const QString& uri, QString* error = nullptr);
    static bool sameShare(const QUrl& requested, const QUrl& mounted);
    static QString targetPath(const QString& root, const QString& relative);
    static QList<MacSmbVolume> mountedVolumes();  // local volume metadata only; call on a worker
    static void mount(const QString& uri, QWidget* parent, Network::Done done);
    static QString mountRoot(const QString& path);
    static void unmount(const QString& path, std::function<void(const QString&)> done);
    static void setHooksForTests(MacSmbTestHooks hooks);
};
}  // namespace tandem
