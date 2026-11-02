// F3 viewer: modes, large files, search, navigation (run offscreen).

#include "ui/Theme.h"
#include "ui/viewer/ByteViews.h"
#include "ui/viewer/ImageView.h"
#include "ui/viewer/ViewerWindow.h"

#include <QApplication>
#include <QImage>
#include <QLineEdit>
#include <QPlainTextEdit>
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
        // KSyntaxHighlighting adds formats to the blocks.
        QVERIFY(!w->textEdit()->document()->firstBlock().layout()->formats().isEmpty());
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

    void escapeCloses()
    {
        auto w = open({P("hello.cpp")});
        QTest::keyClick(w.get(), Qt::Key_Escape);
        QTRY_VERIFY(!w->isVisible());
    }
};

QTEST_MAIN(TestViewer)
#include "test_viewer.moc"
