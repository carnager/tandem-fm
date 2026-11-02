// Drives the real main window with keystrokes (run with QT_QPA_PLATFORM=offscreen).

#include "core/Archive.h"
#include "core/DirWatcher.h"
#include "core/FolderHistory.h"
#include "ui/DirTab.h"
#include "ui/FileModel.h"
#include "ui/FileView.h"
#include "ui/FuzzyPopup.h"
#include "ui/IconCache.h"
#include "ui/MainWindow.h"
#include "ui/MultiRenameDialog.h"
#include "ui/Panel.h"
#include "ui/Theme.h"
#include "ui/viewer/ViewerWindow.h"

#include <QPlainTextEdit>
#include <QSignalSpy>

#include <QApplication>
#include <QSettings>
#include <QImage>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QMenu>
#include <QDialog>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace tandem;

namespace {

void writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

// Answers the next modal dialog once it is open.
void answerNextDialog(std::function<void(QWidget*)> answer)
{
    auto attempt = std::make_shared<std::function<void(int)>>();
    *attempt = [answer, attempt](int tries) {
        QWidget* w = QApplication::activeModalWidget();
        if (!w) {
            if (tries > 0)
                QTimer::singleShot(20, [attempt, tries] { (*attempt)(tries - 1); });
            return;
        }
        answer(w);
    };
    QTimer::singleShot(20, [attempt] { (*attempt)(200); });
}

// Picks an entry from the next popup menu (searching submenus too).
void chooseFromNextMenu(const QString& text)
{
    QTimer::singleShot(50, [text] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        QVERIFY(menu);
        std::function<QAction*(QMenu*)> find = [&](QMenu* m) -> QAction* {
            for (QAction* a : m->actions()) {
                if (a->text() == text)
                    return a;
                if (a->menu())
                    if (QAction* sub = find(a->menu()))
                        return sub;
            }
            return nullptr;
        };
        QAction* a = find(menu);
        QVERIFY2(a, qPrintable(text));
        menu->close();
        a->trigger();
    });
}

void rightClick(QAbstractItemView* view, int row)
{
    const QPoint pos = view->visualRect(view->model()->index(row, 0)).center();
    QContextMenuEvent ev(QContextMenuEvent::Mouse, pos, view->viewport()->mapToGlobal(pos));
    QApplication::sendEvent(view->viewport(), &ev);
}

QByteArray readName(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void acceptDialog(QWidget* w)
{
    if (auto* box = qobject_cast<QMessageBox*>(w)) {
        box->button(QMessageBox::Yes)->click();
        return;
    }
    if (auto* d = qobject_cast<QDialog*>(w))
        d->accept();
}

}  // namespace

class TestUi : public QObject {
    Q_OBJECT

    QTemporaryDir m_tmp;
    std::unique_ptr<MainWindow> m_window;
    Panel* m_left = nullptr;
    Panel* m_right = nullptr;

    QString L(const QString& rel = {}) const { return m_tmp.filePath("left" + (rel.isEmpty() ? "" : "/" + rel)); }
    QString R(const QString& rel = {}) const { return m_tmp.filePath("right" + (rel.isEmpty() ? "" : "/" + rel)); }
    FileModel* leftModel() const { return m_left->tab()->model(); }
    FileModel* rightModel() const { return m_right->tab()->model(); }

    void moveCursorTo(Panel* p, const QString& name)
    {
        const int row = p->tab()->model()->rowOf(name);
        QVERIFY2(row >= 0, qPrintable(name));
        p->view()->setCurrentRow(row);
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        qputenv("XDG_CONFIG_HOME", QFile::encodeName(m_tmp.filePath("config")));
        applyTheme(*qApp);
        DirWatcher::instance();
        FolderHistory::instance().setStoragePath(m_tmp.filePath("folders.tsv"));

        QVERIFY(QDir().mkpath(L("sub/deep")));
        QVERIFY(QDir().mkpath(R()));
        writeFile(L("file1.txt"), "one");
        writeFile(L("file2.txt"), "two two");
        writeFile(L("file10.txt"), "ten ten ten");
        writeFile(L("photo.jpg"), QByteArray(5000, 'p'));
        writeFile(L("sub/inner.txt"), "inner");

        m_window = std::make_unique<MainWindow>(QStringList{L(), R()});
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
        m_left = m_window->findChild<Panel*>("leftPanel");
        m_right = m_window->findChild<Panel*>("rightPanel");
        QVERIFY(m_left && m_right);
        QTRY_COMPARE(leftModel()->path(), L());
        QTRY_COMPARE(rightModel()->path(), R());
        m_left->view()->setFocus();
        QTRY_VERIFY(m_left->isActive());
    }

    // The offscreen platform does not reactivate the window after a dialog
    // closes (a real compositor does), so do it before every step.
    void init()
    {
        if (!m_window)
            return;
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        if (!m_right->isActive())
            m_left->view()->setFocus();
    }

    void listingIsSortedNaturally()
    {
        QStringList names;
        for (int r = 0; r < leftModel()->count(); ++r)
            names << leftModel()->entry(r)->name;
        QCOMPARE(names, QStringList({"..", "sub", "file1.txt", "file2.txt", "file10.txt", "photo.jpg"}));
    }

    void enterAndLeaveFolder()
    {
        moveCursorTo(m_left, "sub");
        QTest::keyClick(m_left->view(), Qt::Key_Return);
        QTRY_COMPARE(leftModel()->path(), L("sub"));
        QTest::keyClick(m_left->view(), Qt::Key_Backspace);
        QTRY_COMPARE(leftModel()->path(), L());
        QCOMPARE(m_left->currentName(), QString("sub"));  // cursor returns to where we came from
        QTest::keyClick(m_left->view(), Qt::Key_Left, Qt::AltModifier);  // history back
        QTRY_COMPARE(leftModel()->path(), L("sub"));
        QTest::keyClick(m_left->view(), Qt::Key_Backspace);
        QTRY_COMPARE(leftModel()->path(), L());
    }

    void quickSearch()
    {
        m_left->view()->setCurrentRow(0);
        QTest::keyClicks(m_left->view(), "pho");
        QTRY_COMPARE(m_left->currentName(), QString("photo.jpg"));
        QWidget* search = QApplication::focusWidget();
        QVERIFY(qobject_cast<QLineEdit*>(search));
        QTest::keyClick(search, Qt::Key_Escape);
        QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget*>(m_left->view()));
    }

    void markWithInsertAndPattern()
    {
        moveCursorTo(m_left, "file1.txt");
        QTest::keyClick(m_left->view(), Qt::Key_Insert);
        QTest::keyClick(m_left->view(), Qt::Key_Insert);
        QCOMPARE(leftModel()->markedNames(), QStringList({"file1.txt", "file2.txt"}));
        QCOMPARE(leftModel()->stats().markedBytes, int64_t(3 + 7));
        leftModel()->markAll(false);
        QCOMPARE(leftModel()->markByPattern("*.txt", true), 3);
        leftModel()->markAll(false);
    }

    void copyWithF5()
    {
        moveCursorTo(m_left, "file1.txt");
        QTest::keyClick(m_left->view(), Qt::Key_Insert);
        moveCursorTo(m_left, "sub");
        QTest::keyClick(m_left->view(), Qt::Key_Insert);
        answerNextDialog(acceptDialog);
        QTest::keyClick(m_left->view(), Qt::Key_F5);
        QTRY_VERIFY(QFile::exists(R("sub/inner.txt")));
        QTRY_VERIFY(rightModel()->rowOf("file1.txt") >= 0);  // watcher updated the other panel
        QTRY_VERIFY(rightModel()->rowOf("sub") >= 0);
        QCOMPARE(leftModel()->markedNames(), QStringList());  // marks cleared after the op
    }

    void moveWithF6()
    {
        writeFile(L("movable.txt"), "m");
        QTRY_VERIFY(leftModel()->rowOf("movable.txt") >= 0);
        moveCursorTo(m_left, "movable.txt");
        answerNextDialog(acceptDialog);
        QTest::keyClick(m_left->view(), Qt::Key_F6);
        QTRY_VERIFY(QFile::exists(R("movable.txt")));
        QTRY_COMPARE(leftModel()->rowOf("movable.txt"), -1);
        QTRY_VERIFY(rightModel()->rowOf("movable.txt") >= 0);
    }

    void conflictPromptSkip()
    {
        moveCursorTo(m_left, "file1.txt");
        answerNextDialog([](QWidget* w) {
            acceptDialog(w);  // the destination prompt
            answerNextDialog([](QWidget* prompt) {
                for (auto* b : prompt->findChildren<QPushButton*>())
                    if (b->text() == QObject::tr("&Skip"))
                        b->click();
            });
        });
        writeFile(R("file1.txt"), "keep me");
        QTest::keyClick(m_left->view(), Qt::Key_F5);
        QTest::qWait(500);
        QCOMPARE(QFile(R("file1.txt")).size(), qint64(7));
    }

    void newFolderWithF7()
    {
        answerNextDialog([](QWidget* w) {
            auto* dlg = qobject_cast<QInputDialog*>(w);
            QVERIFY(dlg);
            dlg->setTextValue("made/nested");
            dlg->accept();
        });
        QTest::keyClick(m_left->view(), Qt::Key_F7);
        QTRY_VERIFY(QFileInfo(L("made/nested")).isDir());
        QTRY_COMPARE(m_left->currentName(), QString("made"));  // cursor jumps to the new folder
    }

    void renameInPlace()
    {
        moveCursorTo(m_left, "file2.txt");
        leftModel()->setData(leftModel()->index(m_left->view()->currentRow(), 0), "renamed.txt", Qt::EditRole);
        QTRY_VERIFY(QFile::exists(L("renamed.txt")));
        QTRY_COMPARE(m_left->currentName(), QString("renamed.txt"));
    }

    void deletePermanentlyWithShiftF8()
    {
        moveCursorTo(m_left, "made");
        answerNextDialog(acceptDialog);
        QTest::keyClick(m_left->view(), Qt::Key_F8, Qt::ShiftModifier);
        QTRY_VERIFY(!QFile::exists(L("made")));
        QTRY_COMPARE(leftModel()->rowOf("made"), -1);
    }

    void externalChangesShowUp()
    {
        writeFile(L("from-outside.txt"), "hello");
        QTRY_VERIFY(leftModel()->rowOf("from-outside.txt") >= 0);
        QFile::remove(L("from-outside.txt"));
        QTRY_COMPARE(leftModel()->rowOf("from-outside.txt"), -1);
    }

    void tabs()
    {
        moveCursorTo(m_left, "sub");
        QTest::keyClick(m_left->view(), Qt::Key_T, Qt::ControlModifier);
        QTRY_COMPARE(m_left->tabPaths(), QStringList({L(), L("sub")}));
        QTRY_COMPARE(m_left->path(), L("sub"));
        QTest::keyClick(m_left->view(), Qt::Key_W, Qt::ControlModifier);
        QTRY_COMPARE(m_left->tabPaths(), QStringList({L()}));
    }

    void restoredTabsKeepOrder()
    {
        Panel p;
        p.restoreTabs({L(), L("sub"), R()}, 1);
        QCOMPARE(p.tabPaths(), QStringList({L(), L("sub"), R()}));
        QCOMPARE(p.currentTabIndex(), 1);
        QTRY_COMPARE(p.path(), L("sub"));
    }

    void historyRemembersCursorAndMarks()
    {
        moveCursorTo(m_left, "photo.jpg");
        leftModel()->setMarked(leftModel()->rowOf("file1.txt"), true);
        moveCursorTo(m_left, "sub");
        moveCursorTo(m_left, "photo.jpg");
        m_left->navigate(L("sub"));
        QTRY_COMPARE(leftModel()->path(), L("sub"));
        moveCursorTo(m_left, "inner.txt");
        QTest::keyClick(m_left->view(), Qt::Key_Left, Qt::AltModifier);
        QTRY_COMPARE(leftModel()->path(), L());
        QCOMPARE(m_left->currentName(), QString("photo.jpg"));
        QCOMPARE(leftModel()->markedNames(), QStringList({"file1.txt"}));
        QTest::keyClick(m_left->view(), Qt::Key_Right, Qt::AltModifier);
        QTRY_COMPARE(leftModel()->path(), L("sub"));
        QCOMPARE(m_left->currentName(), QString("inner.txt"));
        QTest::keyClick(m_left->view(), Qt::Key_Backspace);
        QTRY_COMPARE(leftModel()->path(), L());
        QCOMPARE(leftModel()->markedNames(), QStringList({"file1.txt"}));
        leftModel()->markAll(false);
    }

    void contextMenuActsOnClickedItemOnly()
    {
        leftModel()->setMarked(leftModel()->rowOf("file1.txt"), true);
        leftModel()->setMarked(leftModel()->rowOf("file10.txt"), true);
        chooseFromNextMenu(QObject::tr("Copy full paths as text"));
        rightClick(m_left->view(), leftModel()->rowOf("photo.jpg"));
        QCOMPARE(QApplication::clipboard()->text(), L("photo.jpg"));
        QCOMPARE(leftModel()->markedNames().size(), 2);  // marks survive

        chooseFromNextMenu(QObject::tr("Copy full paths as text"));
        rightClick(m_left->view(), leftModel()->rowOf("file1.txt"));  // marked: acts on all marks
        QCOMPARE(QApplication::clipboard()->text(), L("file1.txt") + "\n" + L("file10.txt"));
        leftModel()->markAll(false);
    }

    void contextMenuNewFileOnEmptyArea()
    {
        answerNextDialog([](QWidget* w) {
            auto* dlg = qobject_cast<QInputDialog*>(w);
            QVERIFY(dlg);
            dlg->setTextValue("blank.txt");
            dlg->accept();
        });
        chooseFromNextMenu(QObject::tr("New file…"));
        const QPoint empty(20, m_left->view()->viewport()->height() - 5);
        QContextMenuEvent ev(QContextMenuEvent::Mouse, empty, m_left->view()->viewport()->mapToGlobal(empty));
        QApplication::sendEvent(m_left->view()->viewport(), &ev);
        QTRY_VERIFY(QFile::exists(L("blank.txt")));
        QTRY_COMPARE(m_left->currentName(), QString("blank.txt"));
    }

    void switchPanelsWithTab()
    {
        QTest::keyClick(m_left->view(), Qt::Key_Tab);
        QTRY_VERIFY(m_right->isActive());
        QVERIFY(!m_left->isActive());
        QTest::keyClick(m_right->view(), Qt::Key_Tab);
        QTRY_VERIFY(m_left->isActive());
    }

    void sortBySize()
    {
        QTest::keyClick(m_left->view(), Qt::Key_F6, Qt::ControlModifier);
        QCOMPARE(m_left->sortSpec().column, SortColumn::Size);
        // Biggest file first, folders stay on top.
        int firstFile = -1;
        for (int r = 0; r < leftModel()->count(); ++r)
            if (!leftModel()->entry(r)->isDir) {
                firstFile = r;
                break;
            }
        QCOMPARE(leftModel()->entry(firstFile)->name, QString("photo.jpg"));
        QTest::keyClick(m_left->view(), Qt::Key_F3, Qt::ControlModifier);
        QCOMPARE(m_left->sortSpec().column, SortColumn::Name);
    }

    void missingFolderShowsError()
    {
        m_left->navigate(L("does-not-exist"));
        QTest::qWait(200);
        QCOMPARE(leftModel()->path(), L());  // stays where it was
    }

    void iconThemes()
    {
        const QString base = m_tmp.filePath("icons");
        QVERIFY(QDir().mkpath(base + "/Tandem Test/places/16"));
        QVERIFY(QDir().mkpath(base + "/SomeCursors/cursors"));
        writeFile(base + "/Tandem Test/index.theme",
                  "[Icon Theme]\nName=Tandem Test\nDirectories=places/16\n\n[places/16]\nSize=16\nType=Fixed\n");
        writeFile(base + "/SomeCursors/index.theme", "[Icon Theme]\nName=Some Cursors\n");
        QImage img(16, 16, QImage::Format_ARGB32);
        img.fill(Qt::red);
        QVERIFY(img.save(base + "/Tandem Test/places/16/folder.png"));

        QIcon::setThemeSearchPaths(QIcon::themeSearchPaths() << base);
        const QList<IconTheme> themes = scanIconThemes();
        const auto has = [&](const QString& id) {
            return std::any_of(themes.begin(), themes.end(), [&](const IconTheme& t) { return t.id == id; });
        };
        QVERIFY(has("Tandem Test"));
        QVERIFY(!has("SomeCursors"));  // cursor-only theme

        const QString before = QIcon::themeName();
        IconCache::setTheme("Tandem Test");
        QCOMPARE(QIcon::themeName(), QString("Tandem Test"));
        FileEntry dir;
        dir.isDir = true;
        const QImage got = IconCache::forEntry(dir).pixmap(16).toImage();
        QCOMPARE(got.pixelColor(8, 8), QColor(Qt::red));
        IconCache::setTheme({});  // back to the system theme
        QCOMPARE(QIcon::themeName(), before);
    }

    void archives()
    {
        // Pack with Alt+F5 into the right panel.
        moveCursorTo(m_left, "sub");
        answerNextDialog([this](QWidget* w) {
            auto* edit = w->findChild<QLineEdit*>();
            QVERIFY(edit);
            edit->setText(R("pack.zip"));
            acceptDialog(w);
        });
        QTest::keyClick(m_left->view(), Qt::Key_F5, Qt::AltModifier);
        QTRY_VERIFY(QFileInfo(R("pack.zip")).size() > 0);
        QTRY_VERIFY(rightModel()->rowOf("pack.zip") >= 0);

        // Enter it like a folder, then a folder inside it.
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_right->view()->setFocus();
        QTRY_VERIFY(m_right->isActive());
        moveCursorTo(m_right, "pack.zip");
        QTest::keyClick(m_right->view(), Qt::Key_Return);
        QTRY_COMPARE(rightModel()->path(), R("pack.zip"));
        QVERIFY(m_right->inArchive());
        QVERIFY(rightModel()->rowOf("sub") >= 0);
        moveCursorTo(m_right, "sub");
        QTest::keyClick(m_right->view(), Qt::Key_Return);
        QTRY_COMPARE(rightModel()->path(), R("pack.zip/sub"));
        QVERIFY(rightModel()->rowOf("inner.txt") >= 0);

        // F3 views a member (extracted to the cache behind the scenes).
        moveCursorTo(m_right, "inner.txt");
        QTest::keyClick(m_right->view(), Qt::Key_F3);
        ViewerWindow* viewer = nullptr;
        QTRY_VERIFY((viewer = m_window->findChild<ViewerWindow*>()) != nullptr);
        QTRY_COMPARE(viewer->textEdit()->toPlainText(), QString("inner"));
        viewer->close();
        QTRY_VERIFY(m_window->findChild<ViewerWindow*>() == nullptr);
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_right->view()->setFocus();

        // Read-only: delete is refused, nothing disappears.
        QTest::keyClick(m_right->view(), Qt::Key_F8);
        QTest::qWait(100);
        QVERIFY(QApplication::activeModalWidget() == nullptr);
        QVERIFY(rightModel()->rowOf("inner.txt") >= 0);

        // F5 extracts into the other panel.
        QVERIFY(QDir().mkpath(L("unpacked")));
        m_left->navigate(L("unpacked"));
        QTRY_COMPARE(leftModel()->path(), L("unpacked"));
        answerNextDialog(acceptDialog);
        QTest::keyClick(m_right->view(), Qt::Key_F5);
        QTRY_VERIFY(QFile::exists(L("unpacked/inner.txt")));
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_right->view()->setFocus();

        // Backspace climbs out of the archive and lands on it.
        QTest::keyClick(m_right->view(), Qt::Key_Backspace);
        QTRY_COMPARE(rightModel()->path(), R("pack.zip"));
        QTest::keyClick(m_right->view(), Qt::Key_Backspace);
        QTRY_COMPARE(rightModel()->path(), R());
        QCOMPARE(m_right->currentName(), QString("pack.zip"));
        QVERIFY(!m_right->inArchive());

        // Alt+F9 unpacks the whole archive into the other panel.
        answerNextDialog(acceptDialog);
        QTest::keyClick(m_right->view(), Qt::Key_F9, Qt::AltModifier);
        QTRY_VERIFY(QFile::exists(L("unpacked/sub/deep")));  // single top folder: no extra level

        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
        m_left->view()->setFocus();
    }

    void multiRenameAndUndo()
    {
        QVERIFY(QDir().mkpath(L("mr")));
        for (const char* n : {"b.txt", "a.txt", "c.txt", "keep.md"})
            writeFile(L(QString("mr/") + n), n);
        m_left->navigate(L("mr"));
        QTRY_COMPARE(leftModel()->path(), L("mr"));
        QVERIFY(leftModel()->markByPattern("*.txt", true) == 3);

        answerNextDialog([](QWidget* w) {
            auto* dlg = qobject_cast<MultiRenameDialog*>(w);
            QVERIFY(dlg);
            dlg->nameMaskEdit()->setText("img_[C]");
            QTRY_VERIFY(dlg->renameButton()->isEnabled());
            dlg->renameButton()->click();
        });
        QTest::keyClick(m_left->view(), Qt::Key_M, Qt::ControlModifier);
        QTRY_VERIFY(QFile::exists(L("mr/img_1.txt")));
        QCOMPARE(readName(L("mr/img_1.txt")), QByteArray("a.txt"));  // listing order: a, b, c
        QCOMPARE(readName(L("mr/img_3.txt")), QByteArray("c.txt"));
        QVERIFY(QFile::exists(L("mr/keep.md")));
        QTRY_VERIFY(leftModel()->rowOf("img_2.txt") >= 0);

        // A mask producing a clash keeps the Rename button disabled.
        m_window->activateWindow();  // offscreen: the window stays inactive after a dialog
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        leftModel()->markByPattern("*", true);
        auto clashChecked = std::make_shared<bool>(false);
        answerNextDialog([clashChecked](QWidget* w) {
            auto* dlg = qobject_cast<MultiRenameDialog*>(w);
            QVERIFY(dlg);
            dlg->nameMaskEdit()->setText("same");
            QTest::qWait(100);
            *clashChecked = !dlg->renameButton()->isEnabled();
            dlg->reject();
        });
        QTest::keyClick(m_left->view(), Qt::Key_M, Qt::ControlModifier);
        QTRY_VERIFY(*clashChecked);
        QTRY_VERIFY(QApplication::activeModalWidget() == nullptr);
        leftModel()->markAll(false);

        // Undo restores the old names.
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        QTest::keyClick(m_left->view(), Qt::Key_Z, Qt::ControlModifier);
        QTRY_VERIFY(QFile::exists(L("mr/a.txt")));
        QVERIFY(QFile::exists(L("mr/c.txt")));
        QVERIFY(!QFile::exists(L("mr/img_1.txt")));

        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    void renameInTextEditor()
    {
        QVERIFY(QDir().mkpath(L("ed")));
        for (const char* n : {"one.txt", "two.txt", "three.log"})
            writeFile(L(QString("ed/") + n), n);
        m_left->navigate(L("ed"));
        QTRY_COMPARE(leftModel()->path(), L("ed"));
        auto useEditor = [](const QString& cmd) { QSettings().setValue("tools/nameEditor", cmd); };

        // The "editor" swaps .txt for .md on every line.
        useEditor("sed -i s/\\.txt$/.md/ %f");
        leftModel()->markByPattern("*", true);
        answerNextDialog([](QWidget* w) {
            auto* dlg = qobject_cast<RenameConfirmDialog*>(w);
            QVERIFY(dlg);
            QVERIFY(dlg->renameButton()->isEnabled());
            dlg->renameButton()->click();
        });
        QTest::keyClick(m_left->view(), Qt::Key_E, Qt::ControlModifier);
        QTRY_VERIFY(QFile::exists(L("ed/one.md")));
        QVERIFY(QFile::exists(L("ed/two.md")));
        QVERIFY(QFile::exists(L("ed/three.log")));
        QTRY_VERIFY(leftModel()->rowOf("one.md") >= 0);

        // A deleted line: warn, rename nothing.
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        useEditor("sed -i 1d %f");
        leftModel()->markByPattern("*", true);
        answerNextDialog([](QWidget* w) {
            auto* box = qobject_cast<QMessageBox*>(w);
            QVERIFY(box);
            QVERIFY(box->text().contains("Expected 3 lines"));
            box->button(QMessageBox::Cancel)->click();
        });
        QTest::keyClick(m_left->view(), Qt::Key_E, Qt::ControlModifier);
        QTRY_VERIFY(QApplication::activeModalWidget() == nullptr);
        QTest::qWait(200);
        QVERIFY(QFile::exists(L("ed/one.md")));

        // Nothing changed: no dialog at all.
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        useEditor("true");
        QTest::keyClick(m_left->view(), Qt::Key_E, Qt::ControlModifier);
        QTest::qWait(400);
        QVERIFY(QApplication::activeModalWidget() == nullptr);

        QSettings().remove("tools/nameEditor");
        leftModel()->markAll(false);
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    void commandPaletteAndJump()
    {
        auto* popup = m_window->findChild<FuzzyPopup*>();
        QVERIFY(popup);

        // Palette: run "Show hidden files" by typing a few letters.
        QAction* hidden = nullptr;
        for (QAction* a : m_window->actions())
            if (a->text() == QObject::tr("Show hidden files"))
                hidden = a;
        QVERIFY(hidden);
        const bool before = hidden->isChecked();
        QTest::keyClick(m_left->view(), Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(popup->isVisible());
        QTest::keyClicks(QApplication::focusWidget(), "shw hid");
        QTRY_COMPARE(popup->currentItem()->title.startsWith(QObject::tr("Show hidden files")), true);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
        QTRY_VERIFY(!popup->isVisible());
        QCOMPARE(hidden->isChecked(), !before);
        hidden->trigger();  // back to how it was
        QTRY_COMPARE(hidden->isChecked(), before);

        // Jump: visited folders are offered; Enter goes there.
        m_left->navigate(L("sub/deep"));
        QTRY_COMPARE(leftModel()->path(), L("sub/deep"));
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
        m_left->view()->setFocus();
        QTest::keyClick(m_left->view(), Qt::Key_J, Qt::ControlModifier);
        QTRY_VERIFY(popup->isVisible());
        QTest::keyClicks(QApplication::focusWidget(), "sub dee");
        QTRY_VERIFY(popup->currentItem() && popup->currentItem()->key == L("sub/deep"));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_P, Qt::ControlModifier);  // pin it
        QVERIFY(FolderHistory::instance().isPinned(L("sub/deep")));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
        QTRY_COMPARE(leftModel()->path(), L("sub/deep"));
        FolderHistory::instance().setPinned(L("sub/deep"), false);
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    void undoWithCtrlZ()
    {
        qputenv("XDG_DATA_HOME", QFile::encodeName(m_tmp.filePath("data")));  // private trash
        QVERIFY(QDir().mkpath(m_tmp.filePath("data")));
        QVERIFY(QDir().mkpath(L("u")));
        writeFile(L("u/doc.txt"), "doc");
        m_left->navigate(L("u"));
        QTRY_COMPARE(leftModel()->path(), L("u"));
        auto focusLeft = [this] {
            m_window->activateWindow();
            QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
            m_left->view()->setFocus();
        };

        // Move to the right panel, then undo: it comes back.
        moveCursorTo(m_left, "doc.txt");
        answerNextDialog(acceptDialog);
        QTest::keyClick(m_left->view(), Qt::Key_F6);
        QTRY_VERIFY(QFile::exists(R("doc.txt")));
        QTRY_VERIFY(!QFile::exists(L("u/doc.txt")));
        focusLeft();
        QTest::qWait(100);
        QTest::keyClick(m_left->view(), Qt::Key_Z, Qt::ControlModifier);
        QTRY_VERIFY(QFile::exists(L("u/doc.txt")));
        QTRY_VERIFY(!QFile::exists(R("doc.txt")));

        // Trash, then undo: restored.
        QTRY_VERIFY(leftModel()->rowOf("doc.txt") >= 0);
        focusLeft();
        moveCursorTo(m_left, "doc.txt");
        answerNextDialog(acceptDialog);
        QTest::keyClick(m_left->view(), Qt::Key_F8);
        QTRY_VERIFY(!QFile::exists(L("u/doc.txt")));
        focusLeft();
        QTest::qWait(100);
        QTest::keyClick(m_left->view(), Qt::Key_Z, Qt::ControlModifier);
        QTRY_VERIFY(QFile::exists(L("u/doc.txt")));
        QCOMPARE(readName(L("u/doc.txt")), QByteArray("doc"));

        // New folder, then undo: gone (it's empty).
        focusLeft();
        answerNextDialog([](QWidget* w) {
            auto* dlg = qobject_cast<QInputDialog*>(w);
            QVERIFY(dlg);
            dlg->setTextValue("fresh/inner");
            dlg->accept();
        });
        QTest::keyClick(m_left->view(), Qt::Key_F7);
        QTRY_VERIFY(QFileInfo(L("u/fresh/inner")).isDir());
        focusLeft();
        QTest::keyClick(m_left->view(), Qt::Key_Z, Qt::ControlModifier);
        QTRY_VERIFY(!QFile::exists(L("u/fresh")));

        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    void cleanupTestCase()
    {
        m_window->close();
        m_window.reset();
    }
};

QTEST_MAIN(TestUi)
#include "test_ui.moc"
