#include "core/DesktopApps.h"
#include "core/DirLister.h"
#include "core/FileOps.h"
#include "core/Sorting.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>
#include <unistd.h>

using namespace tandem;

namespace {

void writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return "<missing>";
    return f.readAll();
}

bool runJob(Job& job)
{
    QSignalSpy spy(&job, &Job::finished);
    job.start();
    return spy.wait(20000);
}

FileEntry entry(const QString& name, bool dir = false)
{
    FileEntry e;
    e.name = name;
    e.isDir = dir;
    e.key = makeSortKey(name);
    return e;
}

}  // namespace

class TestCore : public QObject {
    Q_OBJECT

private slots:
    void naturalSort()
    {
        std::vector<FileEntry> v = {entry("file10"), entry("File2"), entry("b-dir", true), entry("file1"),
                                    entry("A-dir", true)};
        FileEntry parent = entry("..", true);
        parent.isParent = true;
        v.push_back(parent);
        sortEntries(v, SortSpec{});
        QStringList names;
        for (auto& e : v)
            names << e.name;
        QCOMPARE(names, QStringList({"..", "A-dir", "b-dir", "file1", "File2", "file10"}));
    }

    void extension()
    {
        QCOMPARE(extensionOf("archive.tar.gz").toString(), QString("gz"));
        QCOMPARE(extensionOf(".bashrc").toString(), QString());
        QCOMPARE(extensionOf("noext").toString(), QString());
        QCOMPARE(extensionOf("trailing.").toString(), QString());
    }

    void listing()
    {
        QTemporaryDir tmp;
        writeFile(tmp.filePath("a.txt"), "hello");
        writeFile(tmp.filePath(".hidden"), "x");
        QVERIFY(QDir(tmp.path()).mkdir("sub"));
        QVERIFY(::symlink("a.txt", QFile::encodeName(tmp.filePath("link")).constData()) == 0);
        QVERIFY(::symlink("missing", QFile::encodeName(tmp.filePath("broken")).constData()) == 0);

        ListResult r = listDirectory(tmp.path(), ListOptions{});
        QCOMPARE(r.error, 0);
        QCOMPARE(int(r.entries.size()), 4);
        QCOMPARE(r.entries[0].name, QString("sub"));
        QVERIFY(r.entries[0].isDir);
        for (auto& e : r.entries) {
            if (e.name == "a.txt")
                QCOMPARE(e.size, int64_t(5));
            if (e.name == "link")
                QVERIFY(e.isLink && !e.brokenLink);
            if (e.name == "broken")
                QVERIFY(e.isLink && e.brokenLink);
        }
        ListOptions hidden;
        hidden.showHidden = true;
        QCOMPARE(int(listDirectory(tmp.path(), hidden).entries.size()), 5);
        QCOMPARE(listDirectory(tmp.filePath("nope"), {}).error, ENOENT);
    }

    void copyTree()
    {
        QTemporaryDir tmp;
        const QString src = tmp.filePath("src");
        QVERIFY(QDir().mkpath(src + "/sub/deeper"));
        QByteArray big(3 * 1024 * 1024 + 17, '\0');
        for (int i = 0; i < big.size(); ++i)
            big[i] = char(i * 31 + 7);
        writeFile(src + "/a.txt", "alpha");
        writeFile(src + "/sub/big.bin", big);
        writeFile(src + "/sub/deeper/c.txt", "gamma");
        QVERIFY(::chmod(QFile::encodeName(src + "/a.txt").constData(), 0640) == 0);
        QVERIFY(::symlink("a.txt", QFile::encodeName(src + "/lnk").constData()) == 0);
        QVERIFY(QDir().mkdir(tmp.filePath("dst")));

        CopyJob job(CopyJob::Mode::Copy, {src}, tmp.filePath("dst"));
        QVERIFY(runJob(job));
        QVERIFY2(job.errors().isEmpty(), qPrintable(job.errors().join('\n')));
        const QString d = tmp.filePath("dst/src");
        QCOMPARE(readFile(d + "/a.txt"), QByteArray("alpha"));
        QCOMPARE(readFile(d + "/sub/big.bin"), big);
        QCOMPARE(readFile(d + "/sub/deeper/c.txt"), QByteArray("gamma"));
        QCOMPARE(QFile::symLinkTarget(d + "/lnk"), d + "/a.txt");
        struct stat st;
        QVERIFY(::stat(QFile::encodeName(d + "/a.txt").constData(), &st) == 0);
        QCOMPARE(st.st_mode & 0777, mode_t(0640));
        QCOMPARE(job.snapshot().bytesDone, job.snapshot().bytesTotal);
        QVERIFY(QFile::exists(src + "/a.txt"));  // source untouched
    }

    void copySingleAsNewName()
    {
        QTemporaryDir tmp;
        writeFile(tmp.filePath("one.txt"), "1");
        CopyJob job(CopyJob::Mode::Copy, {tmp.filePath("one.txt")}, tmp.filePath("two.txt"));
        QVERIFY(runJob(job));
        QCOMPARE(readFile(tmp.filePath("two.txt")), QByteArray("1"));
    }

    void moveSameFs()
    {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.filePath("a/inner")));
        writeFile(tmp.filePath("a/inner/f.txt"), "f");
        QVERIFY(QDir().mkdir(tmp.filePath("b")));
        CopyJob job(CopyJob::Mode::Move, {tmp.filePath("a")}, tmp.filePath("b"));
        QVERIFY(runJob(job));
        QVERIFY(!QFile::exists(tmp.filePath("a")));
        QCOMPARE(readFile(tmp.filePath("b/a/inner/f.txt")), QByteArray("f"));
    }

    void moveMergesIntoExistingFolder()
    {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.filePath("src/d")));
        QVERIFY(QDir().mkpath(tmp.filePath("dst/d")));
        writeFile(tmp.filePath("src/d/new.txt"), "new");
        writeFile(tmp.filePath("src/d/same.txt"), "from-src");
        writeFile(tmp.filePath("dst/d/same.txt"), "from-dst");
        writeFile(tmp.filePath("dst/d/keep.txt"), "keep");

        int prompts = 0;
        CopyJob job(CopyJob::Mode::Move, {tmp.filePath("src/d")}, tmp.filePath("dst"));
        job.setSyncPromptHandler([&](const PromptRequest& r) {
            ++prompts;
            return r.kind == PromptRequest::Kind::Conflict ? Answer::Overwrite : Answer::Cancel;
        });
        QVERIFY(runJob(job));
        QCOMPARE(prompts, 1);
        QCOMPARE(readFile(tmp.filePath("dst/d/same.txt")), QByteArray("from-src"));
        QCOMPARE(readFile(tmp.filePath("dst/d/new.txt")), QByteArray("new"));
        QCOMPARE(readFile(tmp.filePath("dst/d/keep.txt")), QByteArray("keep"));
        QVERIFY(!QFile::exists(tmp.filePath("src/d")));
    }

    void conflictSkipAllAndRename()
    {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkdir(tmp.filePath("s")));
        QVERIFY(QDir().mkdir(tmp.filePath("t")));
        for (const char* n : {"1.txt", "2.txt", "3.txt"}) {
            writeFile(tmp.filePath(QString("s/") + n), "new");
            writeFile(tmp.filePath(QString("t/") + n), "old");
        }
        const QStringList srcs = {tmp.filePath("s/1.txt"), tmp.filePath("s/2.txt"), tmp.filePath("s/3.txt")};

        int prompts = 0;
        CopyJob skip(CopyJob::Mode::Copy, srcs, tmp.filePath("t"));
        skip.setSyncPromptHandler([&](const PromptRequest&) {
            ++prompts;
            return Answer::SkipAll;
        });
        QVERIFY(runJob(skip));
        QCOMPARE(prompts, 1);
        QCOMPARE(readFile(tmp.filePath("t/2.txt")), QByteArray("old"));

        CopyJob rename(CopyJob::Mode::Copy, {srcs[0]}, tmp.filePath("t"));
        rename.setSyncPromptHandler([](const PromptRequest&) { return Answer::Rename; });
        QVERIFY(runJob(rename));
        QCOMPARE(readFile(tmp.filePath("t/1 (2).txt")), QByteArray("new"));
        QCOMPARE(readFile(tmp.filePath("t/1.txt")), QByteArray("old"));
    }

    void copyIntoItselfRejected()
    {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.filePath("a/b")));
        CopyJob job(CopyJob::Mode::Copy, {tmp.filePath("a")}, tmp.filePath("a/b"));
        QVERIFY(runJob(job));
        QCOMPARE(job.errors().size(), 1);
        QVERIFY(!QFile::exists(tmp.filePath("a/b/a")));
    }

    void cancelMidCopyRemovesPartialFile()
    {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkdir(tmp.filePath("t")));
        writeFile(tmp.filePath("f.bin"), QByteArray(1024, 'x'));
        CopyJob job(CopyJob::Mode::Copy, {tmp.filePath("f.bin")}, tmp.filePath("t"));
        job.setPaused(true);
        QSignalSpy spy(&job, &Job::finished);
        job.start();
        QTest::qWait(50);
        job.cancel();
        QVERIFY(spy.wait(5000));
        QCOMPARE(job.snapshot().state, Job::State::Cancelled);
    }

    void deletePermanent()
    {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.filePath("x/y/z")));
        writeFile(tmp.filePath("x/y/z/f"), "f");
        writeFile(tmp.filePath("x/g"), "g");
        DeleteJob job({tmp.filePath("x")}, false);
        QVERIFY(runJob(job));
        QVERIFY(job.errors().isEmpty());
        QVERIFY(!QFile::exists(tmp.filePath("x")));
        QCOMPARE(job.snapshot().filesDone, int64_t(5));
    }

    void desktopExecExpansion()
    {
        using L = QList<QStringList>;
        QCOMPARE(DesktopApps::commandLines("kate -b %U", {"/a b", "/c"}),
                 L({{"kate", "-b", "file:///a%20b", "file:///c"}}));
        QCOMPARE(DesktopApps::commandLines("mpv --player-operation-mode=pseudo-gui -- %f", {"/x", "/y"}),
                 L({{"mpv", "--player-operation-mode=pseudo-gui", "--", "/x"},
                    {"mpv", "--player-operation-mode=pseudo-gui", "--", "/y"}}));
        QCOMPARE(DesktopApps::commandLines("gimp %i %F", {"/p.png"}), L({{"gimp", "/p.png"}}));
        QCOMPARE(DesktopApps::commandLines("\"my app\" --x=100%%", {"/f"}), L({{"my app", "--x=100%", "/f"}}));
    }

    void helpers()
    {
        QTemporaryDir tmp;
        QCOMPARE(makeDirectories(tmp.filePath("n1/n2/n3")), 0);
        QVERIFY(QFileInfo(tmp.filePath("n1/n2/n3")).isDir());
        QCOMPARE(makeDirectories(tmp.filePath("n1")), EEXIST);
        writeFile(tmp.filePath("n1/f.txt"), "12345");
        writeFile(tmp.filePath("n1/n2/g.txt"), "678");
        QCOMPARE(directorySize(tmp.filePath("n1"), nullptr), int64_t(8));
        QCOMPARE(renameNoReplace(tmp.filePath("n1/f.txt"), tmp.filePath("n1/n2/g.txt")), EEXIST);
        QCOMPARE(renameNoReplace(tmp.filePath("n1/f.txt"), tmp.filePath("n1/h.txt")), 0);
        QCOMPARE(QString::fromLocal8Bit(uniqueName(QFile::encodeName(tmp.filePath("n1/h.txt")))),
                 tmp.filePath("n1/h (2).txt"));
    }
};

QTEST_GUILESS_MAIN(TestCore)
#include "test_core.moc"
