// Drives the real main window with keystrokes (run with QT_QPA_PLATFORM=offscreen).

#include "core/Archive.h"
#include "core/DirWatcher.h"
#include "core/FolderHistory.h"
#include "ui/DirTab.h"
#include "ui/FileModel.h"
#include "ui/CommandBar.h"
#include "ui/SyncDialog.h"
#include "ui/Network.h"
#include "ui/PathBar.h"
#include "core/Backend.h"
#include "core/Keyring.h"
#include "core/Remote.h"
#include "TestBackend.h"
#include "SshServer.h"
#include "ui/AuthDialogs.h"
#include "ui/viewer/ImageView.h"
#include "ui/viewer/ViewerWindow.h"
#include "ui/ChecksumDialog.h"
#include "ui/DiskUsageDialog.h"
#include "ui/DuplicatesDialog.h"
#include "ui/FileView.h"
#include "ui/ThumbGrid.h"
#include "ui/FuzzyPopup.h"
#include "ui/KeyChordPopup.h"
#include "ui/IconCache.h"
#include "ui/MainWindow.h"
#include "ui/MultiRenameDialog.h"
#include "ui/PermissionsDialog.h"
#include "ui/SettingsDialog.h"
#include "ui/ShortcutsDialog.h"
#include "ui/TrashDialog.h"
#include "ui/Panel.h"
#include "ui/PathBar.h"
#include "ui/Theme.h"
#include "ui/viewer/ImageView.h"
#include "ui/viewer/ViewerWindow.h"

#include <QPlainTextEdit>

#include <sys/stat.h>
#include <QSignalSpy>

#include <QApplication>
#include <QLabel>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QTabBar>
#include <QToolButton>
#include <QSettings>
#include <QImage>
#include <QImageReader>
#include <QCheckBox>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QListWidget>
#include <QKeySequenceEdit>
#include <QTreeWidget>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QMenu>
#include <QDialog>
#include <QLineEdit>
#include <thread>
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
QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<missing>");
}

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
        setKeyringHooksForTests({});  // never the real keyring
        qputenv("XDG_CONFIG_HOME", QFile::encodeName(m_tmp.filePath("config")));
        applyTheme(*qApp);
        DirWatcher::instance();
        registerBuiltinBackends();
        installAuthDialogs();
        FolderHistory::instance().setStoragePath(m_tmp.filePath("folders.tsv"));
        QSettings().clear();  // test mode keeps settings in ~/.qttest across runs: start clean

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

    void lynxFolderNavigation()
    {
        const QVariant previous = QSettings().value("keys/lynxNavigation");
        auto restore = qScopeGuard([&] {
            if (previous.isValid()) QSettings().setValue("keys/lynxNavigation", previous);
            else QSettings().remove("keys/lynxNavigation");
        });
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
        moveCursorTo(m_left, "sub");
        QTest::keyClick(m_left->view(), Qt::Key_Right);
        QTRY_COMPARE(leftModel()->path(), L("sub"));
        QTest::keyClick(m_left->view(), Qt::Key_Left);
        QTRY_COMPARE(leftModel()->path(), L());
        QTRY_COMPARE(m_left->currentName(), QString("sub"));
        moveCursorTo(m_left, "file1.txt");
        QSignalSpy opened(m_left->view(), &FileView::openRequested);
        QTest::keyClick(m_left->view(), Qt::Key_Right);
        QCOMPARE(opened.size(), 0); // never launch the default application
        QCOMPARE(leftModel()->path(), L());
        SettingsDialog settings(m_window.get());
        auto* lynx = settings.findChild<QCheckBox*>("lynxNavigation");
        QVERIFY(lynx);
        lynx->setChecked(false);
        moveCursorTo(m_left, "sub");
        QTest::keyClick(m_left->view(), Qt::Key_Right);
        QCOMPARE(leftModel()->path(), L());
        QCOMPARE(opened.size(), 0);
        lynx->setChecked(true);
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
        // Numbered titles; Alt+number switches.
        auto* bar = m_left->findChild<QTabBar*>();
        QVERIFY(bar);
        QCOMPARE(bar->tabText(0).left(1), QString("1"));
        QCOMPARE(bar->tabText(1), QString("2  sub"));
        QTest::keyClick(m_left->view(), Qt::Key_1, Qt::AltModifier);
        QTRY_COMPARE(m_left->path(), L());
        QTest::keyClick(m_left->view(), Qt::Key_2, Qt::AltModifier);
        QTRY_COMPARE(m_left->path(), L("sub"));

        // Alt+5 creates tab 5 here; numbers stay put and sorted.
        QTest::keyClick(m_left->view(), Qt::Key_5, Qt::AltModifier);
        QTRY_COMPARE(m_left->tabSlots(), QList<int>({1, 2, 5}));
        QCOMPARE(m_left->path(), L("sub"));
        QCOMPARE(bar->tabText(2), QString("5  sub"));
        QTest::keyClick(m_left->view(), Qt::Key_2, Qt::AltModifier);
        QTest::keyClick(m_left->view(), Qt::Key_W, Qt::ControlModifier);  // close tab 2
        QTRY_COMPARE(m_left->tabSlots(), QList<int>({1, 5}));
        QTest::keyClick(m_left->view(), Qt::Key_T, Qt::ControlModifier);   // takes the free 2
        QTRY_COMPARE(m_left->tabSlots(), QList<int>({1, 2, 5}));
        for (int n : {5, 2}) {
            m_left->selectSlot(n);
            QTest::keyClick(m_left->view(), Qt::Key_W, Qt::ControlModifier);
        }
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

        // Archives are writable: F5 from outside adds, F7 makes a folder,
        // rename and F8 change members. Each is one rewrite of the archive.
        writeFile(L("unpacked/added.txt"), "added");
        QTRY_VERIFY(leftModel()->rowOf("added.txt") >= 0);
        m_left->view()->setFocus();
        moveCursorTo(m_left, "added.txt");
        answerNextDialog(acceptDialog);
        QTest::keyClick(m_left->view(), Qt::Key_F5);
        QTRY_VERIFY(rightModel()->rowOf("added.txt") >= 0);
        QVERIFY(QFile::exists(L("unpacked/added.txt")));  // copied, not moved
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_right->view()->setFocus();
        QTRY_VERIFY(m_right->isActive());

        answerNextDialog([](QWidget* w) {
            w->findChild<QLineEdit*>()->setText("made");
            acceptDialog(w);
        });
        QTest::keyClick(m_right->view(), Qt::Key_F7);
        QTRY_VERIFY(rightModel()->rowOf("made") >= 0);

        m_right->doRename("added.txt", "renamed.txt");
        QTRY_VERIFY(rightModel()->rowOf("renamed.txt") >= 0);
        QVERIFY(rightModel()->rowOf("added.txt") < 0);

        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_right->view()->setFocus();
        moveCursorTo(m_right, "inner.txt");
        answerNextDialog(acceptDialog);
        QTest::keyClick(m_right->view(), Qt::Key_F8);
        QTRY_VERIFY(rightModel()->rowOf("inner.txt") < 0);
        QString err;
        QCOMPARE(readFile(extractToCache(R("pack.zip"), "sub/renamed.txt", &err)), QByteArray("added"));
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_right->view()->setFocus();

        // F4 edits a copy; every save goes back into the archive.
        const QString editorScript = m_tmp.filePath("archive-editor.sh");
        writeFile(editorScript, "#!/bin/sh\nprintf 'edited in zip' > \"$1\"\n");
        QFile::setPermissions(editorScript, QFile::permissions(editorScript) | QFile::ExeOwner);
        QSettings().setValue("tools/editor", editorScript);
        moveCursorTo(m_right, "renamed.txt");
        QTest::keyClick(m_right->view(), Qt::Key_F4);
        QTRY_COMPARE_WITH_TIMEOUT(readFile(extractToCache(R("pack.zip"), "sub/renamed.txt", &err)),
                                  QByteArray("edited in zip"), 10000);
        QSettings().remove("tools/editor");
        QTRY_VERIFY(rightModel()->rowOf("renamed.txt") >= 0);
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
            // Typing "{" offers the fields; Tab takes the highlighted one.
            dlg->templateEdit()->clear();
            dlg->templateEdit()->setFocus();
            QTest::keyClicks(dlg->templateEdit(), "img_{n");
            QTRY_VERIFY(dlg->completionPopup()->isVisible());
            QVERIFY(dlg->completionPopup()->currentItem()->text().startsWith("name"));
            QTest::keyClick(dlg->templateEdit(), Qt::Key_Escape);  // closes the list, not the dialog
            QVERIFY(!dlg->completionPopup()->isVisible());
            QVERIFY(dlg->isVisible());
            QTest::keyClicks(dlg->templateEdit(), ":");
            QTRY_VERIFY(dlg->completionPopup()->isVisible());  // counter styles
            dlg->templateEdit()->setText("img_{n}{e");
            dlg->templateEdit()->setCursorPosition(dlg->templateEdit()->text().size());
            dlg->templateEdit()->setFocus();
            QTest::keyClicks(dlg->templateEdit(), "x");
            QTRY_VERIFY(dlg->completionPopup()->isVisible());
            QTest::keyClick(dlg->templateEdit(), Qt::Key_Tab);
            QCOMPARE(dlg->templateEdit()->text(), QString("img_{n}{ext}"));
            QTRY_VERIFY(dlg->renameButton()->isEnabled());
            QCOMPARE(dlg->renameButton()->text(), QString("Rename 3"));
            dlg->renameButton()->click();
        });
        QTest::keyClick(m_left->view(), Qt::Key_M, Qt::ControlModifier);
        QTRY_VERIFY(QFile::exists(L("mr/img_1.txt")));
        QCOMPARE(readName(L("mr/img_1.txt")), QByteArray("a.txt"));  // listing order: a, b, c
        QTRY_COMPARE(readName(L("mr/img_3.txt")), QByteArray("c.txt"));
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
            dlg->templateEdit()->setText("same");
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
        useEditor("sed -i.bak s/\\.txt$/.md/ %f");
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
        useEditor("sed -i.bak 1d %f");
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

    void filterAsYouType()
    {
        QVERIFY(QDir().mkpath(L("flt/inside")));
        for (const char* n : {"alpha.txt", "beta.txt", "gamma.jpg", "alpine.jpg"})
            writeFile(L(QString("flt/") + n), n);
        m_left->navigate(L("flt"));
        QTRY_COMPARE(leftModel()->path(), L("flt"));
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();

        auto visible = [this] {
            QStringList out;
            for (int r = 0; r < leftModel()->count(); ++r)
                if (!leftModel()->entry(r)->isParent)
                    out << leftModel()->entry(r)->name;
            return out;
        };
        QTest::keyClick(m_left->view(), Qt::Key_F, Qt::ControlModifier);
        QTRY_VERIFY(qobject_cast<QLineEdit*>(QApplication::focusWidget()));
        QTest::keyClicks(QApplication::focusWidget(), "al");
        QTRY_COMPARE(visible(), QStringList({"alpha.txt", "alpine.jpg"}));
        QTest::keyClicks(QApplication::focusWidget(), " *.jpg");
        QTRY_COMPARE(visible(), QStringList({"alpine.jpg"}));
        QCOMPARE(m_left->currentName(), QString("alpine.jpg"));

        // Live updates respect the filter.
        writeFile(L("flt/also.jpg"), "x");     // "al" + jpg: shows up
        writeFile(L("flt/other.jpg"), "x");    // hidden
        QTRY_VERIFY(leftModel()->rowOf("also.jpg") >= 0);
        QTest::qWait(300);
        QCOMPARE(leftModel()->rowOf("other.jpg"), -1);

        // Esc brings everything back.
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
        QTRY_VERIFY(leftModel()->rowOf("other.jpg") >= 0);
        QCOMPARE(leftModel()->rowOf("beta.txt") >= 0, true);

        // A new folder starts unfiltered.
        m_left->view()->setFocus();
        QTest::keyClick(m_left->view(), Qt::Key_F, Qt::ControlModifier);
        QTest::keyClicks(QApplication::focusWidget(), "inside");
        QTRY_COMPARE(visible(), QStringList({"inside"}));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
        moveCursorTo(m_left, "inside");
        QTest::keyClick(m_left->view(), Qt::Key_Return);
        QTRY_COMPARE(leftModel()->path(), L("flt/inside"));
        QCOMPARE(leftModel()->filter(), QString());
        QTest::keyClick(m_left->view(), Qt::Key_Backspace);
        QTRY_COMPARE(leftModel()->path(), L("flt"));
        QVERIFY(leftModel()->rowOf("beta.txt") >= 0);

        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    // Alt+A: the permissions editor, with the octal field and extra people.
    void permissionsEditor()
    {
        QVERIFY(QDir().mkpath(L("pe")));
        writeFile(L("pe/doc.txt"), "x");
        ::chmod(QFile::encodeName(L("pe/doc.txt")).constData(), 0600);
        m_left->navigate(L("pe"));
        QTRY_COMPARE(leftModel()->path(), L("pe"));
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        moveCursorTo(m_left, "doc.txt");
        const bool acl = readPermissions(L("pe/doc.txt")).aclSupported;
        auto checked = std::make_shared<bool>(false);
        answerNextDialog([acl, checked](QWidget* w) {
            auto* dlg = qobject_cast<PermissionsDialog*>(w);
            QVERIFY(dlg);
            QCOMPARE(dlg->octalEdit()->text(), QString("600"));
            QVERIFY(!dlg->applyButton()->isEnabled());  // nothing changed yet
            dlg->bitBox(1, 0)->click();                  // group may read
            QCOMPARE(dlg->octalEdit()->text(), QString("640"));
            dlg->octalEdit()->setText("644");            // typed as a number
            emit dlg->octalEdit()->textEdited("644");
            QVERIFY(dlg->bitBox(2, 0)->isChecked());
            if (acl) {
                dlg->addEdit()->setText("nobody");
                QTest::keyClick(dlg->addEdit(), Qt::Key_Return);
            }
            QVERIFY(dlg->applyButton()->isEnabled());
            *checked = true;
            dlg->applyButton()->click();
        });
        QTest::keyClick(m_left->view(), Qt::Key_A, Qt::AltModifier);
        QTRY_VERIFY(*checked);
        QTRY_COMPARE(readPermissions(L("pe/doc.txt")).mode, 0644u);
        if (acl) {
            QTRY_COMPARE(readPermissions(L("pe/doc.txt")).acl.size(), 1);
            QCOMPARE(readPermissions(L("pe/doc.txt")).acl[0].perms, uint32_t(PermRead));
        }
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    // Alt+Shift+C: compute and save checksums, then check them.
    void checksumRoundTrip()
    {
        QVERIFY(QDir().mkpath(L("ck")));
        writeFile(L("ck/one.iso"), "first");
        writeFile(L("ck/two.iso"), "second");
        writeFile(L("ck/three.iso"), "third");
        QString saved;
        {
            ChecksumDialog make({L("ck/one.iso"), L("ck/two.iso"), L("ck/three.iso")}, m_window.get());
            make.show();
            QVERIFY(!make.isVerifying());
            QTRY_VERIFY(!make.isBusy());
            QCOMPARE(make.list()->topLevelItem(0)->text(1).size(), 64);  // SHA-256 by default
            make.saveOne();
            saved = make.saved();
        }
        QCOMPARE(saved, L("ck/ck.sha256"));
        writeFile(L("ck/two.iso"), "changed!");
        QFile::remove(L("ck/three.iso"));
        ChecksumDialog check({saved}, m_window.get());
        check.show();
        QVERIFY(check.isVerifying());
        QTRY_VERIFY(!check.isBusy());
        QCOMPARE(check.summary(), QObject::tr("%1 OK · %2 different · %3 missing").arg(1).arg(1).arg(1));
    }

    // Ctrl+Shift+U: disk usage, browse down and up, delete with sizes updating.
    void diskUsageWindow()
    {
        QVERIFY(QDir().mkpath(L("du/videos")));
        QVERIFY(QDir().mkpath(L("du/docs")));
        writeFile(L("du/videos/film.mkv"), QByteArray(3 << 20, 'v'));
        writeFile(L("du/videos/clip.mp4"), QByteArray(1 << 20, 'c'));
        writeFile(L("du/docs/a.txt"), "a");
        DiskUsageDialog dlg(L("du"), false, m_window.get());
        dlg.show();
        QTRY_VERIFY(!dlg.isCounting());
        QCOMPARE(dlg.list()->topLevelItem(0)->text(0), QString("videos"));  // biggest first
        dlg.list()->setCurrentItem(dlg.list()->topLevelItem(0));
        QTest::keyClick(dlg.list(), Qt::Key_Return);
        QCOMPARE(dlg.current()->name, QString("videos"));
        QCOMPARE(dlg.list()->topLevelItem(0)->text(0), QString("film.mkv"));
        const int64_t before = dlg.current()->parent->size;
        QSignalSpy deleted(&dlg, &DiskUsageDialog::deleteRequested);
        dlg.list()->setCurrentItem(dlg.list()->topLevelItem(0));
        dlg.deleteSelected(false);
        QCOMPARE(deleted.size(), 1);
        QCOMPARE(deleted.first().at(0).toStringList(), QStringList({L("du/videos/film.mkv")}));
        QVERIFY(dlg.current()->parent->size < before);
        QTest::keyClick(dlg.list(), Qt::Key_Backspace);
        QCOMPARE(dlg.current()->name, L("du"));
        QCOMPARE(dlg.list()->currentItem()->text(0), QString("videos"));  // back on the folder we came from
    }

    // Alt+Shift+F7: duplicates, keep one of each, the rest to the trash.
    void duplicateFinder()
    {
        QVERIFY(QDir().mkpath(L("dup/a")));
        QVERIFY(QDir().mkpath(L("dup/b")));
        writeFile(L("dup/a/song.mp3"), QByteArray(200000, 's'));
        writeFile(L("dup/b/song (1).mp3"), QByteArray(200000, 's'));
        writeFile(L("dup/b/song (2).mp3"), QByteArray(200000, 's'));
        writeFile(L("dup/a/unique.txt"), "only one");
        QFile f(L("dup/a/song.mp3"));  // the oldest copy
        QVERIFY(f.open(QIODevice::ReadWrite));
        QVERIFY(f.setFileTime(QDateTime::currentDateTime().addDays(-30), QFileDevice::FileModificationTime));
        f.close();
        m_left->navigate(L("dup"));
        QTRY_COMPARE(leftModel()->path(), L("dup"));
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        auto checked = std::make_shared<bool>(false);
        answerNextDialog([checked](QWidget* w) {
            auto* dlg = qobject_cast<DuplicatesDialog*>(w);
            QVERIFY(dlg);
            dlg->search();
            QTRY_VERIFY(!dlg->isSearching());
            QCOMPARE(dlg->results()->topLevelItemCount(), 1);
            QCOMPARE(dlg->results()->topLevelItem(0)->childCount(), 3);
            dlg->keep(false);  // the oldest stays
            QCOMPARE(dlg->ticked().size(), 2);
            QVERIFY(!dlg->ticked().contains(dlg->results()->topLevelItem(0)->child(0)->data(0, Qt::UserRole).toString()));
            for (auto* b : dlg->findChildren<QPushButton*>())
                if (b->text().startsWith(QObject::tr("Move")))
                    b->click();
            QCOMPARE(dlg->results()->topLevelItemCount(), 0);  // one left: not a duplicate anymore
            *checked = true;
            dlg->accept();
        });
        QTest::keyClick(m_left->view(), Qt::Key_F7, Qt::AltModifier | Qt::ShiftModifier);
        QTRY_VERIFY(*checked);
        QTRY_VERIFY(!QFile::exists(L("dup/b/song (1).mp3")) && !QFile::exists(L("dup/b/song (2).mp3")));
        QVERIFY(QFile::exists(L("dup/a/song.mp3")));
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    // Ctrl+Shift+F1: thumbnails in a grid, remembered for the folder.
    void thumbnailGrid()
    {
        QVERIFY(QDir().mkpath(L("photos/inner")));
        const QList<QColor> colors = {Qt::red, Qt::green, Qt::blue, Qt::yellow, Qt::magenta, Qt::cyan};
        for (int i = 0; i < colors.size(); ++i) {
            QImage img(400, 300, QImage::Format_RGB32);
            img.fill(colors[i]);
            QVERIFY(img.save(L(QString("photos/p%1.jpg").arg(i)), "JPG"));
        }
        writeFile(L("photos/notes.txt"), "text");
        m_left->navigate(L("photos"));
        QTRY_COMPARE(leftModel()->path(), L("photos"));
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        QVERIFY(!m_left->view()->thumbnails());
        QTest::keyClick(m_left->view(), Qt::Key_F1, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(m_left->view()->thumbnails());
        ThumbGrid* grid = m_left->view()->thumbGrid();
        QTRY_VERIFY(grid->isVisible());

        // The pictures show up (made in the background).
        auto colorIn = [&](int row) {
            const QImage shot = grid->grab().toImage();
            const QRect r = grid->cellRect(row);
            return shot.pixelColor(r.left() + 8 + grid->cellSize() / 2, r.top() + 8 + grid->cellSize() / 2);
        };
        const int p2 = leftModel()->rowOf("p2.jpg");
        QTRY_VERIFY_WITH_TIMEOUT(qAbs(colorIn(p2).blue() - 255) < 40 && colorIn(p2).red() < 60, 10000);

        // Arrows: sideways one, up/down a row.
        m_left->view()->setCurrentRow(0);
        QTest::keyClick(m_left->view(), Qt::Key_Right);
        QCOMPARE(m_left->view()->currentRow(), 1);
        QTest::keyClick(m_left->view(), Qt::Key_Down);
        QCOMPARE(m_left->view()->currentRow(), qMin(1 + grid->columns(), leftModel()->count() - 1));

        // Another folder is a list again; coming back brings the grid back.
        moveCursorTo(m_left, "inner");
        QTest::keyClick(m_left->view(), Qt::Key_Return);
        QTRY_COMPARE(leftModel()->path(), L("photos/inner"));
        QTRY_VERIFY(!m_left->view()->thumbnails());
        QTest::keyClick(m_left->view(), Qt::Key_Backspace);
        QTRY_COMPARE(leftModel()->path(), L("photos"));
        QTRY_VERIFY(m_left->view()->thumbnails());
        QTest::keyClick(m_left->view(), Qt::Key_F1, Qt::ControlModifier | Qt::ShiftModifier);  // off again
        QVERIFY(!m_left->view()->thumbnails());
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    void backgroundRefreshKeepsLatestCursorAndMarks()
    {
        QTemporaryDir serverDir;
        writeFile(serverDir.filePath("a.txt"), "a");
        writeFile(serverDir.filePath("b.txt"), "b");
        auto backend = std::make_shared<TestBackend>(serverDir.path());
        registerBackend("test", backend);
        auto unregister = qScopeGuard([] { registerBackend("test", nullptr); });
        m_right->navigate("test://srv/");
        QTRY_COMPARE(rightModel()->path(), QString("test://srv/"));
        moveCursorTo(m_right, "a.txt");
        backend->latencyMs = 250;
        m_right->refresh();
        QVERIFY(m_right->tab()->isLoading());
        moveCursorTo(m_right, "b.txt");
        rightModel()->setMarked(rightModel()->rowOf("b.txt"), true);
        QTRY_VERIFY(!m_right->tab()->isLoading());
        QCOMPARE(m_right->currentName(), QString("b.txt"));
        QVERIFY(rightModel()->markedNames().contains("b.txt"));
        backend->latencyMs = 0;
        m_right->navigate(R());
        QTRY_COMPARE(rightModel()->path(), R());
        QVERIFY(ioPool()->waitForDone(5000));
    }

    // Server folders notice changes made elsewhere (polled while shown).
    void serverFolderRefreshes()
    {
        QTemporaryDir serverDir;
        writeFile(serverDir.filePath("a.txt"), "a");
        writeFile(serverDir.filePath("b.txt"), "b");
        auto backend = std::make_shared<TestBackend>(serverDir.path());
        registerBackend("test", backend);
        m_right->navigate("test://srv/");
        QTRY_COMPARE(rightModel()->path(), QString("test://srv/"));
        QTRY_VERIFY(rightModel()->rowOf("b.txt") >= 0);
        QVERIFY(m_right->tab()->needsPolling());
        m_right->view()->setCurrentRow(rightModel()->rowOf("b.txt"));

        const int before = backend->calls;
        m_right->tab()->poll();  // nothing changed: one listing, no reload
        QTRY_VERIFY(backend->calls > before);
        QTest::qWait(100);
        QCOMPARE(m_right->currentName(), QString("b.txt"));

        writeFile(serverDir.filePath("new-from-elsewhere.txt"), "n");  // someone else
        m_right->tab()->poll();
        QTRY_VERIFY(rightModel()->rowOf("new-from-elsewhere.txt") >= 0);
        QCOMPARE(m_right->currentName(), QString("b.txt"));  // the cursor stays
#ifdef Q_OS_MACOS
        QVERIFY(m_left->tab()->needsPolling()); // catches file content/metadata changes beside kqueue
#else
        QVERIFY(!m_left->tab()->needsPolling()); // local folders have inotify
#endif

        m_right->navigate(R());
        QTRY_COMPARE(rightModel()->path(), R());
        // A poll can still be reading the mock server after navigation completes.
        QVERIFY(ioPool()->waitForDone(5000));
        registerBackend("test", nullptr);
    }

    // The trash window: lists what's there, restores, deletes for good.
    void trashWindow()
    {
#ifdef Q_OS_MACOS
        QSKIP("macOS Trash browsing is delegated to Finder; native trash/undo is tested separately");
#endif
        QVERIFY(QDir().mkpath(L("tw")));
        writeFile(L("tw/old-report.pdf"), "pdf");
        writeFile(L("tw/gone.txt"), "bye");
        QVERIFY(QFile::moveToTrash(L("tw/old-report.pdf")));
        QVERIFY(QFile::moveToTrash(L("tw/gone.txt")));
        TrashDialog dlg(m_window.get());
        dlg.show();
        QTRY_VERIFY(!dlg.isLoading());
        auto rowOf = [&](const QString& name) -> QTreeWidgetItem* {
            for (int i = 0; i < dlg.list()->topLevelItemCount(); ++i)
                if (dlg.list()->topLevelItem(i)->text(0) == name)
                    return dlg.list()->topLevelItem(i);
            return nullptr;
        };
        QTRY_VERIFY(rowOf("old-report.pdf") && rowOf("gone.txt"));
        QCOMPARE(rowOf("old-report.pdf")->text(1), L("tw"));
        dlg.list()->clearSelection();
        rowOf("old-report.pdf")->setSelected(true);
        dlg.restoreSelected();
        QTRY_COMPARE(readFile(L("tw/old-report.pdf")), QByteArray("pdf"));
        QTRY_VERIFY(!dlg.isLoading() && !rowOf("old-report.pdf"));
        rowOf("gone.txt")->setSelected(true);
        dlg.deleteSelected(false);
        QTRY_VERIFY(!dlg.isLoading() && !rowOf("gone.txt"));
        QVERIFY(!QFile::exists(L("tw/gone.txt")));
    }

    // Ctrl+Shift+F5: a link in the other panel; Ctrl+Z takes it away again.
    void createLinks()
    {
        QVERIFY(QDir().mkpath(L("lk")));
        writeFile(L("lk/target.txt"), "t");
        m_left->navigate(L("lk"));
        m_right->navigate(R());
        QTRY_COMPARE(leftModel()->path(), L("lk"));
        QTRY_COMPARE(rightModel()->path(), R());
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        moveCursorTo(m_left, "target.txt");
        answerNextDialog([](QWidget* w) {
            for (auto* b : w->findChildren<QToolButton*>())
                if (b->text() == QObject::tr("Relative"))
                    b->click();
            acceptDialog(w);
        });
        QTest::keyClick(m_left->view(), Qt::Key_F5, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(QFileInfo(R("target.txt")).isSymLink());
        QCOMPARE(readFile(R("target.txt")), QByteArray("t"));
        QVERIFY(!QFileInfo(R("target.txt")).symLinkTarget().isEmpty());
        QTRY_VERIFY(rightModel()->rowOf("target.txt") >= 0);
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        QTest::keyClick(m_left->view(), Qt::Key_Z, Qt::ControlModifier);
        QTRY_VERIFY(!QFileInfo(R("target.txt")).exists() && !QFileInfo(R("target.txt")).isSymLink());
        QCOMPARE(readFile(L("lk/target.txt")), QByteArray("t"));
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    // Ctrl+,: settings; keys can be changed, conflicts resolved, defaults restored.
    void settingsAndKeys()
    {
        SettingsDialog dlg(m_window.get());
        dlg.show();
        dlg.showPage(3);
        auto find = [&](const QString& text) -> QTreeWidgetItem* {
            for (int i = 0; i < dlg.keyList()->topLevelItemCount(); ++i)
                if (dlg.keyList()->topLevelItem(i)->text(0).endsWith(text))
                    return dlg.keyList()->topLevelItem(i);
            return nullptr;
        };
        QAction* copy = nullptr;
        QAction* move = nullptr;
        for (QAction* a : m_window->actions()) {
            if (a->property("commandId").toString() == "copy")
                copy = a;
            if (a->property("commandId").toString() == "move")
                move = a;
        }
        QVERIFY(copy && move);
        QCOMPARE(move->shortcut(), QKeySequence(Qt::Key_F6));
        dlg.keyList()->setCurrentItem(find("Copy…"));
        dlg.keyEdit()->setKeySequence(QKeySequence(Qt::Key_F6));  // Move's key
        QCOMPARE(copy->shortcut(), QKeySequence(Qt::Key_F6));
        QVERIFY(move->shortcuts().isEmpty());  // gave it up
        QVERIFY(dlg.keyNote()->text().contains("Move"));
        QCOMPARE(QSettings().value("keys/copy").toStringList(), QStringList({"F6"}));
        QCOMPARE(QSettings().value("keys/move").toStringList(), QStringList({"none"}));
        // A restart applies them: simulate with the saved values.
        copy->setShortcut(QKeySequence(Qt::Key_F5));
        applyKeyBindings(m_window->actions());
        QCOMPARE(copy->shortcut(), QKeySequence(Qt::Key_F6));
        for (auto* b : dlg.findChildren<QPushButton*>())
            if (b->text() == QObject::tr("Reset all keys"))
                b->click();
        QCOMPARE(copy->shortcut(), QKeySequence(Qt::Key_F5));
        QCOMPARE(move->shortcut(), QKeySequence(Qt::Key_F6));
        QVERIFY(!QSettings().contains("keys/copy") && !QSettings().contains("keys/move"));
    }

    // F1: grouped shortcuts in columns, with a filter.
    void shortcutsDialog()
    {
        ShortcutsDialog dlg(m_window.get());
        dlg.show();
        const int all = dlg.visibleEntries();
        QVERIFY(all > 40);
        dlg.filterEdit()->setText("rename");
        QVERIFY(dlg.visibleEntries() >= 2 && dlg.visibleEntries() < all);
        dlg.filterEdit()->setText("F5");
        QVERIFY(dlg.visibleEntries() >= 1);
        dlg.filterEdit()->setText("zzzz");
        QCOMPARE(dlg.visibleEntries(), 0);
    }

    // The keyboard always ends up back in a file list, and Esc closes things.
    void focusStaysWithTheLists()
    {
        QVERIFY(QDir().mkpath(L("fx")));
        for (const char* n : {"apple.txt", "banana.txt", "cherry.txt"})
            writeFile(L(QString("fx/") + n), n);
        m_left->navigate(L("fx"));
        QTRY_COMPARE(leftModel()->path(), L("fx"));
        m_right->navigate(R());
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        moveCursorTo(m_left, "apple.txt");
        auto focused = [] { return QApplication::focusWidget(); };

        // A click into a preview gives the keys back to that panel's list.
        QSettings().setValue("view/previewPosition", "each");
        QTest::keyClick(m_left->view(), Qt::Key_Q, Qt::ControlModifier);
        ViewerWindow* left = nullptr;
        ViewerWindow* right = nullptr;
        QTRY_VERIFY((left = m_window->findChild<ViewerWindow*>("quickView")) != nullptr);
        QTRY_VERIFY((right = m_window->findChild<ViewerWindow*>("quickViewRight")) != nullptr);
        QTRY_COMPARE(left->textEdit()->toPlainText(), QString("apple.txt"));
        QTest::mouseClick(left->textEdit()->viewport(), Qt::LeftButton, {}, QPoint(20, 10));
        QTRY_COMPARE(focused(), static_cast<QWidget*>(m_left->view()));
        QTest::keyClick(focused(), Qt::Key_Down);
        QCOMPARE(m_left->currentName(), QString("banana.txt"));
        QTest::mouseClick(right, Qt::LeftButton, {}, QPoint(30, 30));  // the other panel's preview
        QTRY_COMPARE(focused(), static_cast<QWidget*>(m_right->view()));
        QTRY_VERIFY(m_right->isActive());
        QTest::keyClick(m_left->view(), Qt::Key_Q, Qt::ControlModifier);
        QTRY_VERIFY(m_window->findChild<ViewerWindow*>("quickView") == nullptr);
        QSettings().setValue("view/preview", false);
        m_left->view()->setFocus();
        QTRY_VERIFY(m_left->isActive());

        // Search bar: ↓ goes to the results, Esc there closes the bar.
        m_left->startSearchBar();
        auto* searchEdit = qobject_cast<QLineEdit*>(focused());
        QVERIFY(searchEdit && searchEdit->isVisible());
        QTest::keyClick(searchEdit, Qt::Key_Down);
        QCOMPARE(focused(), static_cast<QWidget*>(m_left->view()));
        QTest::keyClick(m_left->view(), Qt::Key_Escape);
        QVERIFY(!searchEdit->isVisible());
        QCOMPARE(focused(), static_cast<QWidget*>(m_left->view()));

        // Filter: Enter back to the list, Esc there clears it.
        m_left->startFilter();
        QTest::keyClicks(focused(), "an");
        QTRY_VERIFY(leftModel()->rowOf("apple.txt") < 0);
        QTest::keyClick(focused(), Qt::Key_Return);
        QCOMPARE(focused(), static_cast<QWidget*>(m_left->view()));
        QTest::keyClick(m_left->view(), Qt::Key_Escape);
        QTRY_VERIFY(leftModel()->rowOf("apple.txt") >= 0);
        QCOMPARE(focused(), static_cast<QWidget*>(m_left->view()));

        // Path bar and quick search: Esc returns to the list.
        m_left->focusPathEdit();
        QVERIFY(focused() != m_left->view());
        QTest::keyClick(focused(), Qt::Key_Escape);
        QTRY_COMPARE(focused(), static_cast<QWidget*>(m_left->view()));
        QTest::keyClicks(m_left->view(), "ch");
        QTRY_COMPARE(m_left->currentName(), QString("cherry.txt"));
        QTest::keyClick(focused(), Qt::Key_Escape);
        QTRY_COMPARE(focused(), static_cast<QWidget*>(m_left->view()));
        QTest::keyClick(focused(), Qt::Key_Up);
        QCOMPARE(m_left->currentName(), QString("banana.txt"));

        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    // Alt+F1: places, drives, servers in a drop-down that also takes a typed path.
    void locations()
    {
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        m_left->showLocations();
        FuzzyPopup* popup = nullptr;
        QTRY_VERIFY([&] {
            for (auto* p : m_window->findChildren<FuzzyPopup*>())
                if (p->isVisible())
                    popup = p;
            return popup != nullptr;
        }());
        QCOMPARE(popup->currentItem()->title, QString("Home"));
        auto* edit = popup->findChild<QLineEdit*>();
        QTest::keyClicks(edit, R());
        QTRY_COMPARE(popup->currentItem()->title, QString("Go to %1").arg(R()));
        QTest::keyClick(edit, Qt::Key_Return);
        QTRY_COMPARE(leftModel()->path(), R());
        QVERIFY(!popup->isVisible());
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    void quickView()
    {
        QVERIFY(QDir().mkpath(L("qv")));
        writeFile(L("qv/a.txt"), "first file");
        writeFile(L("qv/b.txt"), "second file");
        m_left->navigate(L("qv"));
        QTRY_COMPARE(leftModel()->path(), L("qv"));
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        moveCursorTo(m_left, "a.txt");
        // Settings live in ~/.qttest in test mode and survive runs: be explicit.
        QSettings().setValue("view/previewPosition", "each");

        QTest::keyClick(m_left->view(), Qt::Key_Q, Qt::ControlModifier);
        ViewerWindow* qv = nullptr;
        QTRY_VERIFY((qv = m_window->findChild<ViewerWindow*>("quickView")) != nullptr);
        QVERIFY(m_right->isVisible());  // the copy/move target stays
        // Default: one preview below each panel, in that panel's column.
        ViewerWindow* qv2 = m_window->findChild<ViewerWindow*>("quickViewRight");
        QVERIFY(qv2);
        QVERIFY(qv->parentWidget() == m_left->parentWidget());
        QVERIFY(qv2->parentWidget() == m_right->parentWidget());
        QVERIFY(qv->mapTo(m_window.get(), QPoint()).y() > m_left->mapTo(m_window.get(), QPoint()).y());
        QTRY_COMPARE(qv->textEdit()->toPlainText(), QString("first file"));
        QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(m_left->view()));  // focus stays

        QTest::keyClick(m_left->view(), Qt::Key_Down);
        QTRY_COMPARE(qv->currentPath(), L("qv/b.txt"));
        QTRY_COMPARE(qv->textEdit()->toPlainText(), QString("second file"));
        QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(m_left->view()));

        // The right preview shows the right panel's cursor; the left one stays.
        m_right->navigate(L("qv"));
        QTRY_COMPARE(rightModel()->path(), L("qv"));
        moveCursorTo(m_right, "a.txt");
        QTRY_COMPARE(qv2->currentPath(), L("qv/a.txt"));
        QCOMPARE(qv->currentPath(), L("qv/b.txt"));
        m_right->navigate(R());

        // One shared preview below both: follows the active panel.
        m_window->setPreviewPosition("bottom");
        QTRY_VERIFY(m_window->findChild<ViewerWindow*>("quickViewRight") == nullptr);
        QTRY_VERIFY(m_window->findChild<ViewerWindow*>("quickView") != nullptr);
        auto* shared = m_window->findChild<ViewerWindow*>("quickView");
        QCOMPARE(qobject_cast<QSplitter*>(shared->parentWidget())->orientation(), Qt::Vertical);
        QVERIFY(shared->parentWidget() != m_left->parentWidget());

        // On the right: a third column.
        m_window->setPreviewPosition("right");
        QTRY_VERIFY(m_window->findChild<ViewerWindow*>("quickView") &&
                    qobject_cast<QSplitter*>(m_window->findChild<ViewerWindow*>("quickView")->parentWidget())
                            ->orientation() == Qt::Horizontal);
        QSettings().setValue("view/previewPosition", "each");

        // Photos with nearly the same names: each shows its own picture.
        QVERIFY(QDir().mkpath(L("pics")));
        const QList<QColor> colors = {Qt::red, Qt::green, Qt::blue, Qt::yellow, Qt::magenta};
        for (int i = 0; i < colors.size(); ++i) {
            QImage img(64, 48, QImage::Format_RGB32);
            img.fill(colors[i]);
            QVERIFY(img.save(L(QString("pics/IMG_2024_000%1.jpg").arg(i)), "JPG"));
        }
        m_left->navigate(L("pics"));
        QTRY_COMPARE(leftModel()->path(), L("pics"));
        qv = m_window->findChild<ViewerWindow*>("quickView");
        moveCursorTo(m_left, "IMG_2024_0000.jpg");
        auto shown = [&] {  // what is drawn, not just what was loaded
            QWidget* vp = qv->imageView()->viewport();
            const QImage im = vp->grab().toImage();
            return im.isNull() ? QColor() : im.pixelColor(im.width() / 2, im.height() / 2);
        };
        auto near = [](QColor a, QColor b) {
            return a.isValid() && qAbs(a.red() - b.red()) < 40 && qAbs(a.green() - b.green()) < 40 && qAbs(a.blue() - b.blue()) < 40;
        };
        for (int i = 0; i < colors.size(); ++i) {
            QTRY_COMPARE(qv->currentPath(), L(QString("pics/IMG_2024_000%1.jpg").arg(i)));
            QTRY_VERIFY2(near(shown(), colors[i]), qPrintable(QString("file %1 shows %2").arg(i).arg(shown().name())));
            QTest::keyClick(m_left->view(), Qt::Key_Down);
        }
        QTest::keyClick(m_left->view(), Qt::Key_Q, Qt::ControlModifier);
        QTRY_VERIFY(m_window->findChild<ViewerWindow*>("quickView") == nullptr);
        QSettings().setValue("view/preview", false);

        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    void pathSegments()
    {
        m_left->navigate(L("sub/deep"));
        QTRY_COMPARE(leftModel()->path(), L("sub/deep"));
        auto* bar = m_left->findChild<PathBar*>();
        QVERIFY(bar);
        auto segment = [bar](const QString& path) -> QToolButton* {
            for (auto* b : bar->findChildren<QToolButton*>("crumb"))
                if (b->property("path").toString() == path)
                    return b;
            return nullptr;
        };
        QTRY_VERIFY(segment(L("sub")) != nullptr);
        QTRY_VERIFY(segment(L("sub"))->isVisible());
        segment(L("sub"))->click();
        QTRY_COMPARE(leftModel()->path(), L("sub"));
        QCOMPARE(m_left->currentName(), QString("deep"));  // lands where it came from

        // Middle click opens a tab in the background.
        QTRY_VERIFY(segment(L()) != nullptr);
        QTest::mouseClick(segment(L()), Qt::MiddleButton);
        QTRY_COMPARE(m_left->tabPaths(), QStringList({L("sub"), L()}));
        QCOMPARE(m_left->path(), L("sub"));
        m_left->cycleTab(1);
        m_left->closeCurrentTab();

        // Ctrl+L still takes a typed path (relative to the current folder).
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        QTest::keyClick(m_left->view(), Qt::Key_L, Qt::ControlModifier);
        QTRY_VERIFY(qobject_cast<QLineEdit*>(QApplication::focusWidget()));
        QTest::keyClicks(QApplication::focusWidget(), "deep");
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
        QTRY_COMPARE(leftModel()->path(), L("sub/deep"));

        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    void vimKeys()
    {
        QVERIFY(QDir().mkpath(L("vim/inner")));
        for (const char* n : {"a1.txt", "b2.txt", "c3.txt", "d4.txt"})
            writeFile(L(QString("vim/") + n), n);
        m_left->navigate(L("vim"));
        QTRY_COMPARE(leftModel()->path(), L("vim"));
        QAction* vim = nullptr;
        for (QAction* a : m_window->actions())
            if (a->text().startsWith(QObject::tr("Vim keys")))
                vim = a;
        QVERIFY(vim);
        vim->trigger();
        QVERIFY(vim->isChecked());
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        FileView* v = m_left->view();
        v->setFocus();
        v->setCurrentRow(0);

        QTest::keyClicks(v, "jj");
        QCOMPARE(m_left->currentName(), QString("a1.txt"));  // .., inner, a1
        QTest::keyClicks(v, "k");
        QCOMPARE(m_left->currentName(), QString("inner"));
        QTest::keyClicks(v, "G");
        QCOMPARE(m_left->currentName(), QString("d4.txt"));
        QTest::keyClicks(v, "gg");
        QCOMPARE(m_left->currentName(), QString(".."));
        QTest::keyClicks(v, "x");  // letters don't start quick search in vim mode
        QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(v));

        // / search, Enter, then n jumps to the next match.
        QTest::keyClicks(v, "/");
        QTRY_VERIFY(qobject_cast<QLineEdit*>(QApplication::focusWidget()));
        QTest::keyClicks(QApplication::focusWidget(), "txt");
        QTRY_COMPARE(m_left->currentName(), QString("a1.txt"));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
        QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget*>(v));
        QTest::keyClicks(v, "n");
        QCOMPARE(m_left->currentName(), QString("b2.txt"));
        QTest::keyClicks(v, "N");
        QCOMPARE(m_left->currentName(), QString("a1.txt"));

        // v marks, l enters, h leaves.
        QTest::keyClicks(v, "v");
        QCOMPARE(leftModel()->markedNames(), QStringList({"a1.txt"}));
        leftModel()->markAll(false);
        moveCursorTo(m_left, "inner");
        QTest::keyClicks(v, "l");
        QTRY_COMPARE(leftModel()->path(), L("vim/inner"));
        QTest::keyClicks(v, "h");
        QTRY_COMPARE(leftModel()->path(), L("vim"));

        vim->trigger();
        QVERIFY(!vim->isChecked());
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    void gitBadges()
    {
        // This repository itself: the panel shows the branch and model statuses.
        const QString repo = QStringLiteral(TANDEM_SOURCE_DIR);
        if (!QFileInfo::exists(repo + "/.git") || QStandardPaths::findExecutable("git").isEmpty())
            QSKIP("no git checkout");
        m_right->navigate(repo);
        QTRY_COMPARE(rightModel()->path(), repo);
        QTRY_VERIFY(!m_right->tab()->gitBranch().isEmpty());
        QTRY_COMPARE(rightModel()->gitStatus("build"), '!');  // ignored by .gitignore
        m_right->navigate(R());
        QTRY_COMPARE(rightModel()->path(), R());
    }

    void bookmarkChords()
    {
        FolderHistory& h = FolderHistory::instance();
        QTRY_VERIFY(h.isLoaded());
        h.setBookmark(L("sub"), "cp");
        h.setBookmark(L("sub/deep"), "cd");
        h.setBookmark(R(), "c");  // also a prefix of the two above
        auto* chords = m_window->findChild<KeyChordPopup*>();
        QVERIFY(chords);
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();

        // A chain: "c" waits (cp, cd and c exist), "p" jumps.
        QTest::keyClick(m_left->view(), Qt::Key_D, Qt::ControlModifier);
        QTRY_VERIFY(chords->isVisible());
        QTest::keyClicks(chords, "c");
        QVERIFY(chords->isVisible());
        QTest::keyClicks(chords, "p");
        QTRY_VERIFY(!chords->isVisible());
        QTRY_COMPARE(leftModel()->path(), L("sub"));

        // Enter takes the exact key even though longer ones start with it.
        m_left->view()->setFocus();
        QTest::keyClick(m_left->view(), Qt::Key_D, Qt::ControlModifier);
        QTRY_VERIFY(chords->isVisible());
        QTest::keyClicks(chords, "c");
        QTest::keyClick(chords, Qt::Key_Return);
        QTRY_COMPARE(leftModel()->path(), R());

        // Assign a new chain to the current folder.
        m_left->navigate(L("sub/deep"));
        QTRY_COMPARE(leftModel()->path(), L("sub/deep"));
        m_left->view()->setFocus();
        QTest::keyClick(m_left->view(), Qt::Key_D, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(chords->isVisible());
        for (int i = 0; i < 3; ++i)
            QTest::keyClick(chords, Qt::Key_Backspace);  // clear the existing "cd"
        QTest::keyClicks(chords, "zz");
        QTest::keyClick(chords, Qt::Key_Return);
        QTRY_COMPARE(h.pathForKey("zz"), L("sub/deep"));

        // Vim: g + key.
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
        m_left->setVimKeys(true);
        m_left->view()->setFocus();
        QTest::keyClicks(m_left->view(), "gz");
        QTRY_VERIFY(chords->isVisible());
        QTest::keyClicks(chords, "z");
        QTRY_COMPARE(leftModel()->path(), L("sub/deep"));
        m_left->setVimKeys(false);

        // Regression: with the right panel focused, the jump goes to the right panel.
        h.setBookmark(L("sub"), "q");
        m_right->view()->setFocus();
        QTRY_VERIFY(m_right->isActive());
        const QString leftBefore = leftModel()->path();
        QTest::keyClick(m_right->view(), Qt::Key_D, Qt::ControlModifier);
        QTRY_VERIFY(chords->isVisible());
        QTest::keyClicks(chords, "q");
        QTRY_COMPARE(rightModel()->path(), L("sub"));
        QCOMPARE(leftModel()->path(), leftBefore);
        QVERIFY(m_right->isActive());
        // …and the same for the fuzzy jump.
        auto* popup = m_window->findChild<FuzzyPopup*>();
        QTest::keyClick(m_right->view(), Qt::Key_J, Qt::ControlModifier);
        QTRY_VERIFY(popup->isVisible());
        QTest::keyClicks(QApplication::focusWidget(), "sub dee");
        QTRY_VERIFY(popup->currentItem() && popup->currentItem()->key == L("sub/deep"));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
        QTRY_COMPARE(rightModel()->path(), L("sub/deep"));
        QCOMPARE(leftModel()->path(), leftBefore);
        h.setPinned(L("sub"), false);
        m_right->navigate(R());
        QTRY_COMPARE(rightModel()->path(), R());
        m_left->view()->setFocus();

        for (const QString& p : {L("sub"), L("sub/deep"), R()})
            h.setPinned(p, false);
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    void searchAsFolder()
    {
        QVERIFY(QDir().mkpath(L("srch/a/b")));
        writeFile(L("srch/a/report.txt"), "quarterly numbers");
        writeFile(L("srch/a/b/report-old.txt"), "old");
        writeFile(L("srch/other.md"), "numbers here too");
        m_left->navigate(L("srch"));
        QTRY_COMPARE(leftModel()->path(), L("srch"));
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();

        QTest::keyClick(m_left->view(), Qt::Key_F7, Qt::AltModifier);
        QTRY_VERIFY(qobject_cast<QLineEdit*>(QApplication::focusWidget()));
        QTest::keyClicks(QApplication::focusWidget(), "report");
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
        QTRY_VERIFY(m_left->tab()->isSearch());
        QTRY_VERIFY(!m_left->tab()->isSearching());
        QTRY_VERIFY(leftModel()->rowOf("a/report.txt") >= 0);
        QVERIFY(leftModel()->rowOf("a/b/report-old.txt") >= 0);
        QCOMPARE(leftModel()->rowOf("other.md"), -1);

        // Results behave like files: the full path is right, delete works.
        moveCursorTo(m_left, "a/b/report-old.txt");
        QCOMPARE(m_left->currentPath(), L("srch/a/b/report-old.txt"));
        answerNextDialog(acceptDialog);
        QTest::keyClick(m_left->view(), Qt::Key_F8, Qt::ShiftModifier);
        QTRY_VERIFY(!QFile::exists(L("srch/a/b/report-old.txt")));
        QTRY_COMPARE(leftModel()->rowOf("a/b/report-old.txt"), -1);

        // Contents search (Tab flips the mode).
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        QTest::keyClick(m_left->view(), Qt::Key_F7, Qt::AltModifier);
        QTRY_VERIFY(qobject_cast<QLineEdit*>(QApplication::focusWidget()));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_A, Qt::ControlModifier);
        QTest::keyClicks(QApplication::focusWidget(), "numbers");
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
        QTRY_VERIFY(!m_left->tab()->isSearching());
        QTRY_COMPARE(m_left->tab()->searchFound(), 2);
        QVERIFY(leftModel()->rowOf("other.md") >= 0);

        // Esc leaves the results, back in the folder searched.
        m_left->view()->setFocus();
        QTest::keyClick(m_left->view(), Qt::Key_Escape);
        QTRY_VERIFY(!m_left->tab()->isSearch());
        QTRY_COMPARE(leftModel()->path(), L("srch"));
        QVERIFY(leftModel()->rowOf("a") >= 0);

        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    void runCommandOnSelection()
    {
        CommandContext ctx;
        ctx.selection = {"/a b/it's.txt", "/c"};
        ctx.current = "/a b/it's.txt";
        ctx.dir = "/a b";
        ctx.otherDir = "/x";
        QCOMPARE(expandCommand("cp %s %D", ctx), QString(R"(cp '/a b/it'\''s.txt' '/c' '/x')"));
        QCOMPARE(expandCommand("echo %n in %d, 100%%", ctx), QString(R"(echo 'it'\''s.txt' in '/a b', 100%)"));
        QCOMPARE(expandCommand("ls", ctx), QString("ls"));

        QVERIFY(QDir().mkpath(L("cmd")));
        QVERIFY(QDir().mkpath(R("cmd-target")));
        writeFile(L("cmd/one file.txt"), "1");
        writeFile(L("cmd/it's.txt"), "2");
        m_left->navigate(L("cmd"));
        m_right->navigate(R("cmd-target"));
        QTRY_COMPARE(leftModel()->path(), L("cmd"));
        QTRY_COMPARE(rightModel()->path(), R("cmd-target"));
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        leftModel()->markAll(true);

        QTest::keyClick(m_left->view(), Qt::Key_Semicolon, Qt::ControlModifier);
        QTRY_VERIFY(qobject_cast<QLineEdit*>(QApplication::focusWidget()));
        QTest::keyClicks(QApplication::focusWidget(), "cp %s %D");
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
        QTRY_VERIFY(QFile::exists(R("cmd-target/one file.txt")));
        QTRY_VERIFY(QFile::exists(R("cmd-target/it's.txt")));
        QTRY_VERIFY(rightModel()->rowOf("it's.txt") >= 0);  // panels update by themselves
        leftModel()->markAll(false);

        // Output goes to a window.
        m_left->view()->setFocus();
        QTest::keyClick(m_left->view(), Qt::Key_Semicolon, Qt::ControlModifier);
        QTRY_VERIFY(qobject_cast<QLineEdit*>(QApplication::focusWidget()));
        QTest::keyClicks(QApplication::focusWidget(), "echo hello from %d");
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
        QPlainTextEdit* out = nullptr;
        QTRY_VERIFY([&] {
            for (QWidget* w : QApplication::topLevelWidgets())
                if (auto* t = w->findChild<QPlainTextEdit*>(); t && w->isVisible() && t->toPlainText().startsWith("hello"))
                    out = t;
            return out != nullptr;
        }());
        QCOMPARE(out->toPlainText().trimmed(), QString("hello from ") + L("cmd"));
        out->window()->close();

        m_left->navigate(L());
        m_right->navigate(R());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    void compareAndSync()
    {
        auto at = [](const QString& p, qint64 secs) {
            QFile f(p);
            QVERIFY(f.open(QIODevice::ReadWrite));
            QVERIFY(f.setFileTime(QDateTime::fromSecsSinceEpoch(secs), QFileDevice::FileModificationTime));
        };
        QVERIFY(QDir().mkpath(L("cmp/sub")));
        QVERIFY(QDir().mkpath(R("cmp")));
        writeFile(L("cmp/same.txt"), "same");    writeFile(R("cmp/same.txt"), "same");
        writeFile(L("cmp/newer.txt"), "new!");   writeFile(R("cmp/newer.txt"), "old");
        writeFile(L("cmp/sub/deep.txt"), "deep");
        writeFile(R("cmp/rightonly.txt"), "r");
        at(L("cmp/same.txt"), 4000);  at(R("cmp/same.txt"), 4000);
        at(L("cmp/newer.txt"), 9000); at(R("cmp/newer.txt"), 3000);
        m_left->navigate(L("cmp"));
        m_right->navigate(R("cmp"));
        QTRY_COMPARE(leftModel()->path(), L("cmp"));
        QTRY_COMPARE(rightModel()->path(), R("cmp"));
        QTRY_VERIFY(leftModel()->rowOf("newer.txt") >= 0 && rightModel()->rowOf("rightonly.txt") >= 0);
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();

        // Shift+F2 marks what is newer or missing on the other side.
        QTest::keyClick(m_left->view(), Qt::Key_F2, Qt::ShiftModifier);
        QCOMPARE(QSet<QString>(leftModel()->markedSet()), QSet<QString>({"newer.txt", "sub"}));
        QCOMPARE(QSet<QString>(rightModel()->markedSet()), QSet<QString>({"rightonly.txt"}));
        leftModel()->markAll(false);
        rightModel()->markAll(false);

        // Ctrl+Shift+S: the sync dialog.
        QTest::keyClick(m_left->view(), Qt::Key_S, Qt::ControlModifier | Qt::ShiftModifier);
        SyncDialog* dlg = nullptr;
        QTRY_VERIFY((dlg = m_window->findChild<SyncDialog*>()) != nullptr);
        QTRY_VERIFY(!dlg->isComparing());
        SyncModel* m = dlg->model();
        QCOMPARE(m->rowCount(), 3);  // equal files hidden by default
        QCOMPARE(m->action(m->rowOf("newer.txt")), SyncAction::ToRight);
        QCOMPARE(m->action(m->rowOf("sub")), SyncAction::ToRight);
        QCOMPARE(m->action(m->rowOf("rightonly.txt")), SyncAction::ToLeft);

        // Keys change directions: Space leaves "rightonly.txt" alone.
        dlg->view()->setCurrentIndex(m->index(m->rowOf("rightonly.txt"), 0));
        QTest::keyClick(dlg->view(), Qt::Key_Space);
        QCOMPARE(m->action(m->rowOf("rightonly.txt")), SyncAction::None);
        QTest::keyClick(dlg->view(), Qt::Key_Left);
        QCOMPARE(m->action(m->rowOf("rightonly.txt")), SyncAction::ToLeft);
        QTest::keyClick(dlg->view(), Qt::Key_Right);  // impossible: nothing on the left
        QCOMPARE(m->action(m->rowOf("rightonly.txt")), SyncAction::ToLeft);

        QTest::keyClick(dlg->view(), Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(readFile(R("cmp/newer.txt")), QByteArray("new!"));
        QTRY_COMPARE(readFile(R("cmp/sub/deep.txt")), QByteArray("deep"));
        QTRY_COMPARE(readFile(L("cmp/rightonly.txt")), QByteArray("r"));
        // It compares again afterwards: nothing left to do.
        QTRY_VERIFY(!dlg->isComparing() && m->rowCount() == 0);
        QTRY_VERIFY(rightModel()->rowOf("sub") >= 0);

        // Mirror right → left removes what the right side doesn't have.
        writeFile(L("cmp/extra.txt"), "x");
        dlg->setMode(SyncMode::MirrorToLeft);
        dlg->compare();
        QTRY_VERIFY(!dlg->isComparing() && m->rowOf("extra.txt") >= 0);
        QCOMPARE(m->action(m->rowOf("extra.txt")), SyncAction::DeleteLeft);
        answerNextDialog(acceptDialog);
        dlg->synchronize();
        QTRY_VERIFY(!QFile::exists(L("cmp/extra.txt")));
        dlg->setMode(SyncMode::TwoWay);
        dlg->close();

        m_left->navigate(L());
        m_right->navigate(R());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    void networkMount()
    {
        if (!Network::available() || QStandardPaths::findExecutable("gvfsd-localtest", {"/usr/lib", "/usr/libexec", "/usr/lib/gvfs"}).isEmpty())
            QSKIP("GVfs localtest backend not available");
        QVERIFY(QDir().mkpath(L("net")));
        writeFile(R("tocopy.txt"), "over gvfs");
        m_right->navigate(R());
        QTRY_VERIFY(rightModel()->rowOf("tocopy.txt") >= 0);

        // What the path bar does with "sftp://…": mount through GVfs, then open.
        m_left->connectTo("localtest://" + L("net"));
        const QString viaGvfs = gvfsDirectory() + "/localtest:" + L("net");
        if (!QTest::qWaitFor([&] { return m_left->path() == viaGvfs; }, 15000))
            QSKIP("localtest mount failed in this session");
        QVERIFY(isRemotePath(m_left->path()));
        QVERIFY(Network::instance().canUnmount(m_left->path()));
        QVERIFY(Network::instance().recentServers().contains("localtest://" + L("net")));
        // The path bar shows a readable root instead of the GVfs folder name.
        auto labelled = [&] {
            for (auto* b : m_left->findChild<PathBar*>()->findChildren<QToolButton*>())
                if (b->isVisibleTo(m_left) && b->text().startsWith(QString::fromUtf8("⇄ localtest")))
                    return true;
            return false;
        };
        QTRY_VERIFY(labelled());

        // Copy into the remote folder with F5 (the normal job engine).
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_right->view()->setFocus();
        moveCursorTo(m_right, "tocopy.txt");
        answerNextDialog(acceptDialog);
        QTest::keyClick(m_right->view(), Qt::Key_F5);
        QTRY_COMPARE(readFile(L("net/tocopy.txt")), QByteArray("over gvfs"));
        QTRY_VERIFY(leftModel()->rowOf("tocopy.txt") >= 0);

        // Disconnect: the panel steps out, the mount goes away.
        m_left->disconnectMount(remoteRootOf(m_left->path())->root);
        QTRY_COMPARE(m_left->path(), QDir::homePath());
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(viaGvfs), 10000);
        Network::instance().forgetServer("localtest://" + L("net"));

        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    void serverLocation()
    {
        QTemporaryDir serverDir;
        const QString srv = serverDir.path();
        QVERIFY(QDir().mkpath(srv + "/www"));
        writeFile(srv + "/www/index.html", "<h1>hi</h1>");
        writeFile(L("upload.txt"), "to the server");
        auto backend = std::make_shared<TestBackend>(srv);
        registerBackend("test", backend);
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());

        // Typed into the path bar like any folder.
        m_right->focusPathEdit();
        auto* edit = qobject_cast<QLineEdit*>(QApplication::focusWidget());
        QVERIFY(edit);
        edit->setText("test://srv/www/");
        QTest::keyClick(edit, Qt::Key_Return);
        QTRY_COMPARE(rightModel()->path(), QString("test://srv/www"));
        QVERIFY(m_right->isRemote());
        QVERIFY(rightModel()->rowOf("index.html") >= 0);
        auto crumbs = [&] {
            QStringList t;
            for (auto* b : m_right->findChild<PathBar*>()->findChildren<QToolButton*>())
                if (b->isVisibleTo(m_right))
                    t << b->text();
            return t;
        };
        QTRY_VERIFY(crumbs().contains(QString::fromUtf8("⇄ test://srv")));
        QCOMPARE(m_right->tab()->title(), QString("www"));

        // F5 from local to the server panel; the panel updates without inotify.
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        moveCursorTo(m_left, "upload.txt");
        answerNextDialog(acceptDialog);
        QTest::keyClick(m_left->view(), Qt::Key_F5);
        QTRY_COMPARE(readFile(srv + "/www/upload.txt"), QByteArray("to the server"));
        QTRY_VERIFY(rightModel()->rowOf("upload.txt") >= 0);

        // F7 and rename on the server.
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_right->view()->setFocus();
        answerNextDialog([](QWidget* w) {
            if (auto* d = qobject_cast<QInputDialog*>(w)) {
                d->setTextValue("assets/css");
                d->accept();
            }
        });
        QTest::keyClick(m_right->view(), Qt::Key_F7);
        QTRY_VERIFY(QFileInfo(srv + "/www/assets/css").isDir());
        QTRY_VERIFY(rightModel()->rowOf("assets") >= 0);
        m_right->doRename("upload.txt", "renamed.txt");
        QTRY_VERIFY(QFile::exists(srv + "/www/renamed.txt"));
        QTRY_VERIFY(rightModel()->rowOf("renamed.txt") >= 0);

        // F3: downloaded to the cache, then shown.
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_right->view()->setFocus();
        moveCursorTo(m_right, "index.html");
        QTest::keyClick(m_right->view(), Qt::Key_F3);
        ViewerWindow* viewer = nullptr;
        QTRY_VERIFY([&] {
            for (QWidget* w : QApplication::topLevelWidgets())
                if (auto* v = qobject_cast<ViewerWindow*>(w); v && v->isVisible())
                    viewer = v;
            return viewer != nullptr;
        }());
        QTRY_COMPARE(viewer->currentPath(), remoteCachePath("test://srv/www/index.html"));
        QCOMPARE(readFile(viewer->currentPath()), QByteArray("<h1>hi</h1>"));
        viewer->close();

        // F4: edit a copy; saving uploads it again.
        const QString editorScript = serverDir.filePath("fake-editor.sh");
        writeFile(editorScript, "#!/bin/sh\nprintf edited > \"$1\"\n");
        QFile::setPermissions(editorScript, QFile::permissions(editorScript) | QFile::ExeOwner);
        QSettings().setValue("tools/editor", editorScript);
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_right->view()->setFocus();
        moveCursorTo(m_right, "renamed.txt");
        QTest::keyClick(m_right->view(), Qt::Key_F4);
        QTRY_COMPARE_WITH_TIMEOUT(readFile(srv + "/www/renamed.txt"), QByteArray("edited"), 10000);
        QSettings().remove("tools/editor");

        // F8 on a server: permanent (no trash), says so.
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        QString info;
        answerNextDialog([&](QWidget* w) {
            if (auto* box = qobject_cast<QMessageBox*>(w)) {
                info = box->informativeText();
                box->button(QMessageBox::Yes)->click();
            }
        });
        m_right->view()->setFocus();
        moveCursorTo(m_right, "assets");
        QTest::keyClick(m_right->view(), Qt::Key_F8);
        QTRY_VERIFY(!QFile::exists(srv + "/www/assets"));
        QVERIFY(info.contains("no trash"));
        QTRY_COMPARE(rightModel()->rowOf("assets"), -1);

        // Up to the server root, which can't go further.
        m_right->goUp();
        QTRY_COMPARE(rightModel()->path(), QString("test://srv/"));
        m_right->goUp();
        QTest::qWait(100);
        QCOMPARE(rightModel()->path(), QString("test://srv/"));

        m_right->navigate(R());
        QTRY_COMPARE(rightModel()->path(), R());
        // A poll can still be reading the mock server after navigation completes.
        QVERIFY(ioPool()->waitForDone(5000));
        registerBackend("test", nullptr);
    }

    // A login asks user and password in one dialog.
    void loginDialog()
    {
        std::optional<QString> answer;
        std::thread asker([&] {
            AuthQuestion q;
            q.kind = AuthQuestion::Kind::Login;
            q.location = QStringLiteral("smb://nas/");
            q.message = QStringLiteral("Log in to nas");
            q.user = QStringLiteral("bob");
            answer = askAuth(q);
        });
        QDialog* dlg = nullptr;
        QTRY_VERIFY([&] {
            for (QWidget* w : QApplication::allWidgets())
                if (w->objectName() == QLatin1String("authDialog") && w->isVisible())
                    dlg = qobject_cast<QDialog*>(w);
            return dlg != nullptr;
        }());
        const QList<QLineEdit*> edits = dlg->findChildren<QLineEdit*>();
        QCOMPARE(edits.size(), 2);
        QCOMPARE(edits[0]->text(), QStringLiteral("bob"));
        QCOMPARE(edits[1]->echoMode(), QLineEdit::Password);
        edits[0]->setText(QStringLiteral(" WORK\\alice "));
        edits[1]->setText(QStringLiteral("se cret"));
        dlg->accept();
        asker.join();
        QCOMPARE(answer, std::optional<QString>(QStringLiteral("WORK\\alice\nse cret")));
    }

    // "Remember in the keyring": offered with a keyring, and reported to the asker.
    void loginDialogRemembers()
    {
        setKeyringHooksForTests({[](const QString&, const QString&, const QString&) { return std::optional<StoredLogin>{}; },
                                 [](const QString&, const QString&, const StoredLogin&) { return true; }});
        std::optional<QString> answer;
        bool remembered = false;
        std::thread asker([&] {
            AuthQuestion q;
            q.kind = AuthQuestion::Kind::Login;
            q.location = QStringLiteral("smb://nas/");
            q.user = QStringLiteral("bob");
            q.offerRemember = true;
            answer = askAuth(q);
            remembered = lastAnswerRemember();
        });
        QDialog* dlg = nullptr;
        QTRY_VERIFY([&] {
            for (QWidget* w : QApplication::allWidgets())
                if (w->objectName() == QLatin1String("authDialog") && w->isVisible())
                    dlg = qobject_cast<QDialog*>(w);
            return dlg != nullptr;
        }());
        auto* box = dlg->findChild<QCheckBox*>();
        QVERIFY(box);
        box->setChecked(true);
        dlg->findChildren<QLineEdit*>().at(1)->setText("pw");
        dlg->accept();
        asker.join();
        QCOMPARE(answer, std::optional<QString>(QStringLiteral("bob\npw")));  // no marker left in it
        QVERIFY(remembered);
        setKeyringHooksForTests({});
    }

    void sftpInPanel()
    {
        if (!hasBackend("sftp"))
            QSKIP("built without libssh");
        SshServer server;
        if (!server.start())
            QSKIP("could not start a test sshd");
        QVERIFY(QDir().mkpath(L("remote/docs")));
        writeFile(L("remote/readme.txt"), "served over sftp");
        writeFile(L("toupload.txt"), "uploaded");
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());

        // Typed address; the unknown host key is asked in a dialog.
        QString question;
        answerNextDialog([&](QWidget* w) {
            question = w->windowTitle();
            for (auto* b : w->findChildren<QPushButton*>())
                if (b->text() == QObject::tr("Trust and connect"))
                    b->click();
        });
        m_right->focusPathEdit();
        auto* edit = qobject_cast<QLineEdit*>(QApplication::focusWidget());
        QVERIFY(edit);
        const QString loc = server.location(L("remote"));
        edit->setText(loc);
        QTest::keyClick(edit, Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(rightModel()->path(), loc, 10000);
        QCOMPARE(question, QObject::tr("Unknown server"));
        QVERIFY(m_right->isRemote());
        QVERIFY(rightModel()->rowOf("docs") >= 0);
        QVERIFY(rightModel()->rowOf("readme.txt") >= 0);

        // F5 upload into the sftp panel.
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        moveCursorTo(m_left, "toupload.txt");
        answerNextDialog(acceptDialog);
        QTest::keyClick(m_left->view(), Qt::Key_F5);
        QTRY_COMPARE(readFile(L("remote/toupload.txt")), QByteArray("uploaded"));
        QTRY_VERIFY(rightModel()->rowOf("toupload.txt") >= 0);

        // The preview of a folder on the server: its contents, then the total.
        writeFile(L("remote/docs/a.txt"), "1234");
        QVERIFY(QDir().mkpath(L("remote/docs/deeper")));
        writeFile(L("remote/docs/deeper/b.txt"), "123456");
        QSettings().setValue("view/previewPosition", "each");
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_right->view()->setFocus();
        QTRY_VERIFY(m_right->isActive());
        moveCursorTo(m_right, "docs");
        QTest::keyClick(m_right->view(), Qt::Key_Q, Qt::ControlModifier);
        ViewerWindow* pv = nullptr;
        QTRY_VERIFY((pv = m_window->findChild<ViewerWindow*>("quickViewRight")) != nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(pv->currentPath(), loc + "/docs", 10000);
        QTRY_COMPARE_WITH_TIMEOUT(pv->contentsList()->topLevelItemCount(), 2, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(pv->richInfo().contains("10 B"), 10000);  // 4 + 6 bytes, in one request
        QTest::keyClick(m_right->view(), Qt::Key_Q, Qt::ControlModifier);
        QTRY_VERIFY(m_window->findChild<ViewerWindow*>("quickViewRight") == nullptr);
        QSettings().setValue("view/preview", false);

#ifndef Q_OS_MACOS
        // This block requires GNU stat/getfacl on the local test server.
        // Alt+A on the server: its own permissions tools, its users.
        ::chmod(QFile::encodeName(L("remote/readme.txt")).constData(), 0600);
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_right->view()->setFocus();
        m_right->refresh();
        QTRY_VERIFY(rightModel()->rowOf("readme.txt") >= 0);
        moveCursorTo(m_right, "readme.txt");
        auto permsChecked = std::make_shared<bool>(false);
        answerNextDialog([permsChecked](QWidget* w) {
            auto* dlg = qobject_cast<PermissionsDialog*>(w);
            QVERIFY(dlg);
            QCOMPARE(dlg->octalEdit()->text(), QString("600"));
            QVERIFY(dlg->addEdit());  // extra people and groups: the server has getfacl
            dlg->bitBox(1, 1)->click();  // group may write
            *permsChecked = true;
            dlg->applyButton()->click();
        });
        QTest::keyClick(m_right->view(), Qt::Key_A, Qt::AltModifier);
        QTRY_VERIFY_WITH_TIMEOUT(*permsChecked, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(readPermissions(L("remote/readme.txt")).mode, 0620u, 10000);

        #endif
        // A bare address opens the login's home folder.
        m_right->connectTo(server.location().chopped(1));
        QTRY_COMPARE_WITH_TIMEOUT(rightModel()->path(), server.location(QDir::homePath()), 10000);

        m_right->navigate(R());
        QTRY_COMPARE(rightModel()->path(), R());
    }

    void cleanupTestCase()
    {
        // What the tests moved to a trash (/tmp has its own .Trash-<uid>) goes for good.
        QList<TrashItem> ours;
        for (const TrashItem& it : listTrash())
            if (it.originalPath.startsWith(m_tmp.path() + '/'))
                ours << it;
        deleteTrashItems(ours);
        m_window->close();
        m_window.reset();
    }
};

QTEST_MAIN(TestUi)
#include "test_ui.moc"
