#include "core/FileOps.h"
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <sys/xattr.h>

using namespace tandem;

class TestMacOS : public QObject {
    Q_OBJECT
private slots:
    void crossVolumeMoveAndDiskFull()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString image = tmp.filePath("scratch.sparseimage");
        const QString mount = tmp.filePath("mount");
        QVERIFY(QDir().mkdir(mount));
        QProcess disk;
        disk.start("/usr/bin/hdiutil", {"create", "-size", "128m", "-fs", "APFS", "-type", "SPARSE", "-volname", "TandemTest", image});
        QVERIFY(disk.waitForFinished(30000));
        QVERIFY2(disk.exitCode() == 0, disk.readAllStandardError().constData());
        disk.start("/usr/bin/hdiutil", {"attach", "-nobrowse", "-noautoopen", "-mountpoint", mount, image});
        QVERIFY(disk.waitForFinished(30000));
        QVERIFY2(disk.exitCode() == 0, disk.readAllStandardError().constData());
        auto detach = qScopeGuard([&] {
            QProcess::execute("/usr/bin/hdiutil", {"detach", mount});
        });
        const QString source = tmp.filePath("source");
        {
            QFile file(source);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("move me"), qint64(7));
        }
        const QByteArray attr("Finder tag test");
        QCOMPARE(::setxattr(QFile::encodeName(source).constData(), "com.tandem.test", attr.constData(), attr.size(), 0, 0), 0);
        CopyJob move(CopyJob::Mode::Move, {source}, mount);
        QSignalSpy moved(&move, &Job::finished);
        move.start();
        QVERIFY(moved.wait(20000));
        QVERIFY2(move.errors().isEmpty(), qPrintable(move.errors().join(';')));
        QVERIFY(!QFile::exists(source));
        QFile copied(mount + "/source");
        QVERIFY(copied.open(QIODevice::ReadOnly));
        QCOMPARE(copied.readAll(), QByteArray("move me"));
        copied.close();
        char buf[128];
        const ssize_t n = ::getxattr(QFile::encodeName(mount + "/source").constData(), "com.tandem.test", buf, sizeof buf, 0, 0);
        QVERIFY(n >= 0);
        QCOMPARE(QByteArray(buf, n), attr);

        // An actual full filesystem: failed replacement must keep the old file
        // and a cross-volume move must retain its source.
        const QString large = tmp.filePath("large");
        {
            QFile file(large);
            QVERIFY(file.open(QIODevice::WriteOnly));
            const QByteArray block(1024 * 1024, 'x');
            for (int i = 0; i < 160; ++i)
                QCOMPARE(file.write(block), qint64(block.size()));
        }
        bool writeError = false;
        CopyJob full(CopyJob::Mode::Move, {large}, mount + "/source");
        full.setSyncPromptHandler([&](const PromptRequest& request) {
            if (request.kind == PromptRequest::Kind::Conflict)
                return Answer::Overwrite;
            writeError = true;
            return Answer::Skip;
        });
        QSignalSpy finished(&full, &Job::finished);
        full.start();
        QVERIFY(finished.wait(20000));
        QVERIFY(writeError);
        QVERIFY(QFile::exists(large));
        QVERIFY(copied.open(QIODevice::ReadOnly));
        QCOMPARE(copied.readAll(), QByteArray("move me"));
        copied.close();
        QVERIFY(QDir(mount).entryList({".tandem-copy-*"}, QDir::Files | QDir::Hidden).isEmpty());
    }
};
QTEST_GUILESS_MAIN(TestMacOS)
#include "test_macos.moc"
