#include "ui/MacSmb.h"
#include "ui/Panel.h"
#include "ui/FileView.h"
#include "ui/DirTab.h"
#include "ui/Theme.h"
#include "core/Config.h"
#include <QApplication>
#include <QFile>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <atomic>
using namespace tandem;

class TestMacSmb : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        applyTheme(*qApp);
    }
    void init()
    {
        MacSmbTestHooks hooks;
        hooks.volumes = [] { return QList<MacSmbVolume>{}; };
        hooks.mount = [](const MacSmbPlan&, auto done) { done({ENOSYS, {}}); };
        hooks.timeoutMs = 5000;
        MacSmb::setHooksForTests(hooks);
    }
    void cleanup() { MacSmb::setHooksForTests({}); }
    void planSeparatesShareSubfolderAndPassword()
    {
        QString error;
        const auto request = MacSmb::plan("smb://DOMAIN;user:secret@nas/Media%20Share/日本語/with%20space", &error);
        QVERIFY2(request.has_value(), qPrintable(error));
        QCOMPARE(request->mountUrl.path(), QString("/Media Share"));
        QCOMPARE(request->relativePath, QString::fromUtf8("日本語/with space"));
        QCOMPARE(request->user, QString("DOMAIN;user"));
        QCOMPARE(request->password, QString("secret"));
        QVERIFY(request->mountUrl.password().isEmpty());
        QVERIFY(!request->mountUrl.toString().contains("secret"));
        const auto host = MacSmb::plan("smb://nas");
        QVERIFY(host); QVERIFY(host->mountUrl.path().isEmpty()); QVERIFY(host->relativePath.isEmpty());
        QVERIFY(MacSmb::plan("smb://[::1]/share"));
    }
    void rejectsInvalidAndEscapingPaths()
    {
        for (const QString& address : {QString("sftp://nas/share"), QString("smb:///share"), QString("smb://nas/share/../other"),
                                       QString("smb://nas/share/%2e%2e/other"), QString("smb://nas/share%2ffolder"),
                                       QString("smb://nas/share/%00"), QString("smb://nas/share?password=secret")}) {
            QString error;
            QVERIFY2(!MacSmb::plan(address, &error), qPrintable(address)); QVERIFY(!error.isEmpty()); QVERIFY(!error.contains("secret"));
        }
        QCOMPARE(MacSmb::targetPath("/Volumes/Share", "folder/with space"), QString("/Volumes/Share/folder/with space"));
        QVERIFY(MacSmb::targetPath("/Volumes/Share", "../Other").isEmpty());
        QVERIFY(MacSmb::targetPath("relative", "folder").isEmpty());
    }
    void matchesMountIdentityInsteadOfVolumeBasename()
    {
        QVERIFY(MacSmb::sameShare(QUrl("smb://NAS/Media"), QUrl("smb://alice@nas/media")));
        QVERIFY(MacSmb::sameShare(QUrl("smb://alice@nas:445/Media"), QUrl("smb://alice@nas/Media")));
        QVERIFY(!MacSmb::sameShare(QUrl("smb://other/Media"), QUrl("smb://nas/Media")));
        QVERIFY(!MacSmb::sameShare(QUrl("smb://bob@nas/Media"), QUrl("smb://alice@nas/Media")));
        QVERIFY(!MacSmb::sameShare(QUrl("smb://nas:1445/Media"), QUrl("smb://nas/Media")));
        QVERIFY(!MacSmb::sameShare(QUrl("smb://nas"), QUrl("smb://nas/Media")));
        QVERIFY(!MacSmb::sameShare(QUrl("smb://nas/Other"), QUrl("smb://nas/Media")));
    }
    void reusesExistingMountAndOpensRequestedSubdirectory()
    {
        QTemporaryDir volume;
        QVERIFY(QDir().mkpath(volume.filePath("with space/sub")));
        std::atomic<int> mounts{0};
        MacSmbTestHooks hooks;
        hooks.volumes = [&] { return QList<MacSmbVolume>{{QUrl("smb://alice@nas/Media"), volume.path()}}; };
        hooks.mount = [&](const MacSmbPlan&, auto) { ++mounts; };
        MacSmb::setHooksForTests(hooks);
        QString path, error; bool done = false;
        Network::instance().mount("smb://nas/Media/with%20space/sub", nullptr, [&](const QString& result, const QString& message) { path = result; error = message; done = true; });
        QTRY_VERIFY(done); QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(path, volume.filePath("with space/sub")); QCOMPARE(mounts.load(), 0);
    }
    void nativeCompletionReportsTheActualMountPath()
    {
        QTemporaryDir volume;
        MacSmbTestHooks hooks;
        hooks.volumes = [] { return QList<MacSmbVolume>{}; };
        hooks.mount = [&](const MacSmbPlan& request, auto done) {
            QCOMPARE(request.mountUrl.toString(), QString("smb://user@nas/share"));
            QCOMPARE(request.password, QString("secret"));
            done({0, {volume.path()}}); done({EACCES, {}}); // only the first completion is used
        };
        MacSmb::setHooksForTests(hooks);
        int completions = 0; QString path, error;
        Network::instance().mount("smb://user:secret@nas/share", nullptr, [&](const QString& result, const QString& message) { ++completions; path = result; error = message; });
        QTRY_COMPARE(completions, 1); QCOMPARE(path, volume.path()); QVERIFY(error.isEmpty());
        QTest::qWait(50); QCOMPARE(completions, 1);
    }
    void alreadyMountedStatusResolvesToTheReturnedOrExistingVolume()
    {
        QTemporaryDir volume;
        std::atomic<int> reads{0};
        MacSmbTestHooks hooks;
        hooks.volumes = [&] {
            return ++reads == 1 ? QList<MacSmbVolume>{} : QList<MacSmbVolume>{{QUrl("smb://nas/share"), volume.path()}};
        };
        hooks.mount = [](const MacSmbPlan&, auto done) { done({EEXIST, {}}); };
        MacSmb::setHooksForTests(hooks);
        bool done = false; QString path, error;
        Network::instance().mount("smb://nas/share", nullptr, [&](const QString& p, const QString& e) { path = p; error = e; done = true; });
        QTRY_VERIFY(done); QCOMPARE(path, volume.path()); QVERIFY(error.isEmpty()); QCOMPARE(reads.load(), 2);
    }
    void missingOrInvalidNativeMountPathsFailSafely()
    {
        for (const QString& root : {QString(), QString("relative"), QString("/")}) {
            MacSmbTestHooks hooks;
            hooks.volumes = [] { return QList<MacSmbVolume>{}; };
            hooks.mount = [root](const MacSmbPlan&, auto done) { done({0, {root}}); };
            MacSmb::setHooksForTests(hooks);
            bool done = false;
            Network::instance().mount("smb://nas/share", nullptr, [&](const QString& path, const QString& error) {
                QVERIFY(path.isEmpty()); QVERIFY(!error.isEmpty()); done = true;
            });
            QTRY_VERIFY(done);
        }
    }
    void cancellationAndFailureDoNotReturnPathsOrCredentials()
    {
        for (int status : {-128, EACCES, -6602}) {
            MacSmbTestHooks hooks;
            hooks.volumes = [] { return QList<MacSmbVolume>{}; };
            hooks.mount = [status](const MacSmbPlan&, auto done) { done({status, {}}); };
            MacSmb::setHooksForTests(hooks);
            bool done = false; QString error, path;
            Network::instance().mount("smb://user:secret@nas/share", nullptr, [&](const QString& p, const QString& e) { path = p; error = e; done = true; });
            QTRY_VERIFY(done); QVERIFY(path.isEmpty()); QVERIFY(!error.isEmpty()); QVERIFY(!error.contains("secret"));
        }
    }
    void timeoutAndOwnerDestructionCompleteOnce()
    {
        MacSmbTestHooks hooks;
        hooks.volumes = [] { return QList<MacSmbVolume>{}; };
        std::function<void(MacSmbResult)> late;
        hooks.mount = [&](const MacSmbPlan&, auto done) { late = done; };
        hooks.timeoutMs = 200;
        MacSmb::setHooksForTests(hooks);
        int completions = 0;
        Network::instance().mount("smb://nas/share", nullptr, [&](const QString& p, const QString& e) { QVERIFY(p.isEmpty()); QVERIFY(!e.isEmpty()); ++completions; });
        QTRY_COMPARE(completions, 1);
        QVERIFY(bool(late)); late({EACCES, {}});
        QTest::qWait(100); QCOMPARE(completions, 1);
        hooks.timeoutMs = 5000;
        MacSmb::setHooksForTests(hooks);
        auto* owner = new QWidget;
        completions = 0;
        Network::instance().mount("smb://nas/share", owner, [&](const QString& p, const QString& e) { QVERIFY(p.isEmpty()); QVERIFY(!e.isEmpty()); ++completions; });
        delete owner;
        QCOMPARE(completions, 1);
    }
    void panelUsesNativeMountAndRemembersPasswordFreeShare()
    {
        QTemporaryDir volume;
        MacSmbTestHooks hooks;
        hooks.volumes = [] { return QList<MacSmbVolume>{}; };
        hooks.mount = [&](const MacSmbPlan&, auto done) { done({0, {volume.path()}}); };
        MacSmb::setHooksForTests(hooks);
        Panel panel; panel.show(); panel.addTab(QDir::tempPath());
        panel.connectTo("smb://user:secret@nas/share");
        QTRY_COMPARE(panel.path(), volume.path());
        QVERIFY(Network::instance().recentServers().contains("smb://user@nas/share"));
        for (const QString& recent : Network::instance().recentServers()) QVERIFY(!recent.contains("secret"));
    }
    void navigatingAwayIgnoresLateMountCompletion()
    {
        QTemporaryDir volume, elsewhere;
        MacSmbTestHooks hooks;
        hooks.volumes = [] { return QList<MacSmbVolume>{}; };
        std::function<void(MacSmbResult)> complete;
        hooks.mount = [&](const MacSmbPlan&, auto done) { complete = done; };
        MacSmb::setHooksForTests(hooks);
        Panel panel; panel.show(); panel.addTab(QDir::tempPath());
        panel.connectTo("smb://nas/share");
        QTRY_VERIFY(bool(complete));
        panel.navigate(elsewhere.path()); QTRY_COMPARE(panel.path(), elsewhere.path());
        complete({0, {volume.path()}});
        QTest::qWait(200); QCOMPARE(panel.path(), elsewhere.path());
    }
    void unsupportedProtocolsAndUnmountTargetsFailSafely()
    {
        QVERIFY(Network::available());
        bool done = false;
        Network::instance().mount("ftp://nas", nullptr, [&](const QString& path, const QString& error) { QVERIFY(path.isEmpty()); QVERIFY(!error.isEmpty()); QVERIFY(!error.contains("GIO")); done = true; });
        QVERIFY(done);
        QVERIFY(!Network::instance().canUnmount(QDir::tempPath()));
        done = false;
        Network::instance().unmount(QDir::tempPath(), nullptr, [&](const QString& error) { QVERIFY(!error.isEmpty()); done = true; });
        QVERIFY(done);
    }
};
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    if (app.arguments().size() == 3 && app.arguments()[1] == QLatin1String("--probe-mounted")) {
        const QString root = app.arguments()[2];
        for (const auto& volume : MacSmb::mountedVolumes()) {
            if (volume.path != root) continue;
            MacSmbTestHooks hooks;
            hooks.volumes = [volume] { return QList<MacSmbVolume>{volume}; };
            hooks.mount = [](const MacSmbPlan&, auto done) { done({ENOSYS, {}}); }; // never mount or authenticate in this read-only probe
            MacSmb::setHooksForTests(hooks);
            Network::instance().mount(volume.url.toString(), nullptr, [&](const QString& path, const QString& error) {
                const bool ok = path == root && error.isEmpty();
                qInfo() << (ok ? "Existing native SMB share reused successfully" : "Existing SMB reuse probe failed"); app.exit(ok ? 0 : 2);
            });
            QTimer::singleShot(10000, &app, [&app] { app.exit(3); });
            return app.exec();
        }
        return 4;
    }
    TestMacSmb test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_macsmb.moc"
