// F3 viewer: modes, large files, search, navigation (run offscreen).

#include "TestLaunchGuard.h"
#include "ui/Theme.h"
#include "ui/viewer/ByteViews.h"
#include "ui/viewer/ImageView.h"
#include "ui/viewer/MediaControls.h"
#include "ui/viewer/ViewerWindow.h"

#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QDirIterator>
#include <QPainter>
#include <QPdfWriter>
#include <QProcess>
#include <QTextBrowser>
#include <QTreeWidget>
#include "core/FileOps.h"
#include "core/Archive.h"
#include <QImage>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSettings>
#include <QSignalSpy>
#include <QTextBlock>
#include <QTextLayout>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>

using namespace tandem;

class TestViewer : public QObject {
    Q_OBJECT

    QTemporaryDir m_tmp;
    QString P(const QString& n) const { return m_tmp.filePath(n); }

    static void writeFile(const QString& path, const QByteArray& data)
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(data);
    }

    // Opens a viewer and waits for the first load to finish.
    std::unique_ptr<ViewerWindow> open(const QStringList& files, int index = 0)
    {
        auto w = std::make_unique<ViewerWindow>(files, index);
        w->setAttribute(Qt::WA_DeleteOnClose, false);
        QSignalSpy spy(w.get(), &ViewerWindow::loaded);
        w->show();
        if (spy.isEmpty())
            spy.wait(5000);
        w->activateWindow();
        (void)QTest::qWaitForWindowActive(w.get());
        return w;
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        installTestLaunchGuard();  // never real apps
        qputenv("XDG_CONFIG_HOME", QFile::encodeName(m_tmp.filePath("config")));
        applyTheme(*qApp);

        writeFile(P("hello.cpp"), "int main()\n{\n    return 0; // hi\n}\n");
        writeFile(P("blob.bin"), QByteArray("\x7f" "ELF\0\0\0\1binary", 14) + QByteArray(1000, '\0'));
        QImage img(64, 32, QImage::Format_RGB32);
        img.fill(Qt::blue);
        QVERIFY(img.save(P("pic.png")));

        // ~3 MB of numbered lines with a unique marker near the end.
        QByteArray big;
        for (int i = 0; i < 60000; ++i)
            big += "line " + QByteArray::number(i) + " lorem ipsum dolor sit amet consectetur\n";
        big += "the NEEDLE-XYZ marker\n";
        for (int i = 0; i < 100; ++i)
            big += "tail " + QByteArray::number(i) + "\n";
        writeFile(P("big.log"), big);
    }

    void smallTextIsHighlighted()
    {
        auto w = open({P("hello.cpp")});
        QCOMPARE(w->mode(), ViewerWindow::Mode::Text);
        QVERIFY(!w->usesLargeTextView());
        QVERIFY(w->textEdit()->toPlainText().contains("return 0;"));
#ifdef TANDEM_TEST_HIGHLIGHTING
        // KSyntaxHighlighting is optional on every platform.
        QVERIFY(!w->textEdit()->document()->firstBlock().layout()->formats().isEmpty());
#else
        QVERIFY(w->textEdit()->document()->firstBlock().layout()->formats().isEmpty());
#endif
    }

    void binaryOpensAsHex()
    {
        auto w = open({P("blob.bin")});
        QCOMPARE(w->mode(), ViewerWindow::Mode::Hex);
        QTest::keyClick(w.get(), Qt::Key_1);  // force text mode
        QTRY_COMPARE(w->mode(), ViewerWindow::Mode::Text);
    }

    void imageOpensAsImage()
    {
        auto w = open({P("pic.png")});
        QCOMPARE(w->mode(), ViewerWindow::Mode::Image);
        QCOMPARE(w->imageView()->image().size(), QSize(64, 32));
        QTest::keyClick(w.get(), Qt::Key_3);
        QCOMPARE(w->mode(), ViewerWindow::Mode::Hex);
    }

    void largeTextScrollsAndSearches()
    {
        auto w = open({P("big.log")});
        QCOMPARE(w->mode(), ViewerWindow::Mode::Text);
        QVERIFY(w->usesLargeTextView());
        TextScrollView* v = w->largeTextView();
        QCOMPARE(v->topOffset(), qint64(0));

        QTest::keyClick(v, Qt::Key_Down);
        QTRY_COMPARE(v->topOffset(), qint64(QByteArray("line 0 lorem ipsum dolor sit amet consectetur\n").size()));
        QTest::keyClick(v, Qt::Key_Up);
        QTRY_COMPARE(v->topOffset(), qint64(0));

        QTest::keyClick(v, Qt::Key_End);
        QTRY_VERIFY(v->topOffset() > QFileInfo(P("big.log")).size() - 2000);

        QFile bigFile(P("big.log"));
        QVERIFY(bigFile.open(QIODevice::ReadOnly));
        const QByteArray all = bigFile.readAll();
        const qint64 needleAt = all.indexOf("NEEDLE-XYZ");
        QTest::keyClick(v, Qt::Key_Home);
        QTRY_COMPARE(v->topOffset(), qint64(0));
        QSignalSpy found(w.get(), &ViewerWindow::searchFinished);
        QTest::keyClick(w.get(), Qt::Key_F7);
        auto* edit = qobject_cast<QLineEdit*>(QApplication::focusWidget());
        QVERIFY(edit);
        QTest::keyClicks(edit, "needle-xyz");  // case-insensitive
        QTest::keyClick(edit, Qt::Key_Return);
        QVERIFY(found.wait(5000));
        QCOMPARE(found.first().first().toBool(), true);
        // The match line is on screen: top is a little before it.
        QTRY_VERIFY(v->topOffset() <= needleAt && v->topOffset() > needleAt - 4000);
    }

    void hexSearch()
    {
        auto w = open({P("blob.bin")});
        QSignalSpy found(w.get(), &ViewerWindow::searchFinished);
        QTest::keyClick(w.get(), Qt::Key_F7);
        QTest::keyClicks(QApplication::focusWidget(), "binary");
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
        QVERIFY(found.wait(5000));
        QVERIFY(found.first().first().toBool());
    }

    void nextAndPreviousFile()
    {
        auto w = open({P("hello.cpp"), P("pic.png"), P("big.log")});
        QSignalSpy spy(w.get(), &ViewerWindow::loaded);
        QTest::keyClick(w->textEdit(), Qt::Key_N);  // works with the text widget focused
        QTRY_COMPARE(w->currentPath(), P("pic.png"));
        QTRY_COMPARE(w->mode(), ViewerWindow::Mode::Image);
        QTest::keyClick(w.get(), Qt::Key_P);
        QTRY_COMPARE(w->currentPath(), P("hello.cpp"));
    }

    void fifoDoesNotHang()
    {
        QVERIFY(::mkfifo(QFile::encodeName(P("pipe")).constData(), 0600) == 0);
        auto w = open({P("pipe")});
        QTRY_VERIFY(!w->isLoading());
    }

    void jsonIsPrettyPrinted()
    {
        writeFile(P("min.json"), R"({"name":"tandem","tags":["fast","files"],"n":3})");
        auto w = open({P("min.json")});
        QCOMPARE(w->kind(), PreviewKind::Json);
        QCOMPARE(w->mode(), ViewerWindow::Mode::Text);
        QTRY_VERIFY(w->textEdit()->toPlainText().contains("\n    \"tags\": ["));
    }

    void markdownIsRendered()
    {
        writeFile(P("notes.md"), "# Title\n\nSome *emphasis* and a list:\n\n- one\n- two\n");
        auto w = open({P("notes.md")});
        QCOMPARE(w->kind(), PreviewKind::Markdown);
        QCOMPARE(w->mode(), ViewerWindow::Mode::Image);
        QTRY_VERIFY(w->documentView()->toPlainText().startsWith("Title"));
        QTest::keyClick(w.get(), Qt::Key_1);  // the source is still there
        QTRY_VERIFY(w->textEdit()->toPlainText().startsWith("# Title"));
    }

    void folderAndArchiveContents()
    {
        QVERIFY(QDir().mkpath(P("box/inner")));
        writeFile(P("box/a.txt"), "a");
        writeFile(P("box/b.txt"), "bb");
        auto w = open({P("box")});
        QCOMPARE(w->kind(), PreviewKind::Folder);
        QTRY_COMPARE(w->contentsList()->topLevelItemCount(), 3);
        QCOMPARE(w->contentsList()->topLevelItem(0)->text(0), QString("inner"));  // folders first
        QVERIFY(w->richInfo().contains("1 folders, 2 files"));

        PackJob pack({P("box")}, P("box.zip"));
        QSignalSpy done(&pack, &Job::finished);
        pack.start();
        QVERIFY(done.wait(5000));
        auto z = open({P("box.zip")});
        QCOMPARE(z->kind(), PreviewKind::Archive);
        QTRY_COMPARE(z->contentsList()->topLevelItemCount(), 1);
        QCOMPARE(z->contentsList()->topLevelItem(0)->text(0), QString("box"));
        QVERIFY(z->richInfo().contains("2 files"));
    }

    void pdfFirstPage()
    {
        {
            QPdfWriter pdf(P("doc.pdf"));
            pdf.setTitle("Tandem test");
            QPainter p(&pdf);
            p.drawText(100, 100, "page one");
            pdf.newPage();
            p.drawText(100, 100, "page two");
        }
        auto w = open({P("doc.pdf")});
        QCOMPARE(w->kind(), PreviewKind::Pdf);
        QTRY_VERIFY(!w->imageView()->image().isNull());
        QVERIFY(w->richInfo().contains("2 pages"));
        QVERIFY(w->richInfo().contains("Tandem test"));
    }

    void videoAndAudio()
    {
        if (QStandardPaths::findExecutable("ffmpeg").isEmpty() || QStandardPaths::findExecutable("ffprobe").isEmpty())
            QSKIP("ffmpeg not installed");
        QCOMPARE(QProcess::execute("ffmpeg", {"-v", "error", "-y", "-f", "lavfi", "-i",
                                              "testsrc=duration=2:size=320x240:rate=10", "-pix_fmt", "yuv420p",
                                              P("clip.mp4")}), 0);
        QCOMPARE(QProcess::execute("ffmpeg", {"-v", "error", "-y", "-f", "lavfi", "-i", "sine=frequency=440:duration=1",
                                              "-metadata", "title=Tone", "-metadata", "artist=Tandem", P("tone.mp3")}), 0);
        auto v = open({P("clip.mp4")});
        QCOMPARE(v->kind(), PreviewKind::Video);
        QTRY_VERIFY_WITH_TIMEOUT(!v->imageView()->image().isNull(), 15000);
        QCOMPARE(v->imageView()->image().size(), QSize(320, 240));
        QVERIFY(v->richInfo().contains("320 × 240"));
        auto a = open({P("tone.mp3")});
        QCOMPARE(a->kind(), PreviewKind::Audio);
        QTRY_VERIFY_WITH_TIMEOUT(a->richInfo().contains("Tandem — Tone"), 15000);
    }

    // Videos and music play in the viewer, but only when asked to (or with autoplay).
    void playback()
    {
        if (!MediaControls::available())
            QSKIP("built without Qt Multimedia");
        if (!QFile::exists(P("clip.mp4")))
            QSKIP("needs ffmpeg for the test video");
        QSettings().setValue("viewer/autoplay", false);
        auto v = open({P("clip.mp4"), P("hello.cpp")});
        MediaControls* media = v->mediaControls();
        QTRY_VERIFY(media->isVisible());
        QCOMPARE(media->source(), P("clip.mp4"));
        QVERIFY(!media->hasPlayer());  // nothing plays (or loads) by itself
        QTest::keyClick(v.get(), Qt::Key_M);  // tests stay quiet
        QSignalSpy frames(media, &MediaControls::frame);
        QTest::keyClick(v.get(), Qt::Key_Space);
        QVERIFY(media->hasPlayer());
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty(), 15000);
        QTRY_COMPARE(v->imageView()->image().size(), QSize(320, 240));
        QTest::keyClick(v.get(), Qt::Key_Space);
        QTRY_VERIFY(!media->isPlaying());

        // The next file stops it; a text file has no player bar.
        QTest::keyClick(v.get(), Qt::Key_N);
        QTRY_COMPARE(v->currentPath(), P("hello.cpp"));
        QVERIFY(!media->hasPlayer());
        QVERIFY(!media->isVisible());

        // With autoplay on, it starts by itself.
        QSettings().setValue("viewer/autoplay", true);
        auto w = open({P("clip.mp4")});
        QTRY_VERIFY_WITH_TIMEOUT(w->mediaControls()->hasPlayer(), 15000);
        QSettings().setValue("viewer/autoplay", false);
        w->mediaControls()->toggleMute();  // back to sound on
    }

    void fontSample()
    {
        QString font;
#ifdef Q_OS_MACOS
        const QString fontRoot = QStringLiteral("/System/Library/Fonts");
#else
        const QString fontRoot = QStringLiteral("/usr/share/fonts");
#endif
        QDirIterator it(fontRoot, {"*.ttf", "*.otf"}, QDir::Files, QDirIterator::Subdirectories);
        if (it.hasNext())
            font = it.next();
        if (font.isEmpty())
            QSKIP("no font files found");
        QFile::copy(font, P("sample") + font.mid(font.lastIndexOf('.')));
        auto w = open({P("sample") + font.mid(font.lastIndexOf('.'))});
        QCOMPARE(w->kind(), PreviewKind::Font);
        QTRY_VERIFY(!w->imageView()->image().isNull());
        QVERIFY(!w->richInfo().isEmpty());  // family name
    }

    void infoView()
    {
        writeFile(P("info.txt"), "abc");
        auto w = open({P("info.txt")});
        QTest::keyClick(w.get(), Qt::Key_I);
        QTRY_COMPARE(w->mode(), ViewerWindow::Mode::Info);
        auto labels = [&] {
            QStringList out;
            for (QLabel* l : w->infoPage()->findChildren<QLabel*>())
                if (l->isVisible())
                    out << l->text();
            return out;
        };
        QTRY_VERIFY(labels().contains("info.txt"));
        QVERIFY(labels().contains("3 B"));
        const QStringList all = labels();
        QVERIFY(std::any_of(all.begin(), all.end(), [](const QString& s) { return s.startsWith("-rw"); }));
        // SHA-256 on demand.
        QPushButton* compute = nullptr;
        for (QPushButton* b : w->infoPage()->findChildren<QPushButton*>())
            if (b->isVisible())
                compute = b;
        QVERIFY(compute);
        compute->click();
        QTRY_COMPARE(compute->text(), QString("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
        QTest::keyClick(w.get(), Qt::Key_I);  // back to the content
        QTRY_COMPARE(w->mode(), ViewerWindow::Mode::Text);

        // Folders: counts right away, total size once calculated.
        QVERIFY(QDir().mkpath(P("tree/sub")));
        writeFile(P("tree/sub/x.bin"), QByteArray(5000, 'x'));
        auto d = open({P("tree")});
        QTest::keyClick(d.get(), Qt::Key_I);
        auto hasTotal = [&] {
            for (QLabel* l : d->infoPage()->findChildren<QLabel*>())
                if (l->text() == "4.88 KiB")
                    return true;
            return false;
        };
        QTRY_VERIFY(hasTotal());
    }

    void escapeCloses()
    {
        auto w = open({P("hello.cpp")});
        QTest::keyClick(w.get(), Qt::Key_Escape);
        QTRY_VERIFY(!w->isVisible());
    }
};

QTEST_MAIN(TestViewer)
#include "test_viewer.moc"
