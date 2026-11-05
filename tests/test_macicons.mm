#import <AppKit/AppKit.h>

#include "core/Util.h"
#include "ui/FileModel.h"
#include "ui/IconCache.h"
#include "ui/MacFileIcons.h"
#include "ui/MacOpenWith.h"

#include <QApplication>
#include <QFile>
#include <QPainter>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <atomic>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/xattr.h>

using namespace tandem;

static QIcon solidIcon(Qt::GlobalColor color)
{
    QPixmap image(32, 32);
    image.fill(color);
    return QIcon(image);
}

static bool setFixtureIcon(const QString& path, const QImage& image)
{
    @autoreleasepool {
        CGImageRef cg = image.toCGImage();
        NSImage* icon = [[NSImage alloc] initWithCGImage:cg size:NSMakeSize(image.width(), image.height())];
        CGImageRelease(cg);
        NSString* native = [[NSString alloc] initWithCharacters:reinterpret_cast<const unichar*>(path.utf16()) length:path.size()];
        return [[NSWorkspace sharedWorkspace] setIcon:icon forFile:native options:0];
    }
}

static ListResult fixtureListing(const QString& path)
{
    std::optional<ListResult> result;
    ioPool()->start([&] {
        auto listing = listDirectory(path, {});
        postToUi([&, listing = std::move(listing)]() mutable { result = std::move(listing); });
    });
    if (!QTest::qWaitFor([&] { return result.has_value(); }))
        qFatal("Fixture listing timed out");
    return std::move(*result);
}

class TestMacIcons : public QObject {
    Q_OBJECT
private slots:
    void synchronousTypes()
    {
        int types = 0;
        std::atomic<int> paths{0};
        bool mainThread = false;
        const auto green = solidIcon(Qt::green);
        MacFileIcons icons(nullptr, [&](const FileEntry&) {
            ++types;
            mainThread = QThread::currentThread() == qApp->thread();
            return green;
        }, [&](const QString&) { ++paths; return QIcon(); });
        QSignalSpy ready(&icons, &MacFileIcons::ready);
        FileEntry file;
        for (int i = 0; i < 1000; ++i) {
            file.name = QString("file%1.txt").arg(i);
            QCOMPARE(icons.get(file, "/fixture/" + file.name).cacheKey(), green.cacheKey());
        }
        QCOMPARE(types, 1);
        QVERIFY(mainThread);
        QCOMPARE(paths.load(), 0);
        QCOMPARE(ready.count(), 0);  // no queue or repaint for type icons
        for (int i = 0; i < 1000; ++i) {
            file.name = QString("extensionless%1").arg(i);
            icons.get(file);
        }
        QCOMPARE(types, 2);
    }

    void nativeEngineSizes()
    {
        FileEntry folder;
        folder.isDir = true;
        const QIcon icon = macTypeIcon(folder);
        QVERIFY(!icon.isNull());
        for (int side : {16, 24, 96, 128}) {
            for (qreal scale : {1.0, 1.5, 2.0}) {
                const auto pixmap = icon.pixmap(QSize(side, side), scale);
                QCOMPARE(pixmap.size(), QSize(qRound(side * scale), qRound(side * scale)));
                QCOMPARE(pixmap.devicePixelRatioF(), scale);
                QVERIFY(pixmap.toImage().pixelColor(pixmap.width() / 2, pixmap.height() / 2).alpha() > 0);
            }
        }
        QImage image(192, 192, QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(2);
        image.fill(Qt::transparent);
        { QPainter painter(&image); icon.paint(&painter, QRect(0, 0, 96, 96)); }
        QVERIFY(image.pixelColor(96, 96).alpha() > 0);
        QIcon copy = icon;
        copy.setIsMask(true);  // detach exercises the native engine's clone
        QCOMPARE(copy.pixmap(QSize(96, 96), 2).size(), QSize(192, 192));
        QCOMPARE(icon.pixmap(QSize(24, 24), 2, QIcon::Disabled).devicePixelRatioF(), 2.0);
        FileEntry pdf;
        pdf.name = "document.pdf";
        QVERIFY(icon.pixmap(64).toImage() != macTypeIcon(pdf).pixmap(64).toImage());
    }

    void drawingsFollowAppearance()
    {
        FileEntry folder;
        folder.isDir = true;
        const QIcon icon = macTypeIcon(folder);
        const qint64 drawn = icon.pixmap(QSize(32, 32), 2).cacheKey();
        QCOMPARE(icon.pixmap(QSize(32, 32), 2).cacheKey(), drawn);
        macForgetDrawnIcons();
        const QPixmap redrawn = icon.pixmap(QSize(32, 32), 2);
        QVERIFY(redrawn.cacheKey() != drawn);
        QVERIFY(redrawn.toImage().pixelColor(32, 32).alpha() > 0);
    }

    void appearanceChangeResets()
    {
        int types = 0;
        std::atomic<int> paths{0};
        QSemaphore entered, release;
        auto unblock = qScopeGuard([&] { release.release(10); });
        const auto green = solidIcon(Qt::green), blue = solidIcon(Qt::blue), red = solidIcon(Qt::red);
        MacFileIcons icons(nullptr, [&](const FileEntry&) { return ++types == 1 ? green : blue; }, [&](const QString&) {
            if (++paths == 1) { entered.release(); release.acquire(); }
            return red;
        });
        FileEntry file;
        file.name = "Custom.txt";
        file.hasCustomIcon = true;
        QSignalSpy ready(&icons, &MacFileIcons::ready);
        QSignalSpy changed(&icons, &MacFileIcons::changed);
        QCOMPARE(icons.get(file, "/fixture/Custom.txt").cacheKey(), green.cacheKey());
        QVERIFY(entered.tryAcquire(1, 2000));  // a lookup for the old appearance is running
        emit QGuiApplication::styleHints()->colorSchemeChanged(Qt::ColorScheme::Dark);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(icons.get(file, "/fixture/Custom.txt").cacheKey(), blue.cacheKey());  // type looked up again
        QCOMPARE(types, 2);
        QTRY_COMPARE(ready.count(), 1);  // the new lookup
        release.release();
        QTest::qWait(50);
        QCOMPARE(ready.count(), 1);  // the old one is dropped
        QCOMPARE(paths.load(), 2);
        QCOMPARE(icons.get(file, "/fixture/Custom.txt").cacheKey(), red.cacheKey());

        FileEntry pdf;
        pdf.name = "Shared.pdf";
        FileModel model;
        model.setListing("/fixture/model", {pdf, file});
        QSignalSpy repaint(&model, &QAbstractItemModel::dataChanged);
        MacFileIcons::instance().reset();
        QCOMPARE(repaint.count(), 1);
        QCOMPARE(repaint.first().at(0).value<QModelIndex>().row(), 0);
        QCOMPARE(repaint.first().at(1).value<QModelIndex>().row(), model.count() - 1);
    }

    void sharedAcrossModels()
    {
        FileEntry file;
        file.name = "Shared.pdf";
        FileModel left, right;
        left.setLocalIconPaths(true);
        right.setLocalIconPaths(true);
        left.setListing("/fixture/left", {file});
        right.setListing("/fixture/right", {file});
        const QIcon a = left.fileIcon(left.rowOf(file.name));
        const QIcon b = right.fileIcon(right.rowOf(file.name));
        QVERIFY(!a.isNull());
        QCOMPARE(a.cacheKey(), b.cacheKey());
        left.setListing("/fixture/left", {file});
        QCOMPARE(left.fileIcon(left.rowOf(file.name)).cacheKey(), a.cacheKey());
    }

    void pathWorkersAndCtime()
    {
        std::atomic<int> calls{0};
        std::atomic<bool> offThread{false};
        const auto green = solidIcon(Qt::green), red = solidIcon(Qt::red), blue = solidIcon(Qt::blue);
        MacFileIcons icons(nullptr, [&](const FileEntry&) { return green; }, [&](const QString&) {
            offThread = QThread::currentThread() != qApp->thread();
            return ++calls == 1 ? red : blue;
        });
        FileEntry file;
        file.name = "Custom.txt";
        file.hasCustomIcon = true;
        file.iconRevision = 1;
        QSignalSpy ready(&icons, &MacFileIcons::ready);
        QCOMPARE(icons.get(file, "/fixture/Custom.txt").cacheKey(), green.cacheKey());
        QTRY_COMPARE(ready.count(), 1);
        QVERIFY(offThread.load());
        QCOMPARE(icons.get(file, "/fixture/Custom.txt").cacheKey(), red.cacheKey());
        ++file.iconRevision;
        QCOMPARE(icons.get(file, "/fixture/Custom.txt").cacheKey(), green.cacheKey());
        QTRY_COMPARE(ready.count(), 2);
        QCOMPARE(icons.get(file, "/fixture/Custom.txt").cacheKey(), blue.cacheKey());
        for (int i = 0; i < 10; ++i) {
            ++file.mtime;
            ++file.size;
            icons.get(file, "/fixture/Custom.txt");
        }
        QCOMPARE(calls.load(), 2);  // path + ctime, independent of model re-listing
        file.hasCustomIcon = false;
        QCOMPARE(icons.get(file, "/fixture/Custom.txt").cacheKey(), green.cacheKey());
        file.isDir = true;
        file.isPackage = true;
        icons.get(file, "/fixture/Package.app");
        QTRY_COMPARE(ready.count(), 3);
        icons.get(file);  // archives, servers and searches provide no path
        QCOMPARE(calls.load(), 3);
    }

    void oldCompletionCannotReplaceNewIcon()
    {
        std::atomic<int> calls{0};
        QSemaphore entered, release;
        auto unblock = qScopeGuard([&] { release.release(); });
        const auto red = solidIcon(Qt::red), blue = solidIcon(Qt::blue), green = solidIcon(Qt::green);
        MacFileIcons icons(nullptr, [&](const FileEntry&) { return green; }, [&](const QString&) {
            if (++calls == 1) { entered.release(); release.acquire(); return red; }
            return blue;
        });
        FileEntry folder;
        folder.isDir = folder.hasCustomIcon = true;
        folder.iconRevision = 1;
        QSignalSpy ready(&icons, &MacFileIcons::ready);
        icons.get(folder, "/fixture/folder");
        QVERIFY(entered.tryAcquire(1, 2000));
        ++folder.iconRevision;
        icons.get(folder, "/fixture/folder");
        QTRY_COMPARE(ready.count(), 1);
        release.release();
        QTRY_COMPARE(ready.count(), 2);
        QCOMPARE(icons.get(folder, "/fixture/folder").cacheKey(), blue.cacheKey());
        QTest::qWait(20);
        QCOMPARE(calls.load(), 2);
    }

    void failureIsCached()
    {
        std::atomic<int> calls{0};
        const auto green = solidIcon(Qt::green);
        MacFileIcons icons(nullptr, [&](const FileEntry&) { return green; }, [&](const QString&) { ++calls; return QIcon(); });
        FileEntry file;
        file.hasCustomIcon = true;
        QSignalSpy ready(&icons, &MacFileIcons::ready);
        icons.get(file, "/fixture/missing");
        QTRY_COMPARE(ready.count(), 1);
        for (int i = 0; i < 100; ++i)
            QCOMPARE(icons.get(file, "/fixture/missing").cacheKey(), green.cacheKey());
        QCOMPARE(calls.load(), 1);
    }

    void gridSettles_data()
    {
        QTest::addColumn<int>("count");
        QTest::addColumn<bool>("custom");
        QTest::addColumn<bool>("perFrame");  // a view paints once per frame, after many loads
        QTest::newRow("plain") << 60 << false << false;
        QTest::newRow("custom") << 60 << true << false;
        QTest::newRow("large custom") << 240 << true << false;
        QTest::newRow("large custom, per frame") << 240 << true << true;
    }

    void gridSettles()
    {
        QFETCH(int, count);
        QFETCH(bool, custom);
        QFETCH(bool, perFrame);
        int types = 0;
        std::atomic<int> calls{0}, running{0}, peak{0};
        const auto green = solidIcon(Qt::green);
        MacFileIcons icons(nullptr, [&](const FileEntry&) { ++types; return green; }, [&](const QString&) {
            ++calls;
            const int n = ++running;
            int previous = peak.load();
            while (previous < n && !peak.compare_exchange_weak(previous, n)) {}
            QThread::msleep(1);
            --running;
            return green;
        });
        auto paint = [&] {
            for (int i = 0; i < count; ++i) {
                FileEntry folder;
                folder.name = QString("Folder %1").arg(i);
                folder.isDir = true;
                folder.hasCustomIcon = custom;
                icons.get(folder, "/fixture/" + folder.name).pixmap(QSize(96, 96), 2);
            }
        };
        QTimer frame;
        frame.setSingleShot(true);
        frame.setInterval(16);
        connect(&frame, &QTimer::timeout, &icons, paint);
        connect(&icons, &MacFileIcons::ready, &icons, [&] {
            if (!perFrame)
                paint();
            else if (!frame.isActive())
                frame.start();
        });
        QSignalSpy ready(&icons, &MacFileIcons::ready);
        paint();
        QTRY_COMPARE(ready.count(), custom ? count : 0);
        for (int i = 0; i < 10; ++i)
            paint();
        QTest::qWait(50);
        QCOMPARE(calls.load(), custom ? count : 0);
        QCOMPARE(types, 1);
        QVERIFY(peak.load() <= 2);
    }

    // More custom icons on screen than the cache holds: none is dropped while
    // shown (each repaint would load it again), and once off screen they go.
    void pathCacheKeepsShownIcons()
    {
        std::atomic<int> calls{0};
        const auto green = solidIcon(Qt::green), red = solidIcon(Qt::red);
        MacFileIcons icons(nullptr, [&](const FileEntry&) { return green; }, [&](const QString&) { ++calls; return red; });
        QSignalSpy ready(&icons, &MacFileIcons::ready);
        // Like the thumbnail grid, every load repaints all that is on screen,
        // so loads may take longer than kShownMs.
        QList<int> onScreen;
        auto paint = [&] {
            for (int batch : onScreen) {
                for (int i = 0; i < 96; ++i) {
                    FileEntry folder;
                    folder.isDir = folder.hasCustomIcon = true;
                    icons.get(folder, QString("/fixture/%1/%2").arg(batch).arg(i));
                }
            }
        };
        connect(&icons, &MacFileIcons::ready, &icons, paint);
        static_assert(2 * 96 == MacFileIcons::kMaxPathIcons);
        onScreen = {0, 1, 2, 3};  // 384 at once, as on a very large screen
        paint();
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 384, 20000);
        for (int i = 0; i < 10; ++i)
            paint();
        QTest::qWait(50);
        QCOMPARE(calls.load(), 384);  // nothing reloaded

        QTest::qWait(MacFileIcons::kShownMs + 100);
        onScreen = {3, 4};  // the others are now off screen
        paint();
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 384 + 96, 20000);
        onScreen = {0, 3, 4};
        paint();
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 384 + 96 + 96, 20000);
        QTest::qWait(50);
        // Batch 0 was dropped to stay within the bound; batch 3 was kept.
        QCOMPARE(calls.load(), 384 + 96 + 96);
    }

    void finderCustomIconsAndXattrChanges()
    {
        QTemporaryDir temporary;
        QVERIFY(QDir(temporary.path()).mkdir("Custom folder"));
        const QString folder = temporary.filePath("Custom folder");
        QFile file(temporary.filePath("Custom.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        QVERIFY(QDir(temporary.path()).mkdir("Example.app"));
        QVERIFY(QDir(temporary.path()).mkdir("Example.framework"));
        QVERIFY(QDir(temporary.path()).mkdir("Plain folder"));
        QImage custom(256, 256, QImage::Format_RGBA8888_Premultiplied);
        custom.fill(Qt::red);
        for (int y = 128; y < 256; ++y)
            for (int x = 0; x < 256; ++x)
                custom.setPixelColor(x, y, Qt::blue);
        QVERIFY(setFixtureIcon(folder, custom));
        QVERIFY(setFixtureIcon(file.fileName(), custom));
        auto listing = fixtureListing(temporary.path());
        QCOMPARE(listing.error, 0);
        for (const auto& e : listing.entries) {
            if (e.name == "Custom folder" || e.name == "Custom.txt")
                QVERIFY(e.hasCustomIcon);
            if (e.name == "Plain folder")
                QVERIFY(!e.hasCustomIcon && !e.isPackage);
            if (e.name == "Example.app" || e.name == "Example.framework")
                QVERIFY(e.isPackage);
        }
        FileModel left, right;
        left.setLocalIconPaths(true);
        right.setLocalIconPaths(true);
        left.setListing(temporary.path(), std::vector<FileEntry>(listing.entries));
        right.setListing(temporary.path(), std::move(listing.entries));
        const int row = left.rowOf("Custom folder");
        QVERIFY(row >= 0);
        QSignalSpy changed(&left, &QAbstractItemModel::dataChanged);
        QSignalSpy otherChanged(&right, &QAbstractItemModel::dataChanged);
        left.fileIcon(row);
        right.fileIcon(right.rowOf("Custom folder"));
        QTRY_COMPARE(changed.count(), 1);
        QTRY_COMPARE(otherChanged.count(), 1);
        const QIcon original = left.fileIcon(row);
        QCOMPARE(original.cacheKey(), right.fileIcon(right.rowOf("Custom folder")).cacheKey());
        const QImage pixels = original.pixmap(QSize(96, 96), 2).toImage();
        QVERIFY(pixels.pixelColor(96, 48).red() > 220);
        QVERIFY(pixels.pixelColor(96, 144).blue() > 220);
        const int fileRow = left.rowOf("Custom.txt");
        QVERIFY(fileRow >= 0);
        left.fileIcon(fileRow);
        QTRY_COMPARE(changed.count(), 2);
        QVERIFY(left.fileIcon(fileRow).pixmap(64).toImage().pixelColor(32, 16).red() > 220);
        {
            // Drawn to pixels on the worker: drawing reads nothing from the file.
            const QString copy = temporary.filePath("Copy.txt");
            QVERIFY(QFile::copy(file.fileName(), copy));
            QVERIFY(setFixtureIcon(copy, custom));
            const QIcon loaded = macPathIcon(copy);
            QVERIFY(removexattr(QFile::encodeName(copy).constData(), "com.apple.ResourceFork", XATTR_NOFOLLOW) == 0);
            QVERIFY(QFile::remove(copy));
            for (int side : {16, 24, 96}) {
                const QImage image = loaded.pixmap(QSize(side, side), 2).toImage();
                QCOMPARE(image.size(), QSize(side * 2, side * 2));
                QVERIFY(image.pixelColor(side, side / 2).red() > 220);
                QVERIFY(image.pixelColor(side, side * 3 / 2).blue() > 220);
            }
        }
        const auto oldMtime = left.entry(fileRow)->mtime;
        const auto oldCtime = left.entry(fileRow)->iconRevision;
        custom.fill(Qt::green);
        // AppKit may touch mtime; keep it, so only the icon xattrs and ctime change.
        struct stat before;
        QVERIFY(::stat(QFile::encodeName(file.fileName()).constData(), &before) == 0);
        QVERIFY(setFixtureIcon(file.fileName(), custom));
        const struct timespec times[2] = {before.st_atimespec, before.st_mtimespec};
        QVERIFY(::utimensat(AT_FDCWD, QFile::encodeName(file.fileName()).constData(), times, 0) == 0);
        listing = fixtureListing(temporary.path());
        left.setListing(temporary.path(), std::move(listing.entries));
        const int refreshed = left.rowOf("Custom.txt");
        QCOMPARE(left.entry(refreshed)->mtime, oldMtime);
        QVERIFY(left.entry(refreshed)->iconRevision != oldCtime);
        changed.clear();
        left.fileIcon(refreshed);
        QTRY_COMPARE(changed.count(), 1);
        QVERIFY(left.fileIcon(refreshed).pixmap(64).toImage().pixelColor(32, 32).green() > 220);
        // A model browsing an archive/search/server must never use a per-file icon.
        FileModel remote;
        remote.setLocalIconPaths(false);
        FileEntry flagged = *left.entry(refreshed);
        remote.setListing("/fixture/archive", {flagged});
        const auto plain = MacFileIcons::instance().get(flagged);  // no path: the .txt type icon
        QCOMPARE(remote.fileIcon(remote.rowOf(flagged.name)).cacheKey(), plain.cacheKey());
    }

    void explicitThemeBypassesNativeLookup()
    {
        const auto original = QIcon::themeName();
        auto restore = qScopeGuard([&] { IconCache::setTheme({}); QIcon::setThemeName(original); });
        IconCache::setTheme("tandem-test-explicit-theme");
        int calls = 0;
        MacFileIcons icons(nullptr, [&](const FileEntry&) { ++calls; return QIcon(); }, [&](const QString&) { ++calls; return QIcon(); });
        FileEntry folder;
        folder.isDir = folder.hasCustomIcon = true;
        icons.get(folder, "/fixture/folder");
        QCOMPARE(calls, 0);
        IconCache::setTheme({});
        QIcon::setThemeName("tandem-automatic-fallback-theme");
        icons.get(folder);
        QCOMPARE(calls, 1);
    }

    // "Open with" offers what Launch Services offers: TextEdit for text,
    // Preview for PDF, the default app first, and only apps for every file.
    void openWithApps()
    {
        QTemporaryDir temporary;
        const QString text = temporary.filePath("notes.txt"), pdf = temporary.filePath("paper.pdf");
        for (const QString& path : {text, pdf}) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        auto has = [](const QList<MacApp>& apps, const QString& path) {
            return std::any_of(apps.cbegin(), apps.cend(), [&](const MacApp& a) { return a.path == path; });
        };
        const QList<MacApp> forText = macAppsForAll({text});
        QVERIFY(has(forText, "/System/Applications/TextEdit.app"));
        QVERIFY(forText.first().isDefault);
        QCOMPARE(std::count_if(forText.cbegin(), forText.cend(), [](const MacApp& a) { return a.isDefault; }), 1);
        for (const MacApp& app : forText) {
            QVERIFY(!app.name.isEmpty() && !app.name.endsWith(".app"));
            QVERIFY(app.path.endsWith(".app"));
        }
        QVERIFY(has(macAppsForAll({pdf}), "/System/Applications/Preview.app"));
        const QList<MacApp> both = macAppsForAll({text, pdf});
        for (const MacApp& app : both)
            QVERIFY(has(forText, app.path) && has(macAppsForAll({pdf}), app.path));
        QVERIFY(!macAppIcon("/System/Applications/TextEdit.app").isNull());
    }
};

QTEST_MAIN(TestMacIcons)
#include "test_macicons.moc"
