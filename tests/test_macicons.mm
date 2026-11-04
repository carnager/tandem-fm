#import <AppKit/AppKit.h>

#include "core/Util.h"
#include "ui/FileModel.h"
#include "ui/IconCache.h"
#include "ui/MacFileIcons.h"

#include <QApplication>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>

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
        QVERIFY2(pixels.pixelColor(128, 64).red() > 220, "Custom icon top should be red");
        QVERIFY2(pixels.pixelColor(128, 192).blue() > 220, "Custom icon bottom should be blue");
        custom.fill(Qt::green);
        QVERIFY(setFixtureIcon(folder, custom));
        QSignalSpy refreshed(model.findChild<MacFileIcons*>(), &MacFileIcons::ready);
        model.applyUpdates({{entry.name, entry}});  // xattr update, unchanged mtime
        model.data(model.index(row, FileModel::ColName), Qt::DecorationRole);
        QTRY_VERIFY(!refreshed.isEmpty());
        icon = model.data(model.index(row, FileModel::ColName), Qt::DecorationRole).value<QIcon>();
        QVERIFY(icon.pixmap(256).toImage().pixelColor(128, 64).green() > 220);
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

    void refreshKeepsPreviousIcon()
    {
        std::atomic<int> calls{0};
        MacFileIcons icons(nullptr, [&](const MacIconRequest&) {
            QImage image(32, 32, QImage::Format_ARGB32);
            image.fill(++calls == 1 ? Qt::red : Qt::blue);
            return image;
        });
        FileEntry folder;
        folder.name = "Folder";
        folder.isDir = true;
        QSignalSpy ready(&icons, &MacFileIcons::ready);
        icons.get(folder, "/fixture/folder");
        QTRY_VERIFY(!ready.isEmpty());
        ready.clear();
        icons.reset(true);
        QCOMPARE(icons.get(folder, "/fixture/folder").pixmap(32).toImage().pixelColor(16, 16), QColor(Qt::red));
        QTRY_VERIFY(!ready.isEmpty());
        QCOMPARE(icons.get(folder, "/fixture/folder").pixmap(32).toImage().pixelColor(16, 16), QColor(Qt::blue));
    }

    void explicitTheme()
    {
        const QString original = QIcon::themeName();
        IconCache::systemTheme();
        auto restore = qScopeGuard([&] { QIcon::setThemeName(original); IconCache::clear(); });
        QIcon::setThemeName("tandem-test-explicit-theme");
        std::atomic<int> calls{0};
        MacFileIcons icons(nullptr, [&](const MacIconRequest&) { ++calls; return QImage(); });
        FileEntry folder;
        folder.isDir = true;
        icons.get(folder, "/fixture/folder");
        QCOMPARE(calls.load(), 0);
    }
};

QTEST_MAIN(TestMacIcons)
#include "test_macicons.moc"
