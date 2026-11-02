#include "core/Archive.h"
#include "core/DesktopApps.h"
#include "core/DirLister.h"
#include "core/FileOps.h"
#include "core/Undo.h"
#include "core/FolderHistory.h"
#include "core/Fuzzy.h"
#include "core/IoScheduler.h"
#include "core/MultiRename.h"
#include "core/Sorting.h"

#include <QDateTime>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
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
