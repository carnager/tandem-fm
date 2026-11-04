#import <AppKit/AppKit.h>

#include "core/Util.h"
#include "core/MacFileIconInfo.h"
#include "ui/FileModel.h"
#include "ui/IconCache.h"
#include "ui/MacFileIcons.h"

#include <QApplication>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>
#include <QTest>

#include <atomic>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

using namespace tandem;

static bool setFixtureIcon(const QString& path, const QImage& image)
{
    @autoreleasepool {
        CGImageRef cg = image.toCGImage();
        NSImage* icon = [[NSImage alloc] initWithCGImage:cg size:NSMakeSize(image.width(), image.height())];
        CGImageRelease(cg);
        NSString* nativePath = [[NSString alloc] initWithCharacters:reinterpret_cast<const unichar*>(path.utf16()) length:path.size()];
        return [[NSWorkspace sharedWorkspace] setIcon:icon forFile:nativePath options:0];
    }
}

class TestMacIcons : public QObject {
    Q_OBJECT
private slots:
    void typeIcons()
    {
        QImage folder, pdf;
        bool done = false;
        ioPool()->start([&] {
            const QImage f = readMacFileIcon({"folder", {}, true, false});
            const QImage p = readMacFileIcon({"document.pdf", {}, false, false});
            postToUi([&, f, p] { folder = f; pdf = p; done = true; });
        });
        QTRY_VERIFY(done);
        QVERIFY(!folder.isNull());
        QVERIFY(!pdf.isNull());
        QVERIFY(folder != pdf);
        QCOMPARE(folder.size(), QSize(64, 64));
        QCOMPARE(pdf.size(), QSize(64, 64));
    }

    void customFolder()
    {
        QTemporaryDir temporary;
        const QString folder = temporary.filePath("Custom folder");
        QVERIFY(QDir().mkdir(folder));
        QImage custom(256, 256, QImage::Format_RGBA8888_Premultiplied);
        custom.fill(Qt::red);
        for (int y = 128; y < 256; ++y)
            for (int x = 0; x < 256; ++x)
                custom.setPixelColor(x, y, Qt::blue);
        QVERIFY(setFixtureIcon(folder, custom));
        FileEntry entry;
        entry.name = "Custom folder";
        entry.isDir = true;
        entry.hasCustomIcon = true;
        FileModel model;
        model.setLocalIconPaths(true);
        std::vector<FileEntry> entries{entry};
        model.setListing(temporary.path(), std::move(entries));
        QCOMPARE(model.count(), 2);  // includes the parent row
        QVERIFY(model.findChild<MacFileIcons*>());
        const int row = model.rowOf(entry.name);
        QVERIFY(row >= 0);
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        model.data(model.index(row, FileModel::ColName), Qt::DecorationRole);
        QTRY_VERIFY(!changed.isEmpty());
        QIcon icon = model.data(model.index(row, FileModel::ColName), Qt::DecorationRole).value<QIcon>();
        QVERIFY(!icon.isNull());
        const QImage pixels = icon.pixmap(256).toImage();
        QVERIFY2(pixels.pixelColor(pixels.width() / 2, pixels.height() / 4).red() > 220, "Custom icon top should be red");
        QVERIFY2(pixels.pixelColor(pixels.width() / 2, pixels.height() * 3 / 4).blue() > 220, "Custom icon bottom should be blue");
        custom.fill(Qt::green);
        QVERIFY(setFixtureIcon(folder, custom));
        QSignalSpy refreshed(model.findChild<MacFileIcons*>(), &MacFileIcons::ready);
        model.applyUpdates({{entry.name, entry}});  // xattr update, unchanged mtime
        model.data(model.index(row, FileModel::ColName), Qt::DecorationRole);
        QTRY_VERIFY(!refreshed.isEmpty());
        icon = model.data(model.index(row, FileModel::ColName), Qt::DecorationRole).value<QIcon>();
        QVERIFY(icon.pixmap(64).toImage().pixelColor(32, 16).green() > 220);
    }

    void cachedAndOffThread()
    {
        std::atomic<int> calls{0};
        std::atomic<bool> offThread{false};
        MacFileIcons icons(nullptr, [&](const MacIconRequest& request) {
            ++calls;
            offThread = QThread::currentThread() != qApp->thread();
            QImage image(32, 32, QImage::Format_ARGB32);
            image.fill(request.path.isEmpty() ? Qt::green : Qt::red);
            return image;
        });
        FileEntry folder;
        folder.name = "One";
        folder.isDir = true;
        folder.hasCustomIcon = true;
        QSignalSpy ready(&icons, &MacFileIcons::ready);
        icons.get(folder);  // a remote/archive folder: no path is given to AppKit
        folder.name = "Two";
        icons.get(folder);
        QTRY_VERIFY(!ready.isEmpty());
        QCOMPARE(calls.load(), 1);
        QVERIFY(offThread.load());
        QCOMPARE(icons.get(folder).pixmap(32).toImage().pixelColor(16, 16), QColor(Qt::green));
        QCOMPARE(calls.load(), 1);

        ready.clear();
        icons.get(folder, "/fixture/folder");
        QTRY_VERIFY(!ready.isEmpty());
        QCOMPARE(calls.load(), 2);
        ready.clear();
        icons.invalidate("/fixture/folder");  // changed custom icon, identical file mtime
        icons.get(folder, "/fixture/folder");
        QTRY_VERIFY(!ready.isEmpty());
        QCOMPARE(calls.load(), 3);
    }

    void staleCompletion()
    {
        std::atomic<int> calls{0};
        QSemaphore entered, release;
        auto unblock = qScopeGuard([&] { release.release(); });
        MacFileIcons icons(nullptr, [&](const MacIconRequest&) {
            const int n = ++calls;
            if (n == 1) {
                entered.release();
                release.acquire();
            }
            QImage image(32, 32, QImage::Format_ARGB32);
            image.fill(n == 1 ? Qt::red : Qt::blue);
            return image;
        });
        FileEntry folder;
        folder.name = "Folder";
        folder.isDir = true;
        folder.hasCustomIcon = true;
        icons.get(folder, "/fixture/folder");
        QVERIFY(entered.tryAcquire(1, 2000));
        icons.reset();
        QSignalSpy ready(&icons, &MacFileIcons::ready);
        icons.get(folder, "/fixture/folder");
        QTRY_VERIFY(!ready.isEmpty());
        release.release();
        QTest::qWait(50);
        QCOMPARE(icons.get(folder, "/fixture/folder").pixmap(32).toImage().pixelColor(16, 16), QColor(Qt::blue));
    }

    void relistReusesIcons()
    {
        std::atomic<int> calls{0};
        MacFileIcons icons(nullptr, [&](const MacIconRequest& request) {
            ++calls;
            QImage image(request.pixels, request.pixels, QImage::Format_ARGB32);
            image.fill(Qt::red);
            return image;
        });
        FileEntry folder;
        folder.name = "Folder";
        folder.isDir = true;
        folder.hasCustomIcon = true;
        QSignalSpy ready(&icons, &MacFileIcons::ready);
        icons.get(folder, "/fixture/folder");
        QTRY_VERIFY(!ready.isEmpty());
        FileEntry file;
        file.name = "file.txt";
        ready.clear();
        icons.get(file, "/fixture/file.txt");
        QTRY_VERIFY(!ready.isEmpty());
        QCOMPARE(calls.load(), 2);
        for (int i = 0; i < 5; ++i) {
            icons.reset(true);
            icons.get(folder, "/fixture/folder");
            icons.get(file, "/fixture/file.txt");
        }
        QTest::qWait(50);
        QCOMPARE(calls.load(), 2);
        ready.clear();
        ++folder.iconRevision;  // Finder metadata can change without size or mtime
        icons.get(folder, "/fixture/folder");
        QTRY_VERIFY(!ready.isEmpty());
        QCOMPARE(calls.load(), 3);
        ready.clear();
        ++folder.mtime;
        icons.get(folder, "/fixture/folder");
        QTRY_VERIFY(!ready.isEmpty());
        QCOMPARE(calls.load(), 4);
        ready.clear();
        ++folder.size;
        icons.get(folder, "/fixture/folder");
        QTRY_VERIFY(!ready.isEmpty());
        QCOMPARE(calls.load(), 5);
    }

    void relistKeepsInFlight()
    {
        std::atomic<int> calls{0};
        QSemaphore entered, release;
        auto unblock = qScopeGuard([&] { release.release(); });
        MacFileIcons icons(nullptr, [&](const MacIconRequest&) {
            if (++calls == 1) {
                entered.release();
                release.acquire();
            }
            QImage image(64, 64, QImage::Format_ARGB32);
            image.fill(Qt::green);
            return image;
        });
        FileEntry folder;
        folder.isDir = true;
        folder.hasCustomIcon = true;
        QSignalSpy ready(&icons, &MacFileIcons::ready);
        icons.get(folder, "/fixture/folder");
        QVERIFY(entered.tryAcquire(1, 2000));
        icons.reset(true);
        icons.get(folder, "/fixture/folder");
        release.release();
        QTRY_VERIFY(!ready.isEmpty());
        QCOMPARE(calls.load(), 1);
    }

    void ordinaryFilesShareTypeIcons()
    {
        std::atomic<int> calls{0}, pathCalls{0};
        MacFileIcons icons(nullptr, [&](const MacIconRequest& request) {
            ++calls;
            if (!request.path.isEmpty())
                ++pathCalls;
            QImage image(request.pixels, request.pixels, QImage::Format_ARGB32);
            image.fill(Qt::green);
            return image;
        });
        FileEntry file;
        QSignalSpy ready(&icons, &MacFileIcons::ready);
        for (int i = 0; i < 1000; ++i) {
            file.name = QString("file%1.txt").arg(i);
            icons.get(file, "/fixture/" + file.name);
        }
        QTRY_VERIFY(!ready.isEmpty());
        QCOMPARE(calls.load(), 1);
        QCOMPARE(pathCalls.load(), 0);
        file.hasCustomIcon = true;
        ready.clear();
        icons.get(file, "/fixture/custom.txt");
        QTRY_VERIFY(!ready.isEmpty());
        QCOMPARE(pathCalls.load(), 1);
        file.hasCustomIcon = false;
        file.isDir = true;
        file.name = "Package.app";
        file.isPackage = true;
        ready.clear();
        icons.get(file, "/fixture/Package.app");
        QTRY_VERIFY(!ready.isEmpty());
        QCOMPARE(pathCalls.load(), 2);
    }

    void rasterSizesAndByteBudget()
    {
        std::atomic<int> calls{0}, pixels{0};
        MacFileIcons icons(nullptr, [&](const MacIconRequest& request) {
            ++calls;
            pixels = request.pixels;
            // Oversized loader results exercise byte-based eviction with few requests.
            QImage image(1024, 1024, QImage::Format_ARGB32);
            image.fill(Qt::green);
            return image;
        });
        FileEntry folder;
        folder.name = "Folder";
        folder.isDir = true;
        folder.hasCustomIcon = true;
        QSignalSpy ready(&icons, &MacFileIcons::ready);
        icons.get(folder, "/fixture/0");
        QTRY_VERIFY(!ready.isEmpty());
        QCOMPARE(pixels.load(), 64);
        ready.clear();
        icons.get(folder, "/fixture/0", 192);
        QTRY_VERIFY(!ready.isEmpty());
        QCOMPARE(pixels.load(), 192);
        for (int i = 1; i <= 5; ++i) {
            ready.clear();
            icons.get(folder, QString("/fixture/%1").arg(i));
            QTRY_VERIFY(!ready.isEmpty());
        }
        QCOMPARE(calls.load(), 7);
        ready.clear();
        icons.get(folder, "/fixture/0");
        QTRY_VERIFY(!ready.isEmpty());
        QCOMPARE(calls.load(), 8);  // old count-based cache would retain this icon
    }

    void retinaGridSettles_data()
    {
        QTest::addColumn<int>("count");
        QTest::addColumn<bool>("custom");
        QTest::newRow("plain folders") << 60 << false;
        QTest::newRow("custom folders") << 60 << true;
        QTest::newRow("large custom grid") << 240 << true;
    }

    void retinaGridSettles()
    {
        QFETCH(int, count);
        QFETCH(bool, custom);
        std::atomic<int> calls{0};
        MacFileIcons icons(nullptr, [&](const MacIconRequest& request) {
            ++calls;
            QImage image(request.pixels, request.pixels, QImage::Format_ARGB32);
            image.fill(Qt::green);
            return image;
        });
        icons.setVisibleItems(count, 192);
        auto paint = [&] {
            // Bound the failing case so a regression cannot spin forever.
            if (calls.load() > count * 4)
                return;
            for (int i = 0; i < count; ++i) {
                FileEntry folder;
                folder.name = QString("Folder %1").arg(i);
                folder.isDir = true;
                folder.hasCustomIcon = custom;
                icons.get(folder, "/fixture/" + folder.name, 192);
            }
        };
        connect(&icons, &MacFileIcons::ready, &icons, paint);
        paint();
        const int expected = custom ? count : 1;
        QTRY_VERIFY(calls.load() >= expected);
        QTest::qWait(200);  // ready() repaints must settle with the entire working set cached
        QCOMPARE(calls.load(), expected);
        for (int i = 0; i < 10; ++i)
            paint();
        QTest::qWait(50);
        QCOMPARE(calls.load(), expected);
    }

    void bulkListingMatchesStat()
    {
        QTemporaryDir temporary;
        const QString filePath = temporary.filePath("file.txt");
        {
            QFile file(filePath);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("fixture data");
        }
        QVERIFY(chmod(QFile::encodeName(filePath).constData(), 0751) == 0);
        QVERIFY(QDir(temporary.path()).mkdir("Folder"));
        QVERIFY(QFile::link(filePath, temporary.filePath("file link")));
        QVERIFY(QFile::link(temporary.filePath("Folder"), temporary.filePath("folder link")));
        QVERIFY(QFile::link(temporary.filePath("missing"), temporary.filePath("broken link")));
        QVERIFY(mkfifo(QFile::encodeName(temporary.filePath("pipe")).constData(), 0600) == 0);
        QFile hidden(temporary.filePath(".hidden"));
        QVERIFY(hidden.open(QIODevice::WriteOnly));
        hidden.close();
        ListResult listing, visible, cancelled;
        std::vector<EntryUpdate> stats;
        bool done = false;
        ioPool()->start([&] {
            ListOptions options;
            options.showHidden = true;
            auto all = listDirectory(temporary.path(), options);
            QStringList names;
            for (const auto& e : all.entries)
                names.append(e.name);
            auto updates = statNames(temporary.path(), names, true);
            auto shown = listDirectory(temporary.path(), {});
            std::atomic<bool> stop{true};
            auto stopped = listDirectory(temporary.path(), {}, &stop);
            postToUi([&, all = std::move(all), updates = std::move(updates), shown = std::move(shown), stopped]() mutable {
                listing = std::move(all);
                stats = std::move(updates);
                visible = std::move(shown);
                cancelled = stopped;
                done = true;
            });
        });
        QTRY_VERIFY(done);
        QCOMPARE(listing.error, 0);
        QCOMPARE(listing.entries.size(), size_t(7));
        QCOMPARE(visible.entries.size(), size_t(6));
        QCOMPARE(cancelled.error, ECANCELED);
        QVERIFY(cancelled.entries.empty());
        for (const FileEntry& e : listing.entries) {
            auto found = std::find_if(stats.begin(), stats.end(), [&](const EntryUpdate& u) { return u.first == e.name; });
            QVERIFY(found != stats.end() && found->second);
            const auto& st = *found->second;
            QCOMPARE(e.mode, st.mode);
            QCOMPARE(e.uid, st.uid);
            QCOMPARE(e.gid, st.gid);
            QCOMPARE(e.size, st.size);
            QCOMPARE(e.mtime, st.mtime);
            QCOMPARE(e.iconRevision, st.iconRevision);
            QCOMPARE(e.isDir, st.isDir);
            QCOMPARE(e.isLink, st.isLink);
            QCOMPARE(e.brokenLink, st.brokenLink);
        }
    }

    void bulkMetadataAcrossBatches()
    {
        QTemporaryDir temporary;
        for (int i = 0; i < 1000; ++i) {
            QFile file(temporary.filePath(QString(100, QLatin1Char('x')) + QString::number(i)));
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        const QString customName = QString::fromUtf8("Custom Grüße.txt");
        QFile custom(temporary.filePath(customName));
        QVERIFY(custom.open(QIODevice::WriteOnly));
        custom.close();
        QImage image(64, 64, QImage::Format_ARGB32);
        image.fill(Qt::red);
        QVERIFY(setFixtureIcon(custom.fileName(), image));
        std::optional<std::vector<FileEntry>> flags;
        bool done = false, invalidRejected = false;
        ioPool()->start([&] {
            const int fd = open(QFile::encodeName(temporary.path()).constData(), O_RDONLY | O_DIRECTORY);
            auto result = macListEntries(fd, true);
            if (fd >= 0)
                close(fd);
            const bool invalid = !macListEntries(-1, true).has_value();
            postToUi([&, result = std::move(result), invalid] {
                flags = result;
                invalidRejected = invalid;
                done = true;
            });
        });
        QTRY_VERIFY(done);
        QVERIFY(flags.has_value());
        QSet<QString> customNames;
        for (const FileEntry& e : *flags)
            if (e.hasCustomIcon)
                customNames.insert(e.name);
        QCOMPARE(customNames, QSet<QString>({customName}));
        QCOMPARE(flags->size(), size_t(1001));
        QVERIFY(invalidRejected);
    }

    void finderMetadataFromListing()
    {
        QTemporaryDir temporary;
        const QString plain = temporary.filePath("plain.txt");
        const QString custom = temporary.filePath("custom.txt");
        for (const QString& path : {plain, custom}) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("fixture");
        }
        for (const QString& name : {"Plain folder", "Custom folder", "Example.app", "Example.framework", "Example.rtfd", "Folder.txt"})
            QVERIFY(QDir(temporary.path()).mkdir(name));
        QImage image(64, 64, QImage::Format_ARGB32);
        image.fill(Qt::red);
        QVERIFY(setFixtureIcon(custom, image));
        QVERIFY(setFixtureIcon(temporary.filePath("Custom folder"), image));
        ListResult listing;
        bool done = false;
        ioPool()->start([&] {
            auto result = listDirectory(temporary.path(), {});
            postToUi([&, result = std::move(result)]() mutable { listing = std::move(result); done = true; });
        });
        QTRY_VERIFY(done);
        QCOMPARE(listing.error, 0);
        const FileEntry* customEntry = nullptr;
        for (const FileEntry& e : listing.entries) {
            if (e.name == "plain.txt")
                QVERIFY(!e.hasCustomIcon);
            if (e.name == "custom.txt")
                customEntry = &e;
            if (e.name == "Plain folder" || e.name == "Folder.txt") {
                QVERIFY(!e.hasCustomIcon);
                QVERIFY(!e.isPackage);
            }
            if (e.name == "Custom folder")
                QVERIFY(e.hasCustomIcon);
            if (e.name == "Example.app" || e.name == "Example.framework" || e.name == "Example.rtfd")
                QVERIFY(e.isPackage);
        }
        QVERIFY(customEntry && customEntry->hasCustomIcon);
        const int64_t revision = customEntry->iconRevision;
        QVERIFY(revision != 0);
        QTest::qWait(10);
        image.fill(Qt::blue);
        QVERIFY(setFixtureIcon(custom, image));
        std::vector<EntryUpdate> updates;
        done = false;
        ioPool()->start([&] {
            auto result = statNames(temporary.path(), {"custom.txt"}, false);
            postToUi([&, result = std::move(result)]() mutable { updates = std::move(result); done = true; });
        });
        QTRY_VERIFY(done);
        QCOMPARE(updates.size(), size_t(1));
        QVERIFY(updates[0].second && updates[0].second->hasCustomIcon);
        QVERIFY(updates[0].second->iconRevision != revision);
        // Selection uses worker-provided flags, not an xattr lookup in get().
        MacFileIcons icons;
        QSignalSpy ready(&icons, &MacFileIcons::ready);
        const FileEntry entry = *updates[0].second;
        icons.get(entry, custom);
        QTRY_VERIFY(!ready.isEmpty());
        const QImage pixels = icons.get(entry, custom).pixmap(64).toImage();
        QVERIFY(pixels.pixelColor(32, 32).blue() > 220);
    }

    void systemChoiceWithFallbackTheme()
    {
        const QString original = QIcon::themeName();
        IconCache::setTheme({});
        auto restore = qScopeGuard([&] { QIcon::setThemeName(original); IconCache::clear(); });
        QIcon::setThemeName("tandem-test-automatic-fallback");
        std::atomic<int> calls{0};
        MacFileIcons icons(nullptr, [&](const MacIconRequest&) {
            ++calls;
            QImage image(32, 32, QImage::Format_ARGB32);
            image.fill(Qt::green);
            return image;
        });
        FileEntry folder;
        folder.isDir = true;
        folder.hasCustomIcon = true;
        QSignalSpy ready(&icons, &MacFileIcons::ready);
        icons.get(folder);
        QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 1000);
        QCOMPARE(calls.load(), 1);
    }

    void explicitTheme()
    {
        const QString original = QIcon::themeName();
        IconCache::systemTheme();
        auto restore = qScopeGuard([&] { IconCache::setTheme({}); QIcon::setThemeName(original); });
        IconCache::setTheme("tandem-test-explicit-theme");
        std::atomic<int> calls{0};
        MacFileIcons icons(nullptr, [&](const MacIconRequest&) { ++calls; return QImage(); });
        FileEntry folder;
        folder.isDir = true;
        folder.hasCustomIcon = true;
        icons.get(folder, "/fixture/folder");
        QCOMPARE(calls.load(), 0);
    }
};

QTEST_MAIN(TestMacIcons)
#include "test_macicons.moc"
