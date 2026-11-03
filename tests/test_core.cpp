#include "core/Archive.h"
#include "core/Backend.h"
#include "core/DesktopApps.h"
#include "core/DirLister.h"
#include "core/FileOps.h"
#include "core/Search.h"
#include "core/FolderCompare.h"
#include "core/Remote.h"
#include "TestBackend.h"
#include "SshServer.h"
#include "core/GitStatus.h"
#include "core/Undo.h"
#include "core/FolderHistory.h"
#include "core/Fuzzy.h"
#include "core/IoScheduler.h"
#include "core/MultiRename.h"
#include "core/Sorting.h"

#include <QDateTime>
#include <QFile>
#include <QElapsedTimer>
#include <QProcess>
#include <QStandardPaths>
#include <QSignalSpy>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>
#include <thread>
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
        QCOMPARE(extensionOf("archive.tar.gz").toString(), QString("tar.gz"));
        QCOMPARE(extensionOf("v1.2.gz").toString(), QString("gz"));
        QCOMPARE(extensionOf(".tar.gz").toString(), QString("gz"));
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

    void terminalCommands()
    {
        const QStringList cmd = {"nvim", "/tmp/f"};
        QCOMPARE(DesktopApps::terminalCommand("kitty", cmd), QStringList({"kitty", "nvim", "/tmp/f"}));
        QCOMPARE(DesktopApps::terminalCommand("/usr/bin/foot", cmd), QStringList({"/usr/bin/foot", "nvim", "/tmp/f"}));
        QCOMPARE(DesktopApps::terminalCommand("alacritty", cmd), QStringList({"alacritty", "-e", "nvim", "/tmp/f"}));
        QCOMPARE(DesktopApps::terminalCommand("wezterm", cmd), QStringList({"wezterm", "start", "--", "nvim", "/tmp/f"}));
        QCOMPARE(DesktopApps::terminalCommand("kitty --class x", cmd),
                 QStringList({"kitty", "--class", "x", "nvim", "/tmp/f"}));
    }

    void defaultAppFromMimeappsList()
    {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.filePath("data/applications")));
        QVERIFY(QDir().mkpath(tmp.filePath("config")));
        writeFile(tmp.filePath("data/applications/aaa.desktop"),
                  "[Desktop Entry]\nType=Application\nName=Aaa\nExec=aaa %f\nMimeType=text/plain;\n");
        writeFile(tmp.filePath("data/applications/zzz.desktop"),
                  "[Desktop Entry]\nType=Application\nName=Zzz\nExec=zzz %F\nMimeType=text/plain;\n");
        writeFile(tmp.filePath("data/applications/gone.desktop"),
                  "[Desktop Entry]\nType=Application\nName=Gone\nExec=gone\nMimeType=text/plain;\n");
        writeFile(tmp.filePath("config/mimeapps.list"),
                  "[Default Applications]\ntext/plain=zzz.desktop;\n[Removed Associations]\ntext/plain=gone.desktop;\n");
        qputenv("XDG_DATA_HOME", QFile::encodeName(tmp.filePath("data")));
        qputenv("XDG_DATA_DIRS", QFile::encodeName(tmp.filePath("none")));
        qputenv("XDG_CONFIG_HOME", QFile::encodeName(tmp.filePath("config")));
        qputenv("XDG_CONFIG_DIRS", QFile::encodeName(tmp.filePath("none")));

        DesktopApps& apps = DesktopApps::instance();
        apps.loadAsync();
        QTRY_VERIFY(apps.isLoaded());
        QCOMPARE(apps.defaultFor({"text/plain"}).id, QString("zzz.desktop"));
        const QList<DesktopApp> all = apps.appsFor({"text/plain"});
        QCOMPARE(all.size(), 2);  // "gone" was removed by the user
        QCOMPARE(all[1].id, QString("aaa.desktop"));
        QVERIFY(apps.defaultFor({"image/x-nothing"}).id.isEmpty());

        // Setting a default rewrites only that line; other sections stay.
        QVERIFY(apps.setDefault("text/plain", "aaa.desktop"));
        QVERIFY(apps.setDefault("image/png", "zzz.desktop"));
        QCOMPARE(apps.defaultFor({"text/plain"}).id, QString("aaa.desktop"));
        QCOMPARE(readFile(tmp.filePath("config/mimeapps.list")),
                 QByteArray("[Default Applications]\ntext/plain=aaa.desktop;\nimage/png=zzz.desktop;\n"
                            "[Removed Associations]\ntext/plain=gone.desktop;\n"));
    }

    void archiveRoundTrip_data()
    {
        QTest::addColumn<QString>("ext");
        QTest::newRow("zip") << ".zip";
        QTest::newRow("7z") << ".7z";
        QTest::newRow("tar.gz") << ".tar.gz";
        QTest::newRow("tar.zst") << ".tar.zst";
    }

    void archiveRoundTrip()
    {
        QFETCH(QString, ext);
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.filePath("src/docs/deep")));
        writeFile(tmp.filePath("src/readme.txt"), "hello");
        writeFile(tmp.filePath("src/docs/a.txt"), "aaa");
        writeFile(tmp.filePath("src/docs/deep/b.bin"), QByteArray(200000, 'b'));
        QVERIFY(::symlink("readme.txt", QFile::encodeName(tmp.filePath("src/link")).constData()) == 0);

        const QString arc = tmp.filePath("out" + ext);
        PackJob pack({tmp.filePath("src")}, arc);
        QVERIFY(runJob(pack));
        QVERIFY2(pack.errors().isEmpty(), qPrintable(pack.errors().join('\n')));
        QVERIFY(QFileInfo(arc).size() > 0);
        QVERIFY(!QFile::exists(arc + ".part"));

        // Browse: root, then a folder inside.
        ListResult root = listLocation(arc, {});
        QCOMPARE(root.error, 0);
        QVERIFY(root.inArchive);
        QCOMPARE(root.archivePath, arc);
        QCOMPARE(int(root.entries.size()), 1);
        QCOMPARE(root.entries[0].name, QString("src"));
        QCOMPARE(root.entries[0].size, int64_t(5 + 3 + 200000));  // folder total (links count 0)
        ListResult docs = listLocation(arc + "/src/docs", {});
        QCOMPARE(docs.error, 0);
        QCOMPARE(docs.innerPath, QString("src/docs"));
        QStringList names;
        for (auto& e : docs.entries)
            names << e.name;
        QCOMPARE(names, QStringList({"deep", "a.txt"}));
        QCOMPARE(listLocation(arc + "/src/nope", {}).error, ENOENT);

        // Extract a subfolder somewhere else.
        QVERIFY(QDir().mkdir(tmp.filePath("x")));
        ExtractJob ex(arc, {"src/docs"}, tmp.filePath("x"));
        QVERIFY(runJob(ex));
        QVERIFY2(ex.errors().isEmpty(), qPrintable(ex.errors().join('\n')));
        QCOMPARE(readFile(tmp.filePath("x/docs/a.txt")), QByteArray("aaa"));
        QCOMPARE(readFile(tmp.filePath("x/docs/deep/b.bin")), QByteArray(200000, 'b'));
        QVERIFY(!QFile::exists(tmp.filePath("x/readme.txt")));

        // Single file into the cache (viewer / open).
        QString err;
        const QString cached = extractToCache(arc, "src/readme.txt", &err);
        QVERIFY2(!cached.isEmpty(), qPrintable(err));
        QCOMPARE(readFile(cached), QByteArray("hello"));
    }

    void archiveUpdate_data()
    {
        QTest::addColumn<QString>("ext");
        QTest::newRow("zip") << ".zip";
        QTest::newRow("7z") << ".7z";
        QTest::newRow("tar") << ".tar";
        QTest::newRow("tar.gz") << ".tar.gz";
        QTest::newRow("tar.zst") << ".tar.zst";
    }

    // Changing an existing archive: remove, rename, new folder, add, replace.
    void archiveUpdate()
    {
        QFETCH(QString, ext);
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.filePath("src/docs/deep")));
        writeFile(tmp.filePath("src/readme.txt"), "hello");
        writeFile(tmp.filePath("src/docs/a.txt"), "aaa");
        writeFile(tmp.filePath("src/docs/deep/b.bin"), QByteArray(200000, 'b'));
        QVERIFY(::symlink("readme.txt", QFile::encodeName(tmp.filePath("src/link")).constData()) == 0);
        const QString arc = tmp.filePath("out" + ext);
        PackJob pack({tmp.filePath("src")}, arc);
        QVERIFY(runJob(pack));
        QVERIFY(listLocation(arc + "/src", {}).error == 0);  // index cached now

        QVERIFY(QDir().mkpath(tmp.filePath("more/inner")));
        writeFile(tmp.filePath("more/inner/m.txt"), "more");
        writeFile(tmp.filePath("extra.txt"), "extra");
        ArchiveChanges c;
        c.remove << "src/docs/deep";
        c.rename << qMakePair(QString("src/readme.txt"), QString("src/README.txt"));  // same size: cache must not lie
        c.mkdir << "src/new/sub";
        c.add << qMakePair(tmp.filePath("extra.txt"), QString("src/docs/extra.txt"))
              << qMakePair(tmp.filePath("more"), QString("src/more"));
        c.moveSources = true;
        ArchiveUpdateJob job(arc, c);
        QVERIFY(runJob(job));
        QVERIFY2(job.errors().isEmpty(), qPrintable(job.errors().join('\n')));
        QCOMPARE(job.snapshot().state, Job::State::Done);
        QVERIFY(!QFile::exists(tmp.filePath("extra.txt")));  // moved in
        QVERIFY(!QFile::exists(tmp.filePath("more")));
        QCOMPARE(QDir(tmp.path()).entryList({".out*"}, QDir::Hidden | QDir::Files), QStringList());  // no temp left

        auto names = [&](const QString& inner) {
            QStringList n;
            const ListResult l = listLocation(arc + "/" + inner, {.showHidden = true});
            for (const FileEntry& e : l.entries)
                n << e.name + (e.isLink ? "@" : "");
            n.sort();
            return n;
        };
        QCOMPARE(names("src"), QStringList({"README.txt", "docs", "link@", "more", "new"}));
        QCOMPARE(names("src/docs"), QStringList({"a.txt", "extra.txt"}));
        QCOMPARE(names("src/new"), QStringList({"sub"}));
        QString err;
        QCOMPARE(readFile(extractToCache(arc, "src/README.txt", &err)), QByteArray("hello"));
        QCOMPARE(readFile(extractToCache(arc, "src/more/inner/m.txt", &err)), QByteArray("more"));
        QCOMPARE(readFile(extractToCache(arc, "src/docs/extra.txt", &err)), QByteArray("extra"));

        // Adding a name that exists: asked; "rename" keeps both.
        writeFile(tmp.filePath("a.txt"), "second");
        int prompts = 0;
        ArchiveChanges c2;
        c2.add << qMakePair(tmp.filePath("a.txt"), QString("src/docs/a.txt"));
        ArchiveUpdateJob renameJob(arc, c2);
        renameJob.setSyncPromptHandler([&](const PromptRequest& r) {
            ++prompts;
            return r.kind == PromptRequest::Kind::Conflict ? Answer::Rename : Answer::Cancel;
        });
        QVERIFY(runJob(renameJob));
        QCOMPARE(prompts, 1);
        QCOMPARE(names("src/docs"), QStringList({"a (2).txt", "a.txt", "extra.txt"}));
        QCOMPARE(readFile(extractToCache(arc, "src/docs/a (2).txt", &err)), QByteArray("second"));
        QVERIFY(QFile::exists(tmp.filePath("a.txt")));  // a copy, not a move

        // Saving an edited member replaces it without asking.
        writeFile(tmp.filePath("a.txt"), "edited!");
        ArchiveChanges c3;
        c3.add << qMakePair(tmp.filePath("a.txt"), QString("src/docs/a.txt"));
        c3.replaceExisting = true;
        ArchiveUpdateJob replaceJob(arc, c3);
        replaceJob.setSyncPromptHandler([&](const PromptRequest&) { ++prompts; return Answer::Cancel; });
        QVERIFY(runJob(replaceJob));
        QCOMPARE(prompts, 1);
        QCOMPARE(names("src/docs"), QStringList({"a (2).txt", "a.txt", "extra.txt"}));
        QCOMPARE(readFile(extractToCache(arc, "src/docs/a.txt", &err)), QByteArray("edited!"));
    }

    void archiveUpdateRefuses()
    {
        QTemporaryDir tmp;
        writeFile(tmp.filePath("one.txt"), "x");
        QVERIFY(QProcess::execute("gzip", {"-k", tmp.filePath("one.txt")}) == 0);
        QString why;
        QVERIFY(!canUpdateArchive(tmp.filePath("one.txt.gz"), &why));
        QVERIFY(!canUpdateArchive(tmp.filePath("x.rar"), &why));
        QVERIFY(canUpdateArchive(tmp.filePath("x.tar.xz")));
        ArchiveChanges c;
        c.mkdir << "d";
        ArchiveUpdateJob job(tmp.filePath("one.txt.gz"), c);
        job.setSyncPromptHandler([](const PromptRequest&) { return Answer::Cancel; });
        QVERIFY(runJob(job));
        QCOMPARE(job.snapshot().state, Job::State::Failed);
    }

    void extractConflictsAndSmartFolder()
    {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.filePath("one")));
        writeFile(tmp.filePath("one/f.txt"), "new");
        writeFile(tmp.filePath("loose.txt"), "loose");
        const QString single = tmp.filePath("single.zip");
        const QString multi = tmp.filePath("multi.tar.xz");
        PackJob p1({tmp.filePath("one")}, single);
        PackJob p2({tmp.filePath("one"), tmp.filePath("loose.txt")}, multi);
        QVERIFY(runJob(p1));
        QVERIFY(runJob(p2));

        QVERIFY(QDir().mkdir(tmp.filePath("dest")));
        ExtractJob s1(single, {""}, tmp.filePath("dest"), true);  // one top folder: no extra level
        QVERIFY(runJob(s1));
        QCOMPARE(readFile(tmp.filePath("dest/one/f.txt")), QByteArray("new"));
        ExtractJob s2(multi, {""}, tmp.filePath("dest"), true);  // several: folder named after it
        QVERIFY(runJob(s2));
        QCOMPARE(readFile(tmp.filePath("dest/multi/loose.txt")), QByteArray("loose"));

        writeFile(tmp.filePath("dest/one/f.txt"), "edited");
        int prompts = 0;
        ExtractJob again(single, {"one/f.txt"}, tmp.filePath("dest/one"));
        again.setSyncPromptHandler([&](const PromptRequest& r) {
            ++prompts;
            return r.kind == PromptRequest::Kind::Conflict ? Answer::Skip : Answer::Cancel;
        });
        QVERIFY(runJob(again));
        QCOMPARE(prompts, 1);
        QCOMPARE(readFile(tmp.filePath("dest/one/f.txt")), QByteArray("edited"));
    }

    void archiveWithoutFolderEntries()
    {
        const QString bsdtar = QStandardPaths::findExecutable("bsdtar");
        if (bsdtar.isEmpty())
            QSKIP("bsdtar not installed");
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.filePath("a/b")));
        writeFile(tmp.filePath("a/b/c.txt"), "c");
        // Only the file is stored; "a" and "a/b" exist implicitly.
        QCOMPARE(QProcess::execute(bsdtar, {"-C", tmp.path(), "-cf", tmp.filePath("t.tar"), "a/b/c.txt"}), 0);
        ListResult r = listLocation(tmp.filePath("t.tar") + "/a", {});
        QCOMPARE(r.error, 0);
        QCOMPARE(int(r.entries.size()), 1);
        QVERIFY(r.entries[0].isDir);
        QCOMPARE(r.entries[0].size, int64_t(1));
    }

    void archivePathSafety()
    {
        QCOMPARE(normalizeArchivePath("./a//b/"), QString("a/b"));
        QCOMPARE(normalizeArchivePath("/etc/passwd"), QString("etc/passwd"));
        QCOMPARE(normalizeArchivePath("a/../../x"), QString());
        QVERIFY(isArchiveName("x.tar.gz"));
        QVERIFY(isArchiveName("X.ZIP"));
        QVERIFY(!isArchiveName("doc.odt"));
        QVERIFY(!isArchiveName(".zip"));
        QCOMPARE(archiveStem("photos.tar.gz"), QString("photos"));
    }

    void renameMasks()
    {
        const QList<RenameItem> items = {{"IMG_0042.JPG", false, 1700000000}, {"notes.tar.gz", false, 0},
                                         {"Folder.v2", true, 0}};
        auto names = [&](const RenameOptions& o) {
            const RenamePlan p = buildRenamePlan(items, "Trip", o, {});
            QStringList out;
            for (const auto& r : p.rows)
                out << r.newName;
            return p.error.isEmpty() ? out : QStringList{p.error};
        };
        RenameOptions o;
        QCOMPARE(names(o), QStringList({"IMG_0042.JPG", "notes.tar.gz", "Folder.v2"}));  // unchanged
        o.nameMask = "[P]_[C]";
        o.counterDigits = 3;
        QCOMPARE(names(o), QStringList({"Trip_001.JPG", "Trip_002.tar.gz", "Trip_003"}));  // folders have no ext
        o = {};
        o.nameMask = "[N5-]_[N1,3]";
        o.extMask = "[E1]";
        QCOMPARE(names(o).first(), QString("0042_IMG.J"));
        o = {};
        o.nameMask = "[N-4-][[][C10+5:2][]]";
        QCOMPARE(names(o), QStringList({"0042[10].JPG", "otes[15].tar.gz", "r.v2[20]"}));
        o = {};
        o.nameMask = "[YMD]-[N]";
        o.extMask = "[E]";
        QVERIFY(names(o).first().startsWith(QDateTime::fromSecsSinceEpoch(1700000000).toString("yyyyMMdd") + "-IMG"));
        o = {};
        o.search = "img_(\\d+)";
        o.replace = "photo-$1";
        o.regex = true;
        QCOMPARE(names(o).first(), QString("photo-0042.JPG"));
        o.regex = false;
        o.search = ".";
        o.replace = "\\1";  // plain text: no regex magic
        QCOMPARE(names(o).at(1), QString("notes\\1tar\\1gz"));
        o = {};
        o.caseMode = RenameOptions::Case::Lower;
        QCOMPARE(names(o).first(), QString("img_0042.jpg"));
        o.caseMode = RenameOptions::Case::Title;
        QCOMPARE(names({.nameMask = "my holiday's best", .caseMode = RenameOptions::Case::Title}).first(),
                 QString("My Holiday's Best.JPG"));
        o = {};
        o.nameMask = "[Q]";
        QCOMPARE(names(o), QStringList({"Unknown placeholder [Q]"}));
        o.nameMask = "[N";
        QCOMPARE(names(o), QStringList({"Missing ] in the mask"}));
    }

    void renameConflictsAndSwap()
    {
        QTemporaryDir tmp;
        writeFile(tmp.filePath("a"), "A");
        writeFile(tmp.filePath("b"), "B");
        writeFile(tmp.filePath("c"), "C");
        writeFile(tmp.filePath("other"), "O");
        const QSet<QString> existing = {"a", "b", "c", "other"};

        // a -> other collides with a file that stays.
        RenameOptions o;
        o.search = "a";
        o.replace = "other";
        RenamePlan p = buildRenamePlan({{"a", false, 0}}, "", o, existing);
        QCOMPARE(p.problems, 1);
        QVERIFY(!p.canRun());
        // Two files to the same name.
        o.nameMask = "x";
        o.search.clear();
        p = buildRenamePlan({{"a", false, 0}, {"b", false, 0}}, "", o, existing);
        QCOMPARE(p.problems, 2);

        // Rotate a->b, b->c, c->a: needs temporary names, must not lose data.
        RenamePlan rot;
        rot.rows = {{"a", "b", RenameRow::Status::Ok, {}}, {"b", "c", RenameRow::Status::Ok, {}},
                    {"c", "a", RenameRow::Status::Ok, {}}};
        QStringList errors;
        const auto done = executeRenamePlan(tmp.path(), rot, &errors);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
        QCOMPARE(done.size(), 3);
        QCOMPARE(readFile(tmp.filePath("b")), QByteArray("A"));
        QCOMPARE(readFile(tmp.filePath("c")), QByteArray("B"));
        QCOMPARE(readFile(tmp.filePath("a")), QByteArray("C"));
        QCOMPARE(QDir(tmp.path()).entryList(QDir::Files | QDir::Hidden).size(), 4);  // no temp leftovers

        // Undo = the reverse plan.
        RenamePlan undo;
        for (const auto& [now, before] : done)
            undo.rows.append({now, before, RenameRow::Status::Ok, {}});
        executeRenamePlan(tmp.path(), undo, &errors);
        QVERIFY(errors.isEmpty());
        QCOMPARE(readFile(tmp.filePath("a")), QByteArray("A"));
        QCOMPARE(readFile(tmp.filePath("c")), QByteArray("C"));
    }

    void queuePerDrive()
    {
        QTemporaryDir tmp;
        writeFile(tmp.filePath("f1"), QByteArray(4096, '1'));
        writeFile(tmp.filePath("f2"), QByteArray(4096, '2'));
        writeFile(tmp.filePath("f3"), QByteArray(4096, '3'));
        QVERIFY(QDir().mkdir(tmp.filePath("out")));
        auto state = [](Job& j) { return j.snapshot().state; };

        CopyJob a(CopyJob::Mode::Copy, {tmp.filePath("f1")}, tmp.filePath("out"));
        a.setPaused(true);  // gets the drive, then sits paused
        QSignalSpy aDone(&a, &Job::finished);
        a.start();
        QTRY_VERIFY(state(a) != Job::State::Pending && state(a) != Job::State::Queued);

        CopyJob b(CopyJob::Mode::Copy, {tmp.filePath("f2")}, tmp.filePath("out"));
        QSignalSpy bDone(&b, &Job::finished);
        b.start();
        CopyJob c(CopyJob::Mode::Copy, {tmp.filePath("f3")}, tmp.filePath("out"));
        c.setPaused(true);
        QSignalSpy cDone(&c, &Job::finished);
        c.start();
        QTRY_COMPARE(state(b), Job::State::Queued);
        QTRY_COMPARE(state(c), Job::State::Queued);

        // A move inside the same filesystem is a rename: never queued.
        writeFile(tmp.filePath("m"), "m");
        CopyJob move(CopyJob::Mode::Move, {tmp.filePath("m")}, tmp.filePath("out"));
        QVERIFY(runJob(move));
        QVERIFY(QFile::exists(tmp.filePath("out/m")));

        // c jumps ahead of b; when a is done, c takes the drive (and pauses), b keeps waiting.
        c.runNext();
        a.setPaused(false);
        QVERIFY(aDone.wait(5000));
        QTRY_VERIFY(state(c) != Job::State::Queued);
        QTest::qWait(300);
        QCOMPARE(state(b), Job::State::Queued);

        // "Start now" lets b run alongside.
        b.startNow();
        QVERIFY(bDone.wait(5000));
        QCOMPARE(readFile(tmp.filePath("out/f2")), QByteArray(4096, '2'));
        c.setPaused(false);
        QVERIFY(cDone.wait(5000));
    }

    void queueDifferentDrivesRunInParallel()
    {
        QTemporaryDir tmp;  // usually tmpfs
        QTemporaryDir local(QDir::currentPath() + "/queue-test-XXXXXX");
        if (IoScheduler::deviceOf(tmp.path()) == IoScheduler::deviceOf(local.path()))
            QSKIP("temp and working directory are on the same filesystem");
        writeFile(tmp.filePath("a"), "a");
        writeFile(local.filePath("b"), "b");
        QVERIFY(QDir().mkdir(tmp.filePath("out")));
        QVERIFY(QDir().mkdir(local.filePath("out")));
        CopyJob first(CopyJob::Mode::Copy, {tmp.filePath("a")}, tmp.filePath("out"));
        first.setPaused(true);
        QSignalSpy firstDone(&first, &Job::finished);
        first.start();
        QTRY_VERIFY(first.snapshot().state != Job::State::Queued && first.snapshot().state != Job::State::Pending);
        CopyJob second(CopyJob::Mode::Copy, {local.filePath("b")}, local.filePath("out"));
        QVERIFY(runJob(second));  // not blocked by the paused job on the other drive
        first.setPaused(false);
        QVERIFY(firstDone.wait(5000));
    }

    void fuzzy()
    {
        QVERIFY(fuzzyScore(u"copy", u"Copy…") >= 0);
        QVERIFY(fuzzyScore(u"cfp", u"Copy full paths as text") >= 0);
        QCOMPARE(fuzzyScore(u"xyz", u"Copy"), -1);
        QVERIFY(fuzzyScore(u"copy", u"Copy…") > fuzzyScore(u"copy", u"Copy full paths as text"));
        QVERIFY(fuzzyScore(u"mr", u"Multi-rename") > fuzzyScore(u"mr", u"Mirror"));
        QVERIFY(fuzzyScore(u"code tan", u"~/Code/tandem-fm") >= 0);
        QCOMPARE(fuzzyScore(u"tan code", u"~/Code/tandem-fm"), -1);  // terms keep their order
        QList<int> pos;
        fuzzyScore(u"tf", u"tandem-fm", &pos);
        QCOMPARE(pos, QList<int>({0, 7}));  // word starts preferred over the first 'f'… there is only one
    }

    void folderHistory()
    {
        QTemporaryDir tmp;
        FolderHistory& h = FolderHistory::instance();
        h.setStoragePath(tmp.filePath("folders.tsv"));
        h.visit("/a");
        h.visit("/a");
        h.visit("/b");
        QList<FolderHistory::Item> items = h.items();
        QCOMPARE(items.size(), 2);
        QCOMPARE(items[0].path, QString("/a"));
        h.setPinned("/b", true);
        QCOMPARE(h.items().first().path, QString("/b"));
        QVERIFY(h.items().first().pinned);
        h.saveNow();
        h.setStoragePath(tmp.filePath("folders.tsv"));  // clears memory
        QVERIFY(h.items().isEmpty());
        h.loadAsync();
        QTRY_COMPARE(h.items().size(), 2);
        QVERIFY(h.isPinned("/b"));
        h.forget("/a");
        QCOMPARE(h.items().size(), 1);
        QVERIFY(FolderHistory::frecency(10, 0, 30) > FolderHistory::frecency(10, 0, 30 * 86400));

        // Bookmarks: unique keys, chains, persistence.
        h.setBookmark("/proj", "cp");
        h.setBookmark("/docs", "cd");
        QCOMPARE(h.pathForKey("cp"), QString("/proj"));
        h.setBookmark("/other", "cp");  // a used key moves
        QCOMPARE(h.pathForKey("cp"), QString("/other"));
        QCOMPARE(h.bookmarkKey("/proj"), QString());
        QVERIFY(h.isPinned("/proj"));   // still pinned, just without a key
        h.saveNow();
        h.setStoragePath(tmp.filePath("folders.tsv"));
        h.loadAsync();
        QTRY_COMPARE(h.pathForKey("cd"), QString("/docs"));
        QCOMPARE(h.bookmarks().size(), 2);
        h.setPinned("/docs", false);    // unpinning drops the key
        QCOMPARE(h.pathForKey("cd"), QString());

        // Old (v1) files still load; a missing file gets the defaults.
        writeFile(tmp.filePath("old.tsv"), "3\t100\t1\t/pinned old\n1\t50\t0\t/seen\n");
        h.setStoragePath(tmp.filePath("old.tsv"));
        h.loadAsync();
        QTRY_COMPARE(h.items().size(), 2);
        QVERIFY(h.isPinned("/pinned old"));
        h.setStoragePath(tmp.filePath("missing.tsv"));
        h.loadAsync();
        QTRY_COMPARE(h.pathForKey("h"), QDir::homePath());
        QCOMPARE(h.pathForKey("/"), QString("/"));
    }

    void undoRecords()
    {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.filePath("src/d")));
        QVERIFY(QDir().mkpath(tmp.filePath("dst/d")));  // merge target: not undoable
        writeFile(tmp.filePath("src/f"), "f");
        writeFile(tmp.filePath("src/d/x"), "x");

        CopyJob copy(CopyJob::Mode::Copy, {tmp.filePath("src/f"), tmp.filePath("src/d")}, tmp.filePath("dst"));
        QVERIFY(runJob(copy));
        QCOMPARE(copy.undoRecords(), (QList<QPair<QString, QString>>{{tmp.filePath("dst/f"), QString()}}));

        QVERIFY(QDir().mkdir(tmp.filePath("moved")));
        CopyJob move(CopyJob::Mode::Move, {tmp.filePath("src/f")}, tmp.filePath("moved"));
        QVERIFY(runJob(move));
        QCOMPARE(move.undoRecords(),
                 (QList<QPair<QString, QString>>{{tmp.filePath("moved/f"), tmp.filePath("src/f")}}));

        // Trash and restore, inside a private XDG_DATA_HOME on the same filesystem.
        const QByteArray oldData = qgetenv("XDG_DATA_HOME");
        qputenv("XDG_DATA_HOME", QFile::encodeName(tmp.filePath("data")));
        QVERIFY(QDir().mkpath(tmp.filePath("data")));
        writeFile(tmp.filePath("trashme"), "t");
        DeleteJob trash({tmp.filePath("trashme")}, true);
        QVERIFY(runJob(trash));
        const auto recs = trash.undoRecords();
        QCOMPARE(recs.size(), 1);
        QVERIFY(!QFile::exists(tmp.filePath("trashme")));
        QVERIFY(QFile::exists(recs.first().first));
        QVERIFY(restoreFromTrash(recs).isEmpty());
        QCOMPARE(readFile(tmp.filePath("trashme")), QByteArray("t"));
        QVERIFY(!QFile::exists(recs.first().first));
        qputenv("XDG_DATA_HOME", oldData);

        // New folders go away only while empty.
        QVERIFY(QDir().mkpath(tmp.filePath("new/a/b")));
        QVERIFY(QDir().mkpath(tmp.filePath("full")));
        writeFile(tmp.filePath("full/keep"), "k");
        const QStringList errors = removeIfStillEmpty({tmp.filePath("new"), tmp.filePath("full")});
        QCOMPARE(errors.size(), 1);
        QVERIFY(!QFile::exists(tmp.filePath("new")));
        QVERIFY(QFile::exists(tmp.filePath("full/keep")));
    }

    void gitParse()
    {
        const char raw[] = "## main...origin/main [ahead 1]\0"
                           " M src/a.cpp\0"
                           "A  src/new.h\0"
                           "R  src/moved.cpp\0src/old.cpp\0"
                           "UU conflict.txt\0"
                           "?? notes.md\0"
                           "!! build/\0"
                           "!! src/gen/x.o\0"
                           "?? docs/draft/\0";
        const QByteArray out(raw, sizeof(raw) - 1);
        GitInfo root = parseGitStatus(out, "/r", "/r");
        QCOMPARE(root.branch, QString("main"));
        QCOMPARE(root.status.value("src"), 'M');      // strongest inside wins
        QCOMPARE(root.status.value("conflict.txt"), 'U');
        QCOMPARE(root.status.value("notes.md"), '?');
        QCOMPARE(root.status.value("build"), '!');
        QCOMPARE(root.status.value("docs"), '?');
        GitInfo src = parseGitStatus(out, "/r", "/r/src");
        QCOMPARE(src.status.value("a.cpp"), 'M');
        QCOMPARE(src.status.value("new.h"), 'A');
        QCOMPARE(src.status.value("moved.cpp"), 'M');
        QVERIFY(!src.status.contains("old.cpp"));
        QVERIFY(!src.status.contains("gen"));       // an ignored file doesn't mark its folder
        QCOMPARE(parseGitStatus("## No commits yet on dev\0", "/r", "/r").branch, QString("dev"));
    }

    void gitRealRepo()
    {
        const QString git = QStandardPaths::findExecutable("git");
        if (git.isEmpty())
            QSKIP("git not installed");
        QTemporaryDir tmp;
        auto run = [&](const QStringList& args) {
            QProcess p;
            p.setWorkingDirectory(tmp.path());
            p.start(git, QStringList{"-c", "user.name=t", "-c", "user.email=t@t", "-c", "init.defaultBranch=trunk"} + args);
            p.waitForFinished();
            return p.exitCode();
        };
        QCOMPARE(run({"init", "-q"}), 0);
        QVERIFY(QDir().mkpath(tmp.filePath("sub")));
        writeFile(tmp.filePath("tracked.txt"), "v1");
        writeFile(tmp.filePath("sub/inner.txt"), "v1");
        writeFile(tmp.filePath(".gitignore"), "*.log\n");
        QCOMPARE(run({"add", "."}), 0);
        QCOMPARE(run({"commit", "-q", "-m", "init"}), 0);
        writeFile(tmp.filePath("tracked.txt"), "v2");
        writeFile(tmp.filePath("sub/inner.txt"), "v2");
        writeFile(tmp.filePath("fresh.txt"), "new");
        writeFile(tmp.filePath("debug.log"), "x");
        writeFile(tmp.filePath("staged.txt"), "s");
        QCOMPARE(run({"add", "staged.txt"}), 0);

        const GitInfo info = gitStatusFor(tmp.path());
        QVERIFY(info.inRepo);
        QCOMPARE(info.branch, QString("trunk"));
        QCOMPARE(info.status.value("tracked.txt"), 'M');
        QCOMPARE(info.status.value("sub"), 'M');
        QCOMPARE(info.status.value("fresh.txt"), '?');
        QCOMPARE(info.status.value("debug.log"), '!');
        QCOMPARE(info.status.value("staged.txt"), 'A');
        QVERIFY(!info.status.contains(".gitignore"));
        const GitInfo sub = gitStatusFor(tmp.filePath("sub"));
        QCOMPARE(sub.status.value("inner.txt"), 'M');
        QVERIFY(!gitStatusFor(QDir::rootPath()).inRepo);
    }

    void search()
    {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.filePath("src/deep")));
        QVERIFY(QDir().mkpath(tmp.filePath(".hidden")));
        QVERIFY(QDir().mkpath(tmp.filePath("build")));
        writeFile(tmp.filePath("src/Fuzzy.cpp"), "int needle = 1;");
        writeFile(tmp.filePath("src/deep/fuzzy.h"), "// nothing");
        writeFile(tmp.filePath(".hidden/fuzzy.txt"), "needle");
        writeFile(tmp.filePath("build/fuzzy.o"), "needle");
        writeFile(tmp.filePath(".gitignore"), "build/\n");
        QVERIFY(QDir().mkdir(tmp.filePath(".git")));  // fd/rg honour .gitignore inside repos
        auto run = [&](SearchSpec s, int limit = 1000) {
            s.root = tmp.path();
            QStringList all;
            const QString err = runSearch(s, nullptr, limit, [&](const QStringList& b) { all << b; });
            all.sort();
            return err.isEmpty() ? all : QStringList{"ERROR " + err};
        };
        QCOMPARE(run({{}, "fuzzy", false, false}), QStringList({"src/Fuzzy.cpp", "src/deep/fuzzy.h"}));
        QCOMPARE(run({{}, "*.h", false, false}), QStringList({"src/deep/fuzzy.h"}));
        QCOMPARE(run({{}, "needle", true, false}), QStringList({"src/Fuzzy.cpp"}));
        QCOMPARE(run({{}, "needle", true, true}),
                 QStringList({".hidden/fuzzy.txt", "build/fuzzy.o", "src/Fuzzy.cpp"}));
        QCOMPARE(run({{}, "fuzzy", false, true}, 2).size(), 2);  // limit
        QCOMPARE(run({{}, "", false, false}), QStringList());

        // Without fd/rg/grep: names still work (built-in walk), contents explain why not.
        const QByteArray oldPath = qgetenv("PATH");
        qputenv("PATH", QFile::encodeName(tmp.filePath("no-tools")));
        QStringList names = run({{}, "fuzzy", false, false});
        // Hidden skipped; the fallback doesn't read .gitignore, so build/ shows up.
        QCOMPARE(names, QStringList({"build/fuzzy.o", "src/Fuzzy.cpp", "src/deep/fuzzy.h"}));
        QCOMPARE(run({{}, "fuzzy", false, true}).size(), 4);                // incl. hidden & build
        QVERIFY(run({{}, "needle", true, false}).first().startsWith("ERROR"));
        qputenv("PATH", oldPath);
    }

    void folderCompare()
    {
        QTemporaryDir tmp;
        const QString L = tmp.filePath("L"), R = tmp.filePath("R");
        auto at = [](const QString& p, qint64 secs) {
            QFile f(p);
            QVERIFY(f.open(QIODevice::ReadWrite));
            QVERIFY(f.setFileTime(QDateTime::fromSecsSinceEpoch(secs), QFileDevice::FileModificationTime));
        };
        QVERIFY(QDir().mkpath(L + "/sub/deep"));
        QVERIFY(QDir().mkpath(R + "/sub"));
        QVERIFY(QDir().mkpath(L + "/onlyleft"));
        writeFile(L + "/same.txt", "same");          writeFile(R + "/same.txt", "same");
        writeFile(L + "/newer.txt", "new!");         writeFile(R + "/newer.txt", "old");
        writeFile(L + "/older.txt", "old");          writeFile(R + "/older.txt", "new!");
        writeFile(L + "/sametime.txt", "aaaa");      writeFile(R + "/sametime.txt", "bbbb");
        writeFile(L + "/sub/deep/x.txt", "x");
        writeFile(L + "/onlyleft/y.txt", "y");
        writeFile(R + "/sub/r.txt", "r");
        writeFile(L + "/clash", "file");             QVERIFY(QDir().mkpath(R + "/clash"));
        at(L + "/same.txt", 1000);     at(R + "/same.txt", 1001);  // within tolerance
        at(L + "/newer.txt", 5000);    at(R + "/newer.txt", 2000);
        at(L + "/older.txt", 2000);    at(R + "/older.txt", 5000);
        at(L + "/sametime.txt", 3000); at(R + "/sametime.txt", 3000);

        auto diffs = [](const CompareResult& r) {
            QMap<QString, Diff> m;
            for (const CompareItem& i : r.items)
                m[i.rel] = i.diff;
            return m;
        };
        CompareResult res = compareFolders(L, R, {});
        QVERIFY(res.error.isEmpty());
        auto d = diffs(res);
        QCOMPARE(d.value("same.txt"), Diff::Equal);
        QCOMPARE(d.value("newer.txt"), Diff::LeftNewer);
        QCOMPARE(d.value("older.txt"), Diff::RightNewer);
        QCOMPARE(d.value("sametime.txt"), Diff::Equal);  // same size and time: trusted
        QCOMPARE(d.value("sub/deep"), Diff::LeftOnly);    // one-sided folder = one item
        QCOMPARE(d.value("onlyleft"), Diff::LeftOnly);
        QCOMPARE(d.value("sub/r.txt"), Diff::RightOnly);
        QCOMPARE(d.value("clash"), Diff::TypeMismatch);
        QVERIFY(!d.contains("sub"));  // folders on both sides only show their contents

        CompareOptions byContent;
        byContent.contents = true;
        QCOMPARE(diffs(compareFolders(L, R, byContent)).value("sametime.txt"), Diff::Differs);
        CompareOptions flat;
        flat.recursive = false;
        QVERIFY(!diffs(compareFolders(L, R, flat)).contains("sub/r.txt"));
        QVERIFY(!compareFolders(L, tmp.filePath("missing"), {}).error.isEmpty());

        // Two-way sync: newer wins, one-sided items are copied over.
        std::vector<SyncAction> actions;
        for (const CompareItem& i : res.items)
            actions.push_back(defaultAction(i.diff, SyncMode::TwoWay));
        const SyncPlan plan = buildSyncPlan(L, R, res.items, actions);
        QVERIFY(plan.deletions.isEmpty());
        CopyJob job(plan.copies);
        job.setOverwriteExisting();
        QVERIFY(runJob(job));
        QCOMPARE(readFile(R + "/newer.txt"), QByteArray("new!"));
        QCOMPARE(readFile(L + "/older.txt"), QByteArray("new!"));
        QCOMPARE(readFile(R + "/sub/deep/x.txt"), QByteArray("x"));
        QCOMPARE(readFile(L + "/sub/r.txt"), QByteArray("r"));
        QCOMPARE(readFile(R + "/onlyleft/y.txt"), QByteArray("y"));
        QCOMPARE(readFile(L + "/clash"), QByteArray("file"));  // mismatches are left alone
        QCOMPARE(readFile(L + "/sametime.txt"), QByteArray("aaaa"));
        auto after = diffs(compareFolders(L, R, {}));
        for (auto it = after.cbegin(); it != after.cend(); ++it)
            QVERIFY2(it.value() == Diff::Equal || it.key() == "clash", qPrintable(it.key()));

        // Mirror: the right side ends up like the left, extra files deleted.
        writeFile(R + "/extra.txt", "extra");
        CompareResult res2 = compareFolders(L, R, {});
        actions.clear();
        for (const CompareItem& i : res2.items)
            actions.push_back(defaultAction(i.diff, SyncMode::MirrorToRight));
        const SyncPlan mirror = buildSyncPlan(L, R, res2.items, actions);
        QCOMPARE(mirror.deletions, QStringList({R + "/extra.txt"}));
        QVERIFY(mirror.copies.isEmpty());
    }

    void remotePaths()
    {
        QCOMPARE(describeGvfsName("sftp:host=example.com,user=bob").label, QString("sftp://bob@example.com"));
        QCOMPARE(describeGvfsName("sftp:host=example.com,port=2222").label, QString("sftp://example.com:2222"));
        QCOMPARE(describeGvfsName("smb-share:server=nas,share=media,user=me").label, QString("smb://me@nas/media"));
        QCOMPARE(describeGvfsName("dav:host=cloud.org,ssl=true,prefix=%2Fremote.php%2Fdav").label,
                 QString("davs://cloud.org/remote.php/dav"));
        QCOMPARE(describeGvfsName("ftp:host=ftp.gnu.org").scheme, QString("ftp"));
        QCOMPARE(describeGvfsName("localtest:").label, QString("localtest"));

        const QString root = gvfsDirectory() + "/sftp:host=h,user=u";
        QVERIFY(isRemotePath(root + "/srv/x"));
        QVERIFY(!isRemotePath("/home/u"));
        QCOMPARE(remoteRootOf(root + "/srv/x")->root, root);
        QCOMPARE(remoteRootOf(root)->label, QString("sftp://u@h"));
        QVERIFY(!remoteRootOf(gvfsDirectory()));
        // Each GVfs mount gets its own transfer queue, unlike the shared FUSE device.
        const QString other = gvfsDirectory() + "/smb-share:server=nas,share=x";
        QCOMPARE(IoScheduler::deviceOf(root + "/a"), IoScheduler::deviceOf(root + "/b/c"));
        QVERIFY(IoScheduler::deviceOf(root + "/a") != IoScheduler::deviceOf(other + "/a"));

        const QByteArray realUser = qgetenv("USER");
        qputenv("USER", "me");
        QCOMPARE(normalizeServerUri("sftp://h/srv"), QString("sftp://h/srv"));
        QCOMPARE(normalizeServerUri("ssh://u@h"), QString("sftp://u@h"));
        QCOMPARE(normalizeServerUri("https://cloud/dav"), QString("davs://cloud/dav"));
        QCOMPARE(normalizeServerUri("u@h"), QString("sftp://u@h"));
        QCOMPARE(normalizeServerUri("u@h:/etc"), QString("sftp://u@h/etc"));
        QCOMPARE(normalizeServerUri("u@h:code/x"), QString("sftp://u@h/home/u/code/x"));
        QCOMPARE(normalizeServerUri("h:~"), QString("sftp://h/home/me"));
        QCOMPARE(normalizeServerUri("/home/me"), QString());
        QCOMPARE(normalizeServerUri("~/x"), QString());
        QCOMPARE(normalizeServerUri("file:///x"), QString());
        QCOMPARE(normalizeServerUri("plainname"), QString());
        QCOMPARE(normalizeServerUri("my file@2x.png"), QString());
        qputenv("USER", realUser);
    }

    void locationPaths()
    {
        QVERIFY(isRemoteLocation("sftp://bob@host/srv"));
        QVERIFY(!isRemoteLocation("/home/x"));
        QVERIFY(!isRemoteLocation("relative/x"));
        QCOMPARE(parentPath("sftp://bob@host/srv/www"), QString("sftp://bob@host/srv"));
        QCOMPARE(parentPath("sftp://bob@host/srv"), QString("sftp://bob@host/"));
        QCOMPARE(parentPath("sftp://bob@host/"), QString("sftp://bob@host/"));
        QCOMPARE(baseName("sftp://bob@host/srv/www"), QString("www"));
        QCOMPARE(baseName("sftp://bob@host/"), QString("bob@host"));
        QCOMPARE(joinPath(QString("sftp://h/"), QString("x")), QString("sftp://h/x"));
        QCOMPARE(normalizeLocation("SFTP://h"), QString("sftp://h/"));
        QCOMPARE(normalizeLocation("sftp://h//a/./b/../c/"), QString("sftp://h/a/c"));
        QCOMPARE(expandUserPath("sftp://h/a/../b"), QString("sftp://h/b"));
        QCOMPARE(locationPath("sftp://h/a/b"), QString("/a/b"));
        QCOMPARE(locationAuthority("sftp://u@h:22/a"), QString("sftp://u@h:22"));
    }

    void backendJobs()
    {
        QTemporaryDir tmp;
        const QString local = tmp.filePath("local"), server = tmp.filePath("server");
        QVERIFY(QDir().mkpath(local + "/tree/sub"));
        QVERIFY(QDir().mkpath(server + "/existing"));
        writeFile(local + "/tree/a.txt", "alpha");
        writeFile(local + "/tree/sub/b.bin", QByteArray(3 << 20, 'b'));  // more than one buffer
        writeFile(local + "/single.txt", "single");
        writeFile(server + "/remote.txt", "from server");
        writeFile(server + "/existing/clash.txt", "old");
        QVERIFY(QFile::link("a.txt", local + "/tree/link"));
        auto backend = std::make_shared<TestBackend>(server);
        registerBackend("test", backend);
        const QString S = "test://srv";

        // Listing goes through the backend.
        ListOptions opt;
        ListResult l = listLocation(S + "/", opt);
        QVERIFY(l.remote);
        QCOMPARE(l.error, 0);
        QCOMPARE(l.entries.size(), size_t(2));
        QCOMPARE(l.entries[0].name, QString("existing"));  // sorted, folders first
        QCOMPARE(listLocation(S + "/missing", opt).error, ENOENT);
        QCOMPARE(listLocation("nosuch://x/", opt).error, EPROTONOSUPPORT);

        // Local → server, folders recursively, links kept.
        CopyJob up(CopyJob::Mode::Copy, {local + "/tree", local + "/single.txt"}, S + "/up");
        QVERIFY(runJob(up));
        QVERIFY2(up.errors().isEmpty(), qPrintable(up.errors().join('\n')));
        QCOMPARE(readFile(server + "/up/tree/a.txt"), QByteArray("alpha"));
        QCOMPARE(readFile(server + "/up/tree/sub/b.bin").size(), 3 << 20);
        QCOMPARE(readFile(server + "/up/single.txt"), QByteArray("single"));
        QCOMPARE(QFileInfo(server + "/up/tree/link").symLinkTarget(), server + "/up/tree/a.txt");
        QCOMPARE(QFileInfo(server + "/up/tree/a.txt").lastModified().toSecsSinceEpoch(),
                 QFileInfo(local + "/tree/a.txt").lastModified().toSecsSinceEpoch());  // servers keep seconds
        QCOMPARE(up.snapshot().bytesDone, int64_t(5 + 6 + (3 << 20)));

        // Server → local.
        CopyJob down(CopyJob::Mode::Copy, {S + "/remote.txt"}, local);
        QVERIFY(runJob(down));
        QCOMPARE(readFile(local + "/remote.txt"), QByteArray("from server"));
        QCOMPARE(down.undoRecords().size(), 1);  // local results can be undone

        // Conflict: the usual question; "rename" picks a free name on the server.
        writeFile(local + "/clash.txt", "new");
        CopyJob clash(CopyJob::Mode::Copy, {local + "/clash.txt"}, S + "/existing");
        clash.setSyncPromptHandler([](const PromptRequest& r) {
            return r.kind == PromptRequest::Kind::Conflict ? Answer::Rename : Answer::Cancel;
        });
        QVERIFY(runJob(clash));
        QCOMPARE(readFile(server + "/existing/clash.txt"), QByteArray("old"));
        QCOMPARE(readFile(server + "/existing/clash (2).txt"), QByteArray("new"));
        CopyJob over(CopyJob::Mode::Copy, {local + "/clash.txt"}, S + "/existing");
        over.setOverwriteExisting();
        QVERIFY(runJob(over));
        QCOMPARE(readFile(server + "/existing/clash.txt"), QByteArray("new"));

        // Move on the server: a rename, no data copied.
        const int before = backend->calls;
        CopyJob mv(CopyJob::Mode::Move, {S + "/up/tree"}, S + "/moved");
        QVERIFY(runJob(mv));
        QVERIFY(QFile::exists(server + "/moved/sub/b.bin"));
        QVERIFY(!QFile::exists(server + "/up/tree"));
        QVERIFY(backend->calls - before < 10);
        // Move server → local: copied, then removed there.
        CopyJob mvDown(CopyJob::Mode::Move, {S + "/moved"}, local + "/back");
        QVERIFY(runJob(mvDown));
        QCOMPARE(readFile(local + "/back/a.txt"), QByteArray("alpha"));
        QVERIFY(!QFile::exists(server + "/moved"));

        // Cancelling mid-file leaves no half file behind.
        backend->latencyMs = 30;
        writeFile(local + "/big.bin", QByteArray(40 << 20, 'x'));
        CopyJob slow(CopyJob::Mode::Copy, {local + "/big.bin"}, S + "/");
        slow.start();
        QTRY_VERIFY(slow.snapshot().state == Job::State::Running);
        slow.cancel();
        QTRY_VERIFY(slow.isFinished());
        backend->latencyMs = 0;
        QVERIFY(!QFile::exists(server + "/big.bin"));

        // Delete on the server: permanent, recursive.
        DeleteJob del({S + "/up", S + "/existing/clash (2).txt"}, true);
        QVERIFY(runJob(del));
        QVERIFY(!QFile::exists(server + "/up"));
        QVERIFY(!QFile::exists(server + "/existing/clash (2).txt"));

        // Helpers.
        QCOMPARE(makeDirectoriesAt(S + "/a/b/c"), 0);
        QVERIFY(QFileInfo(server + "/a/b/c").isDir());
        QCOMPARE(makeDirectoriesAt(S + "/a/b"), EEXIST);
        QCOMPARE(createEmptyFile(S + "/a/new.txt"), 0);
        QCOMPARE(createEmptyFile(S + "/a/new.txt"), EEXIST);
        QCOMPARE(renameNoReplaceAt(S + "/a/new.txt", S + "/a/renamed.txt"), 0);
        QCOMPARE(renameNoReplaceAt(S + "/a/renamed.txt", local + "/x"), EXDEV);
        QCOMPARE(directorySizeAt(S + "/existing", nullptr), int64_t(3));
        QCOMPARE(downloadFile(S + "/remote.txt", tmp.filePath("cache/r.txt"), nullptr), 0);
        QCOMPARE(readFile(tmp.filePath("cache/r.txt")), QByteArray("from server"));

        // Compare and sync work across local and remote.
        CompareOptions co;
        co.contents = true;
        CompareResult cmp = compareFolders(local + "/tree", S + "/cmp", co);
        QVERIFY(!cmp.error.isEmpty());  // missing on the server
        QVERIFY(QDir().mkpath(server + "/cmp"));
        cmp = compareFolders(local + "/tree", S + "/cmp", co);
        QVERIFY(cmp.error.isEmpty());
        std::vector<SyncAction> actions;
        for (const CompareItem& i : cmp.items)
            actions.push_back(defaultAction(i.diff, SyncMode::MirrorToRight));
        CopyJob sync(buildSyncPlan(local + "/tree", S + "/cmp", cmp.items, actions).copies);
        sync.setOverwriteExisting();
        QVERIFY(runJob(sync));
        for (const CompareItem& i : compareFolders(local + "/tree", S + "/cmp", co).items)
            QVERIFY2(i.diff == Diff::Equal, qPrintable(i.rel));
        QVERIFY(*sameContents(local + "/tree/sub/b.bin", S + "/cmp/sub/b.bin"));
        writeFile(server + "/cmp/sub/b.bin", QByteArray(3 << 20, 'c'));
        QVERIFY(!*sameContents(local + "/tree/sub/b.bin", S + "/cmp/sub/b.bin"));

        registerBackend("test", nullptr);
    }

    void sftpBackend()
    {
        registerBuiltinBackends();
        if (!hasBackend("sftp"))
            QSKIP("built without libssh");
        SshServer server;
        if (!server.start())
            QSKIP("could not start a test sshd");
        QAtomicInt hostKeyQuestions = 0, otherQuestions = 0;
        setAuthHandler([&](const AuthQuestion& q) -> std::optional<QString> {
            if (q.kind == AuthQuestion::Kind::HostKey) {
                ++hostKeyQuestions;
                return QString("yes");
            }
            ++otherQuestions;
            return std::nullopt;
        });
        QTemporaryDir tmp;
        const QString base = tmp.path();  // the "server" is this machine
        QVERIFY(QDir().mkpath(base + "/srv/sub"));
        writeFile(base + "/srv/a.txt", "alpha");
        writeFile(base + "/srv/.hidden", "h");
        QVERIFY(QFile::link("a.txt", base + "/srv/link"));
        QVERIFY(QFile::link(base + "/srv/sub", base + "/srv/dirlink"));
        const QString S = server.location(base + "/srv");

        // Listing: first contact asks about the host key, once.
        ListOptions opt;
        ListResult l = listLocation(S, opt);
        QVERIFY2(l.error == 0, qPrintable(l.errorText + " / " + server.log()));
        QCOMPARE(int(hostKeyQuestions), 1);
        QCOMPARE(int(otherQuestions), 0);  // key login, no password
        QStringList names;
        for (const FileEntry& e : l.entries)
            names << e.name;
        QCOMPARE(names, QStringList({"dirlink", "sub", "a.txt", "link"}));  // folders (and links to them) first
        QVERIFY(l.entries[0].isLink && l.entries[0].isDir);
        QCOMPARE(l.entries[2].size, int64_t(5));
        QCOMPARE(l.entries[2].mtime, QFileInfo(base + "/srv/a.txt").lastModified().toSecsSinceEpoch());
        QVERIFY(l.freeBytes > 0);
        opt.showHidden = true;
        QCOMPARE(listLocation(S, opt).entries.size(), size_t(5));
        QCOMPARE(listLocation(S + "/missing", opt).error, ENOENT);
        QCOMPARE(int(hostKeyQuestions), 1);  // remembered in known_hosts

        Backend* b = backendFor(S);
        QCOMPARE(b->homeLocation(S), server.location(QDir::homePath()));
        FileStat st;
        QCOMPARE(b->stat(S + "/link", st), 0);
        QVERIFY(st.isLink);

        // Transfers both ways, bigger than the in-flight window.
        QByteArray big(9 * 1024 * 1024 + 123, Qt::Uninitialized);
        for (int i = 0; i < big.size(); ++i)
            big[i] = char((i * 7919) >> 3);
        writeFile(base + "/big.bin", big);
        QElapsedTimer t;
        t.start();
        CopyJob up(CopyJob::Mode::Copy, {base + "/big.bin", base + "/srv/sub"}, S + "/up");
        QVERIFY(runJob(up));
        QVERIFY2(up.errors().isEmpty(), qPrintable(up.errors().join('\n')));
        const qint64 upMs = t.restart();
        QCOMPARE(readFile(base + "/srv/up/big.bin"), big);
        QVERIFY(QFileInfo(base + "/srv/up/sub").isDir());
        QCOMPARE(QFileInfo(base + "/srv/up/big.bin").lastModified().toSecsSinceEpoch(),
                 QFileInfo(base + "/big.bin").lastModified().toSecsSinceEpoch());
        CopyJob down(CopyJob::Mode::Copy, {S + "/up/big.bin"}, base + "/down.bin");
        QVERIFY(runJob(down));
        const qint64 downMs = t.elapsed();
        QCOMPARE(readFile(base + "/down.bin"), big);
        QVERIFY(*sameContents(base + "/big.bin", S + "/up/big.bin"));
        qInfo("sftp to localhost: 9 MiB up %lld ms, down %lld ms", upMs, downMs);

        // Big files go over several connections at once ("lanes"), both ways.
        QByteArray huge(40 * 1024 * 1024 + 4321, Qt::Uninitialized);
        for (int i = 0; i < huge.size(); i += 4)
            *reinterpret_cast<quint32*>(huge.data() + qMin(i, int(huge.size()) - 4)) = quint32(i * 2654435761u);
        writeFile(base + "/huge.bin", huge);
        CopyJob hugeUp(CopyJob::Mode::Copy, {base + "/huge.bin"}, S + "/huge-up.bin");
        QVERIFY(runJob(hugeUp));
        QVERIFY2(hugeUp.errors().isEmpty(), qPrintable(hugeUp.errors().join('\n')));
        QVERIFY(readFile(base + "/srv/huge-up.bin") == huge);
        CopyJob hugeDown(CopyJob::Mode::Copy, {S + "/huge-up.bin"}, base + "/huge-down.bin");
        QVERIFY(runJob(hugeDown));
        QVERIFY(readFile(base + "/huge-down.bin") == huge);
        QCOMPARE(hugeDown.snapshot().bytesDone, int64_t(huge.size()));
        // Cancelling a laned download stops cleanly and leaves no partial file.
        CopyJob stopped(CopyJob::Mode::Copy, {S + "/huge-up.bin"}, base + "/stopped.bin");
        stopped.start();
        stopped.cancel();
        QTRY_VERIFY(stopped.isFinished());
        QVERIFY(!QFile::exists(base + "/stopped.bin"));
        QCOMPARE(listLocation(S, opt).error, 0);  // connections still usable

        // Links are copied as links.
        QVERIFY(QDir().mkpath(base + "/fromserver"));
        CopyJob links(CopyJob::Mode::Copy, {S + "/link"}, base + "/fromserver");
        QVERIFY(runJob(links));
        QCOMPARE(QFileInfo(base + "/fromserver/link").symLinkTarget(), base + "/fromserver/a.txt");

        // Rename never replaces; mkdir/exists; delete trees.
        QCOMPARE(renameNoReplaceAt(S + "/a.txt", S + "/up/big.bin"), EEXIST);
        QCOMPARE(renameNoReplaceAt(S + "/a.txt", S + "/b.txt"), 0);
        QVERIFY(QFile::exists(base + "/srv/b.txt"));
        QCOMPARE(makeDirectoriesAt(S + "/x/y/z"), 0);
        QCOMPARE(b->mkdir(S + "/x", 0755), EEXIST);
        QCOMPARE(createEmptyFile(S + "/x/new"), 0);
        QCOMPARE(createEmptyFile(S + "/x/new"), EEXIST);
        DeleteJob del({S + "/up", S + "/x"}, true);
        QVERIFY(runJob(del));
        QVERIFY(!QFile::exists(base + "/srv/up"));
        QVERIFY(!QFile::exists(base + "/srv/x"));

        // Many parallel listings share a few connections.
        std::vector<std::thread> threads;
        std::atomic<int> ok{0};
        for (int i = 0; i < 12; ++i)
            threads.emplace_back([&] {
                if (listLocation(S, ListOptions{}).error == 0)
                    ++ok;
            });
        for (auto& th : threads)
            th.join();
        QCOMPARE(ok.load(), 12);

        // The server goes away and comes back: pooled connections are dead,
        // the next call reconnects instead of failing.
        QVERIFY(server.restart());
        QTest::qWait(200);  // let the pooled connections notice they're closed
        const ListResult again = listLocation(S, opt);
        QVERIFY2(again.error == 0, qPrintable(again.errorText));
        QCOMPARE(b->stat(S + "/b.txt", st), 0);

        // Wrong key: a clear error, no hang.
        qputenv("TANDEM_SSH_IDENTITY", (base + "/nokey").toUtf8());
        const ListResult denied = listLocation(server.location(base) .replace("127.0.0.1", "localhost"), opt);
        QVERIFY(denied.error != 0);
        QVERIFY(!denied.errorText.isEmpty());

        setAuthHandler({});
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
