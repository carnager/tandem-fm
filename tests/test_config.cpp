#include "core/Config.h"
#include "core/FolderHistory.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QProcess>
#include <QUuid>
#include <QSettings>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace tandem;

class TestConfig : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    int m_profile = 0;
    QString file() const { return Config::instance().fileName(); }
    static bool write(const QString& path, const QByteArray& data)
    {
        QSaveFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit();
    }
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(m_dir.isValid());
    }
    void init()
    {
        const QString path = m_dir.filePath(QString::number(++m_profile) + "/settings.ini");
        QDir().mkpath(QFileInfo(path).absolutePath());
        QVERIFY(write(path, QByteArray()));
        QString error;
        QVERIFY2(Config::instance().useFile(path, &error, false), qPrintable(error));
    }
    void firstStartupMigratesNativeSettings()
    {
        const QString name = "tandem-migration-" + QUuid::createUuid().toString(QUuid::Id128);
        QSettings native(QSettings::NativeFormat, QSettings::UserScope, name, name);
        auto cleanup = qScopeGuard([&] { native.clear(); native.sync(); });
        const QString marker = QUuid::createUuid().toString();
        native.setValue("migration/marker", marker); native.sync();
        QCOMPARE(native.status(), QSettings::NoError);
        const QString target = m_dir.filePath("first-startup.ini");
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--migration-probe", target, name, marker});
        QVERIFY(child.waitForFinished(10000));
        QVERIFY2(child.exitCode() == 0, child.readAllStandardError().constData());
        QSettings portable(target, QSettings::IniFormat);
        QCOMPARE(portable.value("migration/marker").toString(), marker);
        QCOMPARE(native.value("migration/marker").toString(), marker); // native backup retained
    }

    void roundTripAndNewLocationMigration()
    {
        {
            Settings settings;
            settings.setValue("view/fontScale", 150);
            settings.setValue("keys/copy", QStringList{"F5", "Ctrl+C"});
            settings.setValue("window/geometry", QByteArray("a\0b", 3));
            settings.setValue("view/theme", "dark");
            settings.setValue("network/recent", QStringList{"sftp://user:password@example.com/"});
        }
        QSettings reader(file(), QSettings::IniFormat);
        QCOMPARE(reader.value("view/fontScale").toInt(), 150);
        QCOMPARE(reader.value("keys/copy").toStringList(), QStringList({"F5", "Ctrl+C"}));
        QCOMPARE(reader.value("window/geometry").toByteArray(), QByteArray("a\0b", 3));
        const QString oldFile = file();
        QVERIFY(write(Config::instance().historyFile(), "#tandem-folders 2\n1\t100\t1\td\t/example\n"));
        const QString target = m_dir.filePath("Dropbox folder/日本語/profile.ini");
        QString error;
        QVERIFY2(Config::instance().useFile(target, &error, false), qPrintable(error));
        QCOMPARE(Settings().value("view/fontScale").toInt(), 150);
        QCOMPARE(Settings().value("view/theme").toString(), QString("dark"));
        QCOMPARE(Settings().value("keys/copy").toStringList(), QStringList({"F5", "Ctrl+C"}));
        QCOMPARE(Settings().value("network/recent").toStringList(), QStringList({"sftp://user@example.com/"}));
        QVERIFY(QFileInfo::exists(oldFile));
        QCOMPARE(Config::instance().historyFile(), QFileInfo(target).dir().filePath("profile.folders.tsv"));
        QFile history(Config::instance().historyFile());
        QVERIFY(history.open(QIODevice::ReadOnly));
        QVERIFY(history.readAll().contains("/example"));
    }
    void existingProfileIsAdoptedWithoutOverwrite()
    {
        Settings().setValue("view/theme", "dark");
        const QString existing = m_dir.filePath("existing.ini");
        const QByteArray contents("[view]\ntheme=light\nfontScale=175\n");
        QVERIFY(write(existing, contents));
        QString error;
        QVERIFY2(Config::instance().useFile(existing, &error, false), qPrintable(error));
        QCOMPARE(Settings().value("view/theme").toString(), QString("light"));
        QCOMPARE(Settings().value("view/fontScale").toInt(), 175);
        QFile original(existing); QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), contents);
    }
    void atomicExternalReplacementAndDeletion()
    {
        Settings().setValue("view/theme", "dark");
        QSignalSpy changed(&Config::instance(), &Config::changed);
        QVERIFY(write(file(), "[view]\ntheme=light\nfontScale=150\n"));
        QTRY_COMPARE_WITH_TIMEOUT(Settings().value("view/theme").toString(), QString("light"), 3000);
        QVERIFY(!changed.isEmpty());
        QVERIFY(write(file(), "[view]\ntheme=dark\n"));
        QTRY_COMPARE_WITH_TIMEOUT(Settings().value("view/theme").toString(), QString("dark"), 3000);
        QVERIFY(!Settings().contains("view/fontScale"));
        QVERIFY(write(file(), "[view]\ntheme=light\n"));
        QTRY_COMPARE_WITH_TIMEOUT(Settings().value("view/theme").toString(), QString("light"), 3000);
    }
    void localWritesMergeUnrelatedExternalKeys()
    {
        QSignalSpy changed(&Config::instance(), &Config::changed);
        Settings openedBeforeRemote;
        openedBeforeRemote.setValue("tools/editor", "my-editor");
        QVERIFY(write(file(), "[view]\nfontScale=175\n"));
        openedBeforeRemote.sync();
        QSettings reader(file(), QSettings::IniFormat); reader.sync();
        QCOMPARE(reader.value("view/fontScale").toInt(), 175);
        QCOMPARE(reader.value("tools/editor").toString(), QString("my-editor"));
        QCOMPARE(Settings().value("view/fontScale").toInt(), 175);
        QTRY_VERIFY(!changed.isEmpty());
    }
    void cloudLockFilesDoNotBlockLocalSaving()
    {
        const QString remoteLock = file() + ".lock";
        QVERIFY(write(remoteLock, "123\nforeign-host\nother-machine\n"));
        Settings().setValue("view/theme", "dark");
        QSettings reader(file(), QSettings::IniFormat); reader.sync();
        QCOMPARE(reader.value("view/theme").toString(), QString("dark"));
        QVERIFY(QFileInfo::exists(remoteLock)); // the app does not touch cloud copies of locks
    }

    void unavailableFileKeepsLastValuesAndRetriesWrites()
    {
        Settings().setValue("view/theme", "dark");
        const QString active = file(), missing = file() + ".offline";
        QVERIFY(QFile::rename(active, missing));
        Config::instance().checkForChanges();
        QCOMPARE(Settings().value("view/theme").toString(), QString("dark"));
        Settings().setValue("view/fontScale", 150);
        QVERIFY(!QFileInfo::exists(active)); // never recreate an absent cloud profile
        QCOMPARE(Settings().value("view/fontScale").toInt(), 150);
        QVERIFY(QFile::rename(missing, active));
        Config::instance().checkForChanges();
        QSettings reader(active, QSettings::IniFormat); reader.sync();
        QCOMPARE(reader.value("view/theme").toString(), QString("dark"));
        QCOMPARE(reader.value("view/fontScale").toInt(), 150);
    }
    void invalidFileCannotReplaceWorkingSettings()
    {
        Settings().setValue("view/theme", "dark");
        QVERIFY(write(file(), "[broken\nvalue=bad\n"));
        Config::instance().checkForChanges();
        QCOMPARE(Settings().value("view/theme").toString(), QString("dark"));
        Settings().setValue("view/fontScale", 175);
        QFile unchanged(file()); QVERIFY(unchanged.open(QIODevice::ReadOnly));
        QCOMPARE(unchanged.readAll(), QByteArray("[broken\nvalue=bad\n"));
        unchanged.close();
        QVERIFY(write(file(), "[view]\ntheme=light\n"));
        Config::instance().checkForChanges();
        QCOMPARE(Settings().value("view/theme").toString(), QString("light"));
        QCOMPARE(Settings().value("view/fontScale").toInt(), 175);
    }
    void groupedRemovalAndStaleScopes()
    {
        {
            Settings settings;
            settings.beginGroup("left"); settings.setValue("sortColumn", 2); settings.setValue("tabs", QStringList{"/tmp"}); settings.endGroup();
            settings.setValue("view/theme", "dark");
        }
        {
            Settings settings; settings.remove("left");
        }
        QVERIFY(!Settings().contains("left/sortColumn"));
        QCOMPARE(Settings().value("view/theme").toString(), QString("dark"));
        Settings stale;
        stale.setValue("view/theme", "stale");
        const QString different = m_dir.filePath("other-profile.ini");
        QVERIFY(write(different, "[view]\ntheme=light\n"));
        QString error;
        QVERIFY2(Config::instance().useFile(different, &error, false), qPrintable(error));
        stale.sync();
        QCOMPARE(Settings().value("view/theme").toString(), QString("light"));
    }
    void locationIsRememberedLocallyAndEnvironmentCanOverrideIt()
    {
        QSettings native(QSettings::NativeFormat, QSettings::UserScope,
                         QCoreApplication::organizationName(), QCoreApplication::applicationName());
        const auto oldPointer = native.value("configuration/file");
        const QByteArray oldEnvironment = qgetenv("TANDEM_CONFIG_FILE");
        auto restore = qScopeGuard([&] {
            if (oldPointer.isValid()) native.setValue("configuration/file", oldPointer); else native.remove("configuration/file");
            if (oldEnvironment.isNull()) qunsetenv("TANDEM_CONFIG_FILE"); else qputenv("TANDEM_CONFIG_FILE", oldEnvironment);
        });
        const QString selected = file();
        QString error;
        QVERIFY2(Config::instance().useFile(selected, &error, true), qPrintable(error));
        native.sync();
        QCOMPARE(native.value("configuration/file").toString(), selected);
        QSettings portable(selected, QSettings::IniFormat);
        QVERIFY(!portable.contains("configuration/file"));
        QVERIFY2(Config::instance().initialize({}, &error), qPrintable(error));
        QCOMPARE(Config::instance().fileName(), selected);
        const QString other = m_dir.filePath("environment.ini");
        QVERIFY(write(other, "[view]\ntheme=light\n"));
        qputenv("TANDEM_CONFIG_FILE", QFile::encodeName(other));
        QVERIFY2(Config::instance().initialize({}, &error), qPrintable(error));
        QCOMPARE(Config::instance().fileName(), other);
        QCOMPARE(Settings().value("view/theme").toString(), QString("light"));
        native.sync();
        QCOMPARE(native.value("configuration/file").toString(), selected); // env override is per launch
    }

    void bookmarkSaveRetriesWhenSharedFolderReturns()
    {
        auto& history = FolderHistory::instance();
        history.configurationChanged();
        QTRY_VERIFY(history.isLoaded());
        QVERIFY(history.saveNow());
        const QString directory = QFileInfo(file()).absolutePath();
        const QString offline = directory + ".offline";
        history.setBookmark("/pending", "p");
        QVERIFY(QDir().rename(directory, offline));
        QTest::qWait(2300); // the first scheduled save cannot reach the folder
        QVERIFY(!QFileInfo::exists(directory));
        QVERIFY(QDir().rename(offline, directory));
        auto saved = [&] {
            QFile input(Config::instance().historyFile());
            return input.open(QIODevice::ReadOnly) && input.readAll().contains("/pending");
        };
        QTRY_VERIFY_WITH_TIMEOUT(saved(), 6000);
    }

    void historyMergesRemoteBookmarksAndLocalVisits()
    {
        auto& history = FolderHistory::instance();
        history.setStoragePath(Config::instance().historyFile());
        const QString path = history.storagePath();
        QVERIFY(write(path, "#tandem-folders 2\n1\t100\t1\ta\t/old\n"));
        history.loadAsync(); QTRY_VERIFY(history.isLoaded());
        QCOMPARE(history.pathForKey("a"), QString("/old"));
        history.visit("/local");
        history.setBookmark("/local", "l");
        // The other device removed /old and added /remote while this one was active.
        QVERIFY(write(path, "#tandem-folders 2\n2\t200\t1\tr\t/remote\n"));
        history.reloadAsync();
        QTRY_COMPARE(history.pathForKey("r"), QString("/remote"));
        QVERIFY(history.pathForKey("a").isEmpty());
        QCOMPARE(history.pathForKey("l"), QString("/local"));
        history.saveNow();
        QFile saved(path); QVERIFY(saved.open(QIODevice::ReadOnly));
        const QByteArray data = saved.readAll();
        QVERIFY(data.contains("/remote") && data.contains("/local"));
        QVERIFY(!data.contains("/old"));
    }
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    const auto args = app.arguments();
    if (args.size() == 5 && args[1] == QLatin1String("--migration-probe")) {
        app.setOrganizationName(args[3]); app.setApplicationName(args[3]);
        QString error;
        if (!Config::instance().initialize(args[2], &error)) { qWarning().noquote() << error; return 1; }
        return Settings().value("migration/marker").toString() == args[4] ? 0 : 2;
    }
    TestConfig test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_config.moc"
