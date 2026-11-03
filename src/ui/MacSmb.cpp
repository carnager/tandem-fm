// Apple-only: NetFS delegates authentication and share selection to NetAuthAgent.
#include <dispatch/dispatch.h>
#include <NetFS/NetFS.h>
#include "ui/MacSmb.h"
#include "core/Mounts.h"
#include "core/Util.h"
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QWidget>
#include <memory>

namespace tandem {
namespace {
MacSmbTestHooks g_hooks;
QStringList components(const QUrl& url, bool* valid = nullptr)
{
    QStringList result;
    bool ok = true;
    for (const auto& encoded : url.path(QUrl::FullyEncoded).split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        const QString part = QUrl::fromPercentEncoding(encoded.toUtf8());
        if (part == QLatin1String(".") || part == QLatin1String("..") || part.contains(QLatin1Char('/')) ||
            part.contains(QLatin1Char('\\')) || part.contains(QChar(0))) ok = false;
        result.append(part);
    }
    if (valid) *valid = ok;
    return result;
}
QString firstShare(const QUrl& url) { return components(url).value(0); }
CFURLRef cfUrl(const QUrl& url)
{
    const auto bytes = url.toEncoded(QUrl::FullyEncoded);
    return CFURLCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(bytes.constData()),
                                bytes.size(), kCFStringEncodingUTF8, nullptr);
}
QString failure(int status)
{
    if (status == -128 || status == ECANCELED) return QObject::tr("SMB connection cancelled");
    if (status == ETIMEDOUT) return QObject::tr("The SMB connection timed out");
    if (status == EACCES || status == EPERM) return QObject::tr("macOS denied access to the SMB share");
    if (status > 0) return QObject::tr("macOS could not connect to the SMB share: %1").arg(errnoString(status));
    return QObject::tr("macOS could not connect to the SMB share (error %1)").arg(status);
}
struct MountCall : std::enable_shared_from_this<MountCall> {
    MacSmbPlan plan;
    Network::Done done;
    std::function<QList<MacSmbVolume>()> volumes;
    QPointer<QTimer> timer;
    QMetaObject::Connection ownerGone;
    AsyncRequestID request = nullptr;
    bool completed = false;
    bool received = false;
    CFURLRef url = nullptr;
    CFStringRef user = nullptr, password = nullptr;
    CFMutableDictionaryRef options = nullptr;
    ~MountCall()
    {
        if (url) CFRelease(url);
        if (user) CFRelease(user);
        if (password) CFRelease(password);
        if (options) CFRelease(options);
    }
    void finish(const QString& path, const QString& error, bool cancel = false)
    {
        if (completed) return;
        completed = true;
        if (cancel && request) NetFSMountURLCancel(request);
        request = nullptr;
        QObject::disconnect(ownerGone);
        if (timer) { timer->stop(); timer->deleteLater(); }
        auto completion = std::move(done);
        plan.password.clear();
        if (completion) completion(path, error);
    }
    void result(MacSmbResult result)
    {
        if (completed || received) return;
        received = true; request = nullptr;
        if (result.status != 0) {
            // Some filesystem plug-ins report an existing mount without paths.
            if (result.status != EEXIST) { finish({}, failure(result.status)); return; }
            if (!result.paths.isEmpty()) { open(result.paths.first()); return; }
            const auto self = shared_from_this();
            ioPool()->start([self] {
                QString root;
                for (const auto& volume : self->volumes())
                    if (MacSmb::sameShare(self->plan.mountUrl, volume.url)) { root = volume.path; break; }
                postToUi([self, root] { self->open(root); });
            });
            return;
        }
        open(result.paths.value(0));
    }
    void open(const QString& root)
    {
        if (completed) return;
        const QString path = MacSmb::targetPath(root, plan.relativePath);
        if (path.isEmpty()) { finish({}, QObject::tr("macOS did not return a usable SMB mount point")); return; }
        const auto self = shared_from_this();
        ioPool()->start([self, path] {
            bool ready = false;
            for (int i = 0; i < 30; ++i) {
                if (QFileInfo(path).isDir()) { ready = true; break; }
                QThread::msleep(100);
            }
            postToUi([self, path, ready] {
                if (self->completed) return;
                self->finish(ready ? path : QString(), ready ? QString() : QObject::tr("Connected to SMB, but the requested folder could not be opened"));
            });
        });
    }
    void startNative()
    {
        if (completed) return;
        url = cfUrl(plan.mountUrl);
        if (!url) { finish({}, QObject::tr("Invalid SMB address")); return; }
        if (!plan.user.isEmpty()) user = plan.user.toCFString();
        if (!plan.password.isEmpty()) password = plan.password.toCFString();
        options = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(options, kNAUIOptionKey, kNAUIOptionAllowUI);
        const auto self = shared_from_this();
        const int status = NetFSMountURLAsync(url, nullptr, user, password, options, nullptr, &request,
                                               dispatch_get_main_queue(), ^(int status, AsyncRequestID, CFArrayRef points) {
            MacSmbResult result; result.status = status;
            if (points && CFGetTypeID(points) == CFArrayGetTypeID())
                for (CFIndex i = 0; i < CFArrayGetCount(points); ++i) {
                    const auto value = CFArrayGetValueAtIndex(points, i);
                    if (value && CFGetTypeID(value) == CFStringGetTypeID()) result.paths.append(QString::fromCFString(static_cast<CFStringRef>(value)));
                }
            self->result(std::move(result));
        });
        if (status != 0) { request = nullptr; result({status, {}}); }
    }
};
} // namespace

std::optional<MacSmbPlan> MacSmb::plan(const QString& uri, QString* error)
{
    const QUrl url(uri.trimmed(), QUrl::StrictMode);
    auto reject = [&](const QString& message) -> std::optional<MacSmbPlan> { if (error) *error = message; return std::nullopt; };
    if (!url.isValid() || url.scheme().compare(QLatin1String("smb"), Qt::CaseInsensitive) != 0 || url.host().isEmpty())
        return reject(QObject::tr("Enter an SMB address such as smb://server/share"));
    if (url.hasQuery() || url.hasFragment()) return reject(QObject::tr("SMB addresses cannot contain a query or fragment"));
    bool valid;
    const auto parts = components(url, &valid);
    if (!valid) return reject(QObject::tr("The SMB folder path contains an invalid component"));
    MacSmbPlan result;
    result.mountUrl = url.adjusted(QUrl::RemovePassword);
    result.mountUrl.setPath(parts.isEmpty() ? QString() : QLatin1Char('/') + parts.first(), QUrl::DecodedMode);
    result.relativePath = parts.mid(1).join(QLatin1Char('/'));
    result.user = url.userName(); result.password = url.password();
    return result;
}
bool MacSmb::sameShare(const QUrl& requested, const QUrl& mounted)
{
    if (requested.scheme().compare(QLatin1String("smb"), Qt::CaseInsensitive) != 0 ||
        mounted.scheme().compare(QLatin1String("smb"), Qt::CaseInsensitive) != 0) return false;
    if (requested.host().compare(mounted.host(), Qt::CaseInsensitive) != 0 || requested.port(445) != mounted.port(445)) return false;
    if (!requested.userName().isEmpty() && requested.userName().compare(mounted.userName(), Qt::CaseInsensitive) != 0) return false;
    const QString share = firstShare(requested);
    return !share.isEmpty() && share.compare(firstShare(mounted), Qt::CaseInsensitive) == 0;
}
QString MacSmb::targetPath(const QString& root, const QString& relative)
{
    if (!root.startsWith(QLatin1Char('/')) || root == QLatin1String("/") || root.contains(QChar(0))) return {};
    const QString base = QDir::cleanPath(root);
    const QString target = QDir::cleanPath(base + QLatin1Char('/') + relative);
    return target == base || target.startsWith(base + QLatin1Char('/')) ? target : QString();
}
QList<MacSmbVolume> MacSmb::mountedVolumes()
{
    QList<MacSmbVolume> result;
    for (const auto& mount : listMounts()) {
        if (mount.fsType != QLatin1String("smbfs")) continue;
        const auto native = mount.path.toCFString();
        const auto local = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, native, kCFURLPOSIXPathStyle, true);
        CFRelease(native);
        if (!local) continue;
        const auto remote = NetFSCopyURLForRemountingVolume(local);
        CFRelease(local);
        if (!remote) continue;
        const auto string = CFURLGetString(remote);
        if (string) result.append({QUrl(QString::fromCFString(string)).adjusted(QUrl::RemovePassword), mount.path});
        CFRelease(remote);
    }
    return result;
}
void MacSmb::mount(const QString& uri, QWidget* parent, Network::Done done)
{
    QString error;
    auto requestPlan = plan(uri, &error);
    if (!requestPlan) { done({}, error); return; }
    auto call = std::make_shared<MountCall>();
    call->plan = std::move(*requestPlan); call->done = std::move(done);
    call->timer = new QTimer(&Network::instance());
    call->timer->setSingleShot(true);
    QObject::connect(call->timer, &QTimer::timeout, &Network::instance(), [call] { call->finish({}, failure(ETIMEDOUT), true); });
    if (parent) call->ownerGone = QObject::connect(parent, &QObject::destroyed, &Network::instance(), [call] { call->finish({}, failure(ECANCELED), true); });
    const auto hooks = g_hooks;
    call->volumes = hooks.volumes ? hooks.volumes : mountedVolumes;
    call->timer->start(hooks.timeoutMs);
    ioPool()->start([call, hooks] {
        const auto volumes = call->volumes();
        QString existing;
        for (const auto& volume : volumes)
            if (sameShare(call->plan.mountUrl, volume.url)) { existing = volume.path; break; }
        postToUi([call, hooks, existing] {
            if (call->completed) return;
            if (!existing.isEmpty()) { call->result({0, {existing}}); return; }
            if (hooks.mount) hooks.mount(call->plan, [call](MacSmbResult result) {
                postToUi([call, result = std::move(result)]() mutable { call->result(std::move(result)); });
            });
            else call->startNative();
        });
    });
}
QString MacSmb::mountRoot(const QString& path)
{
    QString root;
    const QString clean = QDir::cleanPath(path);
    for (const auto& mount : listMounts())
        if (mount.fsType == QLatin1String("smbfs") && !mount.path.startsWith(QLatin1String("/Volumes/.timemachine/")) &&
            (clean == mount.path || clean.startsWith(mount.path + QLatin1Char('/'))) && mount.path.size() > root.size()) root = mount.path;
    return root;
}
void MacSmb::unmount(const QString& path, std::function<void(const QString&)> done)
{
    const QString root = mountRoot(path);
    if (root.isEmpty()) { done(QObject::tr("This is not a mounted SMB share")); return; }
    auto* process = new QProcess(&Network::instance());
    auto completed = std::make_shared<bool>(false);
    auto finish = [process, completed, done = std::move(done)](const QString& error) {
        if (std::exchange(*completed, true)) return;
        process->deleteLater(); done(error);
    };
    QObject::connect(process, &QProcess::errorOccurred, process, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finish(QObject::tr("Could not start the macOS disk utility"));
    });
    QObject::connect(process, &QProcess::finished, process, [process, finish](int status, QProcess::ExitStatus exit) {
        const QString error = QString::fromLocal8Bit(process->readAllStandardError()).trimmed();
        finish(status == 0 && exit == QProcess::NormalExit ? QString() : error.isEmpty() ? QObject::tr("macOS could not disconnect the share; it may be in use") : error);
    });
    QTimer::singleShot(30000, process, [process, completed, finish] {
        if (!*completed) { process->kill(); finish(QObject::tr("Disconnecting the SMB share timed out")); }
    });
    process->start(QStringLiteral("/usr/sbin/diskutil"), {QStringLiteral("unmount"), root}); // deliberately never force-unmount
}
void MacSmb::setHooksForTests(MacSmbTestHooks hooks) { g_hooks = std::move(hooks); }
} // namespace tandem
