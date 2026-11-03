#include "ui/MainWindow.h"

#include "core/Archive.h"
#include "core/Backend.h"
#include "core/DesktopApps.h"
#include "core/FolderHistory.h"
#include "core/MultiRename.h"
#include "core/Util.h"
#include "ui/DirTab.h"
#include "ui/Dialogs.h"
#include "ui/FileModel.h"
#include "ui/FileColors.h"
#include "ui/FileView.h"
#include "ui/ThumbGrid.h"
#include "ui/IconCache.h"
#include "ui/JobManager.h"
#include "ui/CommandBar.h"
#include "ui/SyncDialog.h"
#include "ui/JobsPanel.h"
#include "ui/KeyChordPopup.h"
#include "ui/EditNames.h"
#include "ui/ChecksumDialog.h"
#include "ui/DiskUsageDialog.h"
#include "ui/DuplicatesDialog.h"
#include "ui/MultiRenameDialog.h"
#include "ui/PermissionsDialog.h"
#include "ui/SettingsDialog.h"
#include "ui/ShortcutsDialog.h"
#include "ui/TrashDialog.h"
#include "ui/Panel.h"
#include "ui/Theme.h"
#include "ui/viewer/ViewerWindow.h"

#include <QAbstractButton>
#include <QActionGroup>
#include <QApplication>
#include <QPushButton>
#include <QToolButton>
#include <QDialogButtonBox>
#include <QButtonGroup>
#include <QRegularExpression>
#include <QDateTime>
#include <QStandardPaths>
#include <QCryptographicHash>
#include <QClipboard>
#ifndef Q_OS_MACOS
#include <QDBusConnection>
#include <QDBusInterface>
#endif
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTimer>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMimeDatabase>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QPlainTextEdit>
#include <QProcess>
#include <QScrollArea>
#include <QSettings>
#include <QSplitter>
#include <QVBoxLayout>

#include <cmath>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

namespace tandem {

namespace {

// Bottom bar button: "F5" in accent color followed by the command name.
class FKeyButton : public QAbstractButton {
public:
    FKeyButton(const QString& key, const QString& label, QWidget* parent)
        : QAbstractButton(parent), m_key(key), m_label(label)
    {
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    QSize sizeHint() const override
    {
        const QFontMetrics fm(font());
        return {fm.horizontalAdvance(m_key + QStringLiteral("  ") + m_label) + 24, fm.height() + 12};
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        const Theme& t = theme();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (isDown() || underMouse()) {
            p.setPen(Qt::NoPen);
            p.setBrush(isDown() ? t.cursorActive : t.header);
            p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 6, 6);
        }
        QFont kf = font();
        kf.setWeight(QFont::Bold);
        const QFontMetrics kfm(kf);
        const QFontMetrics fm(font());
        const int total = kfm.horizontalAdvance(m_key) + 6 + fm.horizontalAdvance(m_label);
        int x = (width() - total) / 2;
        p.setFont(kf);
        p.setPen(t.accent);
        p.drawText(QRect(x, 0, width(), height()), Qt::AlignLeft | Qt::AlignVCenter, m_key);
        x += kfm.horizontalAdvance(m_key) + 6;
        p.setFont(font());
        p.setPen(t.text);
        p.drawText(QRect(x, 0, width(), height()), Qt::AlignLeft | Qt::AlignVCenter, m_label);
    }

private:
    QString m_key, m_label;
};

QStringList localPaths(const QList<QUrl>& urls)
{
    QStringList out;
    for (const QUrl& u : urls)
        if (u.isLocalFile())
            out.append(QDir::cleanPath(u.toLocalFile()));
    return out;
}

QString describeItems(const QStringList& paths)
{
    if (paths.size() == 1)
        return QStringLiteral("“%1”").arg(baseName(paths.first()));
    return QObject::tr("%n items", nullptr, int(paths.size()));
}

}  // namespace

MainWindow::MainWindow(const QStringList& startPaths)
{
    setWindowTitle(QStringLiteral("Tandem"));
    m_jobs = new JobManager(this);

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(8, 6, 8, 4);
    layout->setSpacing(6);

    m_splitter = new QSplitter(Qt::Horizontal, central);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setHandleWidth(8);
    m_left = new Panel(m_splitter);
    m_right = new Panel(m_splitter);
    m_left->setObjectName(QStringLiteral("leftPanel"));
    m_right->setObjectName(QStringLiteral("rightPanel"));
    // Each panel sits in a column that can hold its own preview below it.
    m_leftCol = new QSplitter(Qt::Vertical, m_splitter);
    m_rightCol = new QSplitter(Qt::Vertical, m_splitter);
    for (QSplitter* col : {m_leftCol, m_rightCol}) {
        col->setChildrenCollapsible(false);
        col->setHandleWidth(8);
    }
    m_leftCol->addWidget(m_left);
    m_rightCol->addWidget(m_right);
    m_splitter->addWidget(m_leftCol);
    m_splitter->addWidget(m_rightCol);
    // Both previews keep the same height when one is dragged.
    auto syncHeights = [this](QSplitter* from, QSplitter* to) {
        connect(from, &QSplitter::splitterMoved, this, [from, to] {
            if (to->count() == 2 && from->count() == 2 && to->sizes() != from->sizes())
                to->setSizes(from->sizes());
        });
    };
    syncHeights(m_leftCol, m_rightCol);
    syncHeights(m_rightCol, m_leftCol);
    m_vsplit = new QSplitter(Qt::Vertical, central);
    m_vsplit->setChildrenCollapsible(false);
    m_vsplit->setHandleWidth(8);
    m_vsplit->addWidget(m_splitter);
    layout->addWidget(m_vsplit, 1);

    QWidget* fkeys = createFKeyBar();
    layout->addWidget(fkeys);
    setCentralWidget(central);
    // Floats over the panels in the bottom-right corner, above the F-key bar.
    m_jobsPanel = new JobsPanel(m_jobs, central);
    m_jobsPanel->setBottomMargin(fkeys->sizeHint().height() + 14);

    for (Panel* p : {m_left, m_right}) {
        connect(p, &Panel::activated, this, &MainWindow::setActive);
        connect(p, &Panel::switchRequested, this, [this] {
            other()->view()->setFocus();
        });
        connect(p, &Panel::cursorChanged, this, [this](Panel* panel) {
            if (m_quickView && (m_quickView2 || panel == active()))
                m_quickTimer->start();
        });
        connect(p, &Panel::openFileRequested, this, [this, p](const QString& path) { openFrom(p, path); });
        connect(p, &Panel::dropRequested, this,
                [this, p](const QList<QUrl>& urls, const QString& dir, Qt::DropAction action) {
                    handleDrop(urls, dir, action);
                });
        connect(p, &Panel::contextMenuRequested, this, &MainWindow::showContextMenu);
        connect(p, &Panel::zoomRequested, this, &MainWindow::setZoom);
        connect(p, &Panel::commandIdRequested, this, &MainWindow::runCommandId);
        connect(p, &Panel::paletteRequested, this, &MainWindow::commandPalette);
        connect(p, &Panel::bookmarkJumpRequested, this, [this](const QString& keys) { m_chords->openJump(keys); });
        connect(p, &Panel::bookmarkAssignRequested, this, [this] { m_chords->openAssign(active()->path()); });
        connect(p, &Panel::commandRequested, this, &MainWindow::openCommandBar);
        connect(p, &Panel::trashRequested, this, &MainWindow::showTrash);
        connect(p, &Panel::archiveRenameRequested, this, [this, p](const QString& from, const QString& to) {
            ArchiveChanges c;
            c.rename.append({p->innerPath(from), p->innerPath(to)});
            changeArchive(p, c, tr("Rename in %1").arg(baseName(p->archivePath())));
        });
        connect(p, &Panel::renamed, this, [this](const QString& dir, const QString& from, const QString& to) {
            pushUndo({UndoEntry::Kind::Rename, tr("rename of %1").arg(from), dir, {{to, from}}});
        });
    }
    // Previews are for looking, not typing: a click in one hands the keys
    // back to the list it belongs to (and makes that panel the active one).
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        returnFocusFromPreview(now);
        // Focus lost to nothing (a popup closed by a click on a label, say):
        // the keys go back to the active list rather than nowhere.
        if (!now || now == this)
            QTimer::singleShot(0, this, [this] {
                QWidget* f = QApplication::focusWidget();
                if (isActiveWindow() && !QApplication::activeModalWidget() && !QApplication::activePopupWidget() &&
                    (!f || f == this))
                    active()->view()->setFocus();
            });
    });
    qApp->installEventFilter(this);  // clicks on a preview's non-focusable parts too
    // Directories without inotify (some FUSE/network mounts) need a manual
    // refresh once an operation that touched them is done.
    connect(m_jobs, &JobManager::jobFinished, this, [this](Job* job) {
        if (!job->property("isUndo").toBool())
            recordJobUndo(job);
        notifyFinished(job);
        for (Panel* p : {m_left, m_right})
            if (p->tab() && !p->tab()->isWatched())
                p->refresh();
    });

    createMenus();
    loadSettings(startPaths);
    DesktopApps::instance().loadAsync();
    FolderHistory::instance().loadAsync();
    m_popup = new FuzzyPopup(this);
    m_commandBar = new CommandBar(this);
    connect(m_commandBar, &CommandBar::runRequested, this, &MainWindow::runCommand);
    m_chords = new KeyChordPopup(this);
    connect(m_chords, &KeyChordPopup::jumpRequested, this, [this](const QString& p) { active()->navigate(p); });
    connect(m_chords, &KeyChordPopup::fuzzyRequested, this, &MainWindow::jumpToFolder);
    connect(m_chords, &KeyChordPopup::message, this,
            [this](const QString& text, bool error) { active()->showMessage(text, error); });
    m_quickTimer = new QTimer(this);
    m_quickTimer->setSingleShot(true);
    m_quickTimer->setInterval(120);  // holding an arrow key doesn't decode every file
    connect(m_quickTimer, &QTimer::timeout, this, &MainWindow::updateQuickView);
    ioPool()->start([] { pruneExtractCache(); });
    loadPlugins();
    QPointer<MainWindow> self(this);
    ioPool()->start([self] {
        auto themes = std::make_shared<QList<IconTheme>>(scanIconThemes());
        postToUi([self, themes] {
            if (!self)
                return;
            self->m_iconThemes = *themes;
            self->m_iconThemesLoaded = true;
        });
    });
}

Panel* MainWindow::other() const
{
    return m_active == m_left ? m_right : m_left;
}

void MainWindow::setActive(Panel* p)
{
    if (m_active == p)
        return;
    m_active = p;
    m_left->setActive(p == m_left);
    m_right->setActive(p == m_right);
    if (m_quickView)
        m_quickTimer->start();
    setWindowTitle(QStringLiteral("Tandem — %1").arg(p->path()));
}

// ------------------------------------------------------------------ menus

// Commands get a stable id from their English name, so changed keys
// survive a language switch.
QAction* MainWindow::addCommand(QMenu* menu, const char* source, const QList<QKeySequence>& keys,
                                std::function<void()> fn)
{
    QString id = QString::fromUtf8(source).toLower();
    id.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    id.remove(QRegularExpression(QStringLiteral("^-+|-+$")));
    return addCommand(menu, tr(source), keys, std::move(fn), id);
}

QAction* MainWindow::addCommand(QMenu* menu, const QString& text, const QList<QKeySequence>& keys,
                                std::function<void()> fn, const QString& id)
{
    QAction* a = menu->addAction(text);
    a->setShortcuts(keys);
    a->setProperty("commandId", id);
    a->setProperty("defaultKeys", QVariant::fromValue(keys));
    a->setProperty("category", menu->title().remove(QLatin1Char('&')));
    a->setShortcutContext(Qt::WindowShortcut);
    connect(a, &QAction::triggered, this, [fn = std::move(fn)] { fn(); });
    addAction(a);  // shortcuts work even with the menu bar hidden
    return a;
}

void MainWindow::createMenus()
{
    using K = QKeySequence;
    QMenuBar* bar = menuBar();

    // Files: what happens to the selected items.
    QMenu* files = bar->addMenu(tr("&Files"));
    m_actions[QStringLiteral("open")] = addCommand(files, QT_TR_NOOP("Open"), {}, [this] { active()->openCurrent(); });
    addCommand(files, QT_TR_NOOP("View"), {K(Qt::Key_F3)}, [this] { viewCurrent(); });
    m_actions[QStringLiteral("edit")] = addCommand(files, QT_TR_NOOP("Edit"), {K(Qt::Key_F4)}, [this] { editCurrent(); });
    files->addSeparator();
    m_actions[QStringLiteral("copy")] = addCommand(files, QT_TR_NOOP("Copy…"), {K(Qt::Key_F5)}, [this] { copyOrMove(CopyJob::Mode::Copy); });
    m_actions[QStringLiteral("move")] = addCommand(files, QT_TR_NOOP("Move…"), {K(Qt::Key_F6)}, [this] { copyOrMove(CopyJob::Mode::Move); });
    m_actions[QStringLiteral("rename")] = addCommand(files, QT_TR_NOOP("Rename"), {K(Qt::SHIFT | Qt::Key_F6)}, [this] { active()->startRename(); });
    m_actions[QStringLiteral("multiRename")] =
        addCommand(files, QT_TR_NOOP("Multi-rename…"), {K(Qt::CTRL | Qt::Key_M)}, [this] { multiRename(); });
    m_actions[QStringLiteral("editorRename")] =
        addCommand(files, QT_TR_NOOP("Rename in text editor…"), {K(Qt::CTRL | Qt::Key_E)}, [this] { renameInEditor(); });
    files->addSeparator();
    m_actions[QStringLiteral("mkfile")] =
        addCommand(files, QT_TR_NOOP("New file…"), {K(Qt::SHIFT | Qt::Key_F4)}, [this] { makeFile(); });
    m_actions[QStringLiteral("mkdir")] = addCommand(files, QT_TR_NOOP("New folder…"), {K(Qt::Key_F7)}, [this] { makeFolder(); });
    m_actions[QStringLiteral("link")] =
        addCommand(files, QT_TR_NOOP("Create link…"), {K(Qt::CTRL | Qt::SHIFT | Qt::Key_F5)}, [this] { createLink(); });
    files->addSeparator();
    m_actions[QStringLiteral("permissions")] =
        addCommand(files, QT_TR_NOOP("Permissions…"), {K(Qt::ALT | Qt::Key_A)}, [this] { editPermissions(); });
    files->addSeparator();
    m_actions[QStringLiteral("trash")] = addCommand(files, QT_TR_NOOP("Move to trash"), {K(Qt::Key_F8), K(Qt::Key_Delete)}, [this] { deleteSelected(false); });
    m_actions[QStringLiteral("delete")] = addCommand(files, QT_TR_NOOP("Delete permanently"), {K(Qt::SHIFT | Qt::Key_F8), K(Qt::SHIFT | Qt::Key_Delete)},
               [this] { deleteSelected(true); });
    files->addSeparator();
    addCommand(files, QT_TR_NOOP("Quit"), {K(Qt::ALT | Qt::Key_F4)}, [this] { close(); });

    // Edit: undo, the clipboard and marking.
    QMenu* edit = bar->addMenu(tr("&Edit"));
    m_actions[QStringLiteral("undo")] = addCommand(edit, QT_TR_NOOP("Undo"), {K(Qt::CTRL | Qt::Key_Z)}, [this] { undo(); });
    m_actions[QStringLiteral("undo")]->setEnabled(false);
    edit->addSeparator();
    m_actions[QStringLiteral("clipCut")] = addCommand(edit, QT_TR_NOOP("Cut to clipboard"), {K(Qt::CTRL | Qt::Key_X)}, [this] { copyToClipboard(true); });
    m_actions[QStringLiteral("clipCopy")] = addCommand(edit, QT_TR_NOOP("Copy to clipboard"), {K(Qt::CTRL | Qt::Key_C)}, [this] { copyToClipboard(false); });
    m_actions[QStringLiteral("paste")] = addCommand(edit, QT_TR_NOOP("Paste"), {K(Qt::CTRL | Qt::Key_V)}, [this] { paste(); });
    m_actions[QStringLiteral("copyPath")] = addCommand(edit, QT_TR_NOOP("Copy full paths as text"), {K(Qt::CTRL | Qt::SHIFT | Qt::Key_C)},
               [this] { copyPathsAsText(); });
    edit->addSeparator();
    m_actions[QStringLiteral("markAll")] = addCommand(edit, QT_TR_NOOP("Mark all"), {K(Qt::CTRL | Qt::Key_A)}, [this] { active()->markAll(true); });
    m_actions[QStringLiteral("unmarkAll")] = addCommand(edit, QT_TR_NOOP("Unmark all"), {K(Qt::CTRL | Qt::SHIFT | Qt::Key_A)}, [this] { active()->markAll(false); });
    addCommand(edit, QT_TR_NOOP("Mark group…\tNum +"), {}, [this] { active()->markGroup(1); });
    addCommand(edit, QT_TR_NOOP("Unmark group…\tNum -"), {}, [this] { active()->markGroup(-1); });
    addCommand(edit, QT_TR_NOOP("Invert marks\tNum *"), {}, [this] { active()->markGroup(0); });
    edit->addSeparator();
    addCommand(edit, QT_TR_NOOP("Filter as you type…"), {K(Qt::CTRL | Qt::Key_F)}, [this] { active()->startFilter(); });

    QMenu* go = bar->addMenu(tr("&Go"));
    addCommand(go, QT_TR_NOOP("Back"), {K(Qt::ALT | Qt::Key_Left)}, [this] { active()->back(); });
    addCommand(go, QT_TR_NOOP("Forward"), {K(Qt::ALT | Qt::Key_Right)}, [this] { active()->forward(); });
    addCommand(go, QT_TR_NOOP("Parent folder"), {K(Qt::CTRL | Qt::Key_PageUp)}, [this] { active()->goUp(); });
    addCommand(go, QT_TR_NOOP("Enter folder or archive"), {K(Qt::CTRL | Qt::Key_PageDown)}, [this] { active()->enterCurrent(); });
    addCommand(go, QT_TR_NOOP("Home"), {K(Qt::CTRL | Qt::Key_QuoteLeft)}, [this] { active()->navigate(QDir::homePath()); });
    addCommand(go, QT_TR_NOOP("Root"), {K(Qt::CTRL | Qt::Key_Backslash)}, [this] { active()->navigate(QStringLiteral("/")); });
    addCommand(go, QT_TR_NOOP("Go to path…"), {K(Qt::CTRL | Qt::Key_L)}, [this] { active()->focusPathEdit(); });
    go->addSeparator();
    addCommand(go, QT_TR_NOOP("Jump to folder (search)…"), {K(Qt::CTRL | Qt::Key_J)}, [this] { jumpToFolder(); });
    go->addSeparator();
    addCommand(go, QT_TR_NOOP("Locations (left)"), {K(Qt::ALT | Qt::Key_F1)}, [this] { m_left->showMountMenu(); });
    addCommand(go, QT_TR_NOOP("Locations (right)"), {K(Qt::ALT | Qt::Key_F2)}, [this] { m_right->showMountMenu(); });
    addCommand(go, QT_TR_NOOP("Connect to server…"), {K(Qt::CTRL | Qt::Key_K)}, [this] { active()->connectDialog(); });
    addCommand(go, QT_TR_NOOP("Trash…"), {}, [this] { showTrash(); });
    go->addSeparator();
    addCommand(go, QT_TR_NOOP("Swap panels"), {K(Qt::CTRL | Qt::Key_U)}, [this] { swapPanels(); });
    addCommand(go, QT_TR_NOOP("Same folder in other panel"), {K(Qt::ALT | Qt::Key_Equal)},
               [this] { other()->navigate(active()->path()); });
    addCommand(go, QT_TR_NOOP("Show in left panel"), {K(Qt::CTRL | Qt::Key_Left)}, [this] { openInPanel(m_left); });
    addCommand(go, QT_TR_NOOP("Show in right panel"), {K(Qt::CTRL | Qt::Key_Right)}, [this] { openInPanel(m_right); });

    // Bookmarks: the folders with a key chain and the pinned ones, for the
    // mouse; the keys show where shortcuts go. Filled each time it opens.
    QMenu* marks = bar->addMenu(tr("&Bookmarks"));
    addCommand(marks, QT_TR_NOOP("Bookmark this folder…"), {K(Qt::CTRL | Qt::SHIFT | Qt::Key_D)},
               [this] { m_chords->openAssign(active()->path()); });
    addCommand(marks, QT_TR_NOOP("Bookmarks (key chains)…"), {K(Qt::CTRL | Qt::Key_D)}, [this] { m_chords->openJump(); });
    marks->addSeparator();
    connect(marks, &QMenu::aboutToShow, this, [this, marks] { fillBookmarksMenu(marks); });

    QMenu* tabs = bar->addMenu(tr("&Tabs"));
    addCommand(tabs, QT_TR_NOOP("New tab"), {K(Qt::CTRL | Qt::Key_T)}, [this] {
        const QString p = active()->currentEntry() && active()->currentEntry()->isDir &&
                                  !active()->currentEntry()->isParent
                              ? active()->currentPath()
                              : active()->path();
        active()->addTab(p);
    });
    addCommand(tabs, QT_TR_NOOP("Close tab"), {K(Qt::CTRL | Qt::Key_W)}, [this] { active()->closeCurrentTab(); });
    tabs->addSeparator();
    addCommand(tabs, QT_TR_NOOP("Next tab"), {K(Qt::CTRL | Qt::Key_Tab)}, [this] { active()->cycleTab(1); });
    addCommand(tabs, QT_TR_NOOP("Previous tab"), {K(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab), K(Qt::CTRL | Qt::Key_Backtab)},
               [this] { active()->cycleTab(-1); });
    tabs->addSeparator();
    for (int i = 1; i <= 9; ++i)
        addCommand(tabs, tr("Tab %1").arg(i), {K(Qt::ALT | (Qt::Key_0 + i))}, [this, i] { active()->selectSlot(i); },
                   QStringLiteral("tab-%1").arg(i));

    // View: how the panels look right now. Lasting choices are in Settings.
    QMenu* view = bar->addMenu(tr("&View"));
    QMenu* sort = view->addMenu(tr("Sort by"));
    m_actions[QStringLiteral("sortName")] = addCommand(sort, QT_TR_NOOP("Sort by name"), {K(Qt::CTRL | Qt::Key_F3)}, [this] { active()->sortBy(SortColumn::Name); });
    m_actions[QStringLiteral("sortExt")] = addCommand(sort, QT_TR_NOOP("Sort by extension"), {K(Qt::CTRL | Qt::Key_F4)}, [this] { active()->sortBy(SortColumn::Ext); });
    m_actions[QStringLiteral("sortDate")] = addCommand(sort, QT_TR_NOOP("Sort by date"), {K(Qt::CTRL | Qt::Key_F5)}, [this] { active()->sortBy(SortColumn::Time); });
    m_actions[QStringLiteral("sortSize")] = addCommand(sort, QT_TR_NOOP("Sort by size"), {K(Qt::CTRL | Qt::Key_F6)}, [this] { active()->sortBy(SortColumn::Size); });
    for (QAction* a : sort->actions())
        a->setProperty("category", tr("View"));  // for the palette and keys page
    view->addSeparator();
    m_hiddenAction = addCommand(view, QT_TR_NOOP("Show hidden files"), {K(Qt::CTRL | Qt::Key_H)}, [this] {
        m_left->setShowHidden(m_hiddenAction->isChecked());
        m_right->setShowHidden(m_hiddenAction->isChecked());
    });
    m_hiddenAction->setCheckable(true);
    addCommand(view, QT_TR_NOOP("Thumbnails"), {K(Qt::CTRL | Qt::SHIFT | Qt::Key_F1)},
               [this] { active()->toggleThumbnails(); });
    addCommand(view, QT_TR_NOOP("Zoom in"), {K(Qt::CTRL | Qt::Key_Equal), K(Qt::CTRL | Qt::Key_Plus)},
               [this] { active()->zoomBy(1); });
    addCommand(view, QT_TR_NOOP("Zoom out"), {K(Qt::CTRL | Qt::Key_Minus)}, [this] { active()->zoomBy(-1); });
    addCommand(view, QT_TR_NOOP("Normal size"), {K(Qt::CTRL | Qt::Key_0)}, [this] { active()->zoomBy(0); });
    view->addSeparator();
    addCommand(view, QT_TR_NOOP("Preview"), {K(Qt::CTRL | Qt::Key_Q)}, [this] { toggleQuickView(); });
    addCommand(view, QT_TR_NOOP("Preview: details instead of content"), {K(Qt::CTRL | Qt::Key_I)}, [this] {
        const bool on = !QSettings().value(QStringLiteral("preview/info"), false).toBool();
        if (!m_quickView)
            toggleQuickView();
        for (ViewerWindow* v : {m_quickView, m_quickView2})
            if (v)
                v->setShowInfo(on);
        QSettings().setValue(QStringLiteral("preview/info"), on);
    });
    view->addSeparator();
    addCommand(view, QT_TR_NOOP("Toggle menu bar"), {K(Qt::CTRL | Qt::SHIFT | Qt::Key_M)},
               [this] { menuBar()->setVisible(!menuBar()->isVisible()); });
    m_actions[QStringLiteral("refresh")] = addCommand(view, QT_TR_NOOP("Refresh"), {K(Qt::CTRL | Qt::Key_R)}, [this] { active()->refresh(); });

    // Tools: the bigger jobs, each with its own window or bar.
    QMenu* tools = bar->addMenu(tr("T&ools"));
    addCommand(tools, QT_TR_NOOP("Search files (names or contents)…"), {K(Qt::ALT | Qt::Key_F7), K(Qt::CTRL | Qt::SHIFT | Qt::Key_F)},
               [this] { active()->startSearchBar(); });
    addCommand(tools, QT_TR_NOOP("Find duplicate files…"), {K(Qt::ALT | Qt::SHIFT | Qt::Key_F7)}, [this] { findDuplicates(); });
    addCommand(tools, QT_TR_NOOP("Disk usage…"), {K(Qt::CTRL | Qt::SHIFT | Qt::Key_U)}, [this] { showDiskUsage(); });
    addCommand(tools, QT_TR_NOOP("Calculate all folder sizes"), {K(Qt::ALT | Qt::SHIFT | Qt::Key_Return)},
               [this] { active()->computeAllSizes(); });
    tools->addSeparator();
    m_actions[QStringLiteral("compare")] =
        addCommand(tools, QT_TR_NOOP("Compare folders (mark differences)"), {K(Qt::SHIFT | Qt::Key_F2)}, [this] { compareFolders(); });
    m_actions[QStringLiteral("sync")] =
        addCommand(tools, QT_TR_NOOP("Synchronize folders…"), {K(Qt::CTRL | Qt::SHIFT | Qt::Key_S)}, [this] { synchronizeFolders(); });
    tools->addSeparator();
    m_actions[QStringLiteral("pack")] = addCommand(tools, QT_TR_NOOP("Pack…"), {K(Qt::ALT | Qt::Key_F5)}, [this] { pack(); });
    m_actions[QStringLiteral("unpack")] =
        addCommand(tools, QT_TR_NOOP("Unpack to other panel…"), {K(Qt::ALT | Qt::Key_F9)}, [this] { unpack(); });
    m_actions[QStringLiteral("unpackHere")] =
        addCommand(tools, QT_TR_NOOP("Unpack here"), {K(Qt::ALT | Qt::SHIFT | Qt::Key_F9)}, [this] { unpack(true); });
    m_actions[QStringLiteral("checksums")] =
        addCommand(tools, QT_TR_NOOP("Checksums…"), {K(Qt::ALT | Qt::SHIFT | Qt::Key_C)}, [this] { showChecksums(); });
    tools->addSeparator();
    m_actions[QStringLiteral("command")] =
        addCommand(tools, QT_TR_NOOP("Run command…"), {K(Qt::CTRL | Qt::Key_Semicolon)}, [this] { openCommandBar(); });
    m_actions[QStringLiteral("terminal")] =
        addCommand(tools, QT_TR_NOOP("Open terminal here"), {K(Qt::CTRL | Qt::ALT | Qt::Key_T)}, [this] { openTerminal(); });
    m_pluginMenu = tools->addMenu(tr("Plugins"));
    m_pluginMenu->menuAction()->setVisible(false);  // until one is switched on
    tools->addSeparator();
    addCommand(tools, QT_TR_NOOP("Settings…"), {K(Qt::CTRL | Qt::Key_Comma)}, [this] { openSettings(); });

    // Switches the Settings dialog shows; not in the menu bar, but still
    // commands for the palette and the keys page.
    auto* prefs = new QMenu(tr("Settings"), this);
    QAction* vim = addCommand(prefs, QT_TR_NOOP("Modal keys (vim, yazi)"), {}, [this] {
        const bool on = m_actions.value(QStringLiteral("vim"))->isChecked();
        QSettings().setValue(QStringLiteral("input/vimKeys"), on);
        m_left->setVimKeys(on);
        m_right->setVimKeys(on);
        active()->showMessage(on ? tr("Modal keys on: letters are commands (F1 lists them)") : tr("Modal keys off"));
    });
    vim->setCheckable(true);
    m_actions[QStringLiteral("vim")] = vim;
    QAction* arrows = addCommand(prefs, QT_TR_NOOP("Arrow keys walk folders (← up, → into)"), {}, [this] {
        const bool on = m_actions.value(QStringLiteral("arrowFolders"))->isChecked();
        QSettings().setValue(QStringLiteral("input/arrowFolders"), on);
        m_left->setArrowFolders(on);
        m_right->setArrowFolders(on);
    });
    arrows->setCheckable(true);
    m_actions[QStringLiteral("arrowFolders")] = arrows;
    QAction* git = addCommand(prefs, QT_TR_NOOP("Show Git status"), {}, [this] {
        const bool on = m_actions.value(QStringLiteral("git"))->isChecked();
        QSettings().setValue(QStringLiteral("view/gitStatus"), on);
        DirTab::setGitEnabled(on);
        for (Panel* p : {m_left, m_right})
            if (p->tab())
                p->tab()->refreshGit();
    });
    git->setCheckable(true);
    m_actions[QStringLiteral("git")] = git;
    QAction* notes = addCommand(prefs, QT_TR_NOOP("Notify when long operations finish"), {}, [this] {
        QSettings().setValue(QStringLiteral("view/notifications"),
                             m_actions.value(QStringLiteral("notifications"))->isChecked());
    });
    notes->setCheckable(true);
    m_actions[QStringLiteral("notifications")] = notes;

    QMenu* help = bar->addMenu(tr("&Help"));
    addCommand(help, QT_TR_NOOP("Command palette…"), {K(Qt::CTRL | Qt::SHIFT | Qt::Key_P)}, [this] { commandPalette(); });
    addCommand(help, QT_TR_NOOP("Keyboard shortcuts"), {K(Qt::Key_F1)}, [this] { showShortcuts(); });
    applyKeyBindings(actions());  // keys changed in the settings
}

// On checksum files it checks; on anything else it computes.
void MainWindow::showChecksums()
{
    Panel* p = active();
    const QStringList paths = p->selectedPaths();
    if (paths.isEmpty() || refuseInArchive(p) || refuseOnServer(p, tr("Checksums")))
        return;
    ChecksumDialog dlg(paths, this);
    dlg.exec();
}

void MainWindow::showDiskUsage()
{
    Panel* p = active();
    if (refuseInArchive(p))
        return;
    DiskUsageDialog dlg(p->path(), p->isRemote(), this);
    connect(&dlg, &DiskUsageDialog::deleteRequested, this, [this](const QStringList& paths, bool toTrash) {
        auto* job = new DeleteJob(paths, toTrash);
        job->setStartImmediately(true);
        m_jobs->add(job);
    });
    connect(&dlg, &DiskUsageDialog::showRequested, this, [p](const QString& path) {
        p->navigate(parentPath(path), baseName(path));
    });
    dlg.exec();
}

void MainWindow::findDuplicates()
{
    Panel* p = active();
    if (refuseInArchive(p) || refuseOnServer(p, tr("Finding duplicates")))
        return;
    const QString otherDir = other()->inArchive() || other()->isRemote() ? QString() : other()->path();
    DuplicatesDialog dlg(p->path(), otherDir, this);
    connect(&dlg, &DuplicatesDialog::trashRequested, this, [this](const QStringList& paths) {
        auto* job = new DeleteJob(paths, true);
        job->setStartImmediately(true);
        m_jobs->add(job);
    });
    connect(&dlg, &DuplicatesDialog::showRequested, this, [p](const QString& path) {
        p->navigate(parentPath(path), baseName(path));
    });
    dlg.exec();
}

void MainWindow::showTrash()
{
#ifdef Q_OS_MACOS
    // Finder owns the private restore metadata. Never guess original paths.
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(QDir::homePath() + QStringLiteral("/.Trash"))))
        active()->showMessage(tr("Could not open Trash in Finder"), true);
    return;
#endif

    TrashDialog dlg(this);
    connect(&dlg, &TrashDialog::changed, this, [this] {
        for (Panel* p : {m_left, m_right})
            p->refresh();
    });
    dlg.exec();
}

void MainWindow::setPreviewPosition(const QString& code)
{
    QSettings().setValue(QStringLiteral("view/previewPosition"), code);
    if (m_quickView) {  // move it right away
        closePreview();
        toggleQuickView();
    }
}

void MainWindow::openSettings(int page)
{
    SettingsDialog dlg(this);
    dlg.showPage(page);
    dlg.exec();
}

QWidget* MainWindow::createFKeyBar()
{
    auto* bar = new QWidget(this);
    auto* l = new QHBoxLayout(bar);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(4);
    const std::pair<const char*, const char*> keys[] = {
        {"F3", QT_TR_NOOP("View")}, {"F4", QT_TR_NOOP("Edit")},       {"F5", QT_TR_NOOP("Copy")},
        {"F6", QT_TR_NOOP("Move")}, {"F7", QT_TR_NOOP("New folder")}, {"F8", QT_TR_NOOP("Delete")},
    };
    const std::function<void()> fns[] = {
        [this] { viewCurrent(); },
        [this] { editCurrent(); },
        [this] { copyOrMove(CopyJob::Mode::Copy); },
        [this] { copyOrMove(CopyJob::Mode::Move); },
        [this] { makeFolder(); },
        [this] { deleteSelected(false); },
    };
    for (size_t i = 0; i < std::size(keys); ++i) {
        auto* b = new FKeyButton(QLatin1String(keys[i].first), tr(keys[i].second), bar);
        connect(b, &QAbstractButton::clicked, this, fns[i]);
        l->addWidget(b);
    }
    return bar;
}

// The panel a preview belongs to, if w is (inside) one.
Panel* MainWindow::previewOwner(QWidget* w) const
{
    if (!w)
        return nullptr;
    if (m_quickView && (m_quickView == w || m_quickView->isAncestorOf(w)))
        return m_quickView2 ? m_left : active();
    if (m_quickView2 && (m_quickView2 == w || m_quickView2->isAncestorOf(w)))
        return m_right;
    return nullptr;
}

void MainWindow::returnFocusFromPreview(QWidget* w)
{
    if (Panel* owner = previewOwner(w))
        QTimer::singleShot(0, owner->view(), [v = QPointer<QWidget>(owner->view())] {
            if (v)
                v->setFocus(Qt::MouseFocusReason);
        });
}

bool MainWindow::eventFilter(QObject* obj, QEvent* e)
{
    if (e->type() == QEvent::MouseButtonPress && obj->isWidgetType() && (m_quickView || m_quickView2))
        returnFocusFromPreview(static_cast<QWidget*>(obj));
    return QMainWindow::eventFilter(obj, e);
}

// --------------------------------------------------------------- commands

void MainWindow::copyOrMove(CopyJob::Mode mode)
{
    Panel* src = active();
    Panel* dst = other();
    const QStringList paths = src->selectedPaths();
    if (paths.isEmpty())
        return;
    if (src->inArchive()) {
        if (dst->inArchive()) {
            src->showMessage(tr("From one archive into another: extract first (F5)"), true);
            return;
        }
        const bool move = mode == CopyJob::Mode::Move;
        QString why;
        if (move && !canUpdateArchive(src->archivePath(), &why)) {
            src->showMessage(why, true);
            return;
        }
        bool now = false;
        const QString dest = askDestination(this, move ? tr("Move out") : tr("Extract"),
                                            (move ? tr("Move %1 out of the archive to:") : tr("Extract %1 to:")).arg(describeItems(paths)),
                                            dst->path(), &now);
        if (dest.isEmpty())
            return;
        QStringList inner;
        for (const QString& p : paths)
            inner.append(src->innerPath(p));
        const QString archive = src->archivePath();
        auto* job = new ExtractJob(archive, inner, expandUserPath(dest));
        job->setStartImmediately(now);
        if (move) {  // then take them out of the archive, if all went well
            connect(job, &Job::finished, this, [this, job, archive, inner] {
                if (job->snapshot().state != Job::State::Done || !job->errors().isEmpty())
                    return;
                ArchiveChanges c;
                c.remove = inner;
                auto* rm = new ArchiveUpdateJob(archive, c, tr("Remove from %1").arg(baseName(archive)));
                rm->setStartImmediately(true);
                m_jobs->add(rm);
            });
        }
        m_jobs->add(job);
        if (!src->hasSelectionOverride())
            src->unmarkAll();
        return;
    }
    if (dst->inArchive() && src->isRemote()) {
        dst->showMessage(tr("Only local files can go into an archive: download them first"), true);
        return;
    }
    const QString title = mode == CopyJob::Mode::Copy ? tr("Copy") : tr("Move");
    QString initial = dst->path();
    if (!initial.endsWith(QLatin1Char('/')))
        initial += QLatin1Char('/');
    bool now = false;
    const QString text =
        askDestination(this, title, tr("%1 %2 to:").arg(title, describeItems(paths)), initial, &now);
    if (text.isEmpty())
        return;

    QString dest = text;
    if (!dest.startsWith(QLatin1Char('/')) && !dest.startsWith(QLatin1Char('~')) && !isRemoteLocation(dest))
        dest = joinPath(src->path(), dest);
    const bool intoDir = text.endsWith(QLatin1Char('/'));
    dest = expandUserPath(dest);
    // "name/" with a single source means: into that folder, even if new.
    if (intoDir && paths.size() == 1)
        dest = joinPath(dest, baseName(paths.first()));

    if (Panel* ap = archivePanelFor(dest)) {  // into an archive
        QString inner = ap->innerPath(intoDir && paths.size() == 1 ? parentPath(dest) : dest);
        if (!addToArchive(ap, paths, inner, mode == CopyJob::Mode::Move))
            return;
        if (!src->hasSelectionOverride())
            src->unmarkAll();
        return;
    }
    auto* job = new CopyJob(mode, paths, dest);
    job->setStartImmediately(now);
    m_jobs->add(job);
    if (!src->hasSelectionOverride())
        src->unmarkAll();
}

void MainWindow::deleteSelected(bool permanent)
{
    Panel* p = active();
    const QStringList paths = p->selectedPaths();
    if (paths.isEmpty())
        return;
    if (p->inArchive()) {
        QString why;
        if (!canUpdateArchive(p->archivePath(), &why)) {
            p->showMessage(why, true);
            return;
        }
        if (QMessageBox::question(this, tr("Delete from archive"),
                                  tr("Delete %1 from %2? This cannot be undone.")
                                      .arg(describeItems(paths), baseName(p->archivePath()))) != QMessageBox::Yes)
            return;
        ArchiveChanges c;
        for (const QString& path : paths)
            c.remove << p->innerPath(path);
        changeArchive(p, c, tr("Delete from %1").arg(baseName(p->archivePath())));
        if (!p->hasSelectionOverride())
            p->unmarkAll();
        return;
    }
    if (p->isRemote())
        permanent = true;  // servers have no trash
    QMessageBox box(this);
    box.setIcon(permanent ? QMessageBox::Warning : QMessageBox::Question);
    box.setWindowTitle(permanent ? tr("Delete permanently") : tr("Move to trash"));
    box.setText(permanent ? tr("Permanently delete %1?").arg(describeItems(paths))
                          : tr("Move %1 to the trash?").arg(describeItems(paths)));
    if (permanent)
        box.setInformativeText(p->isRemote() ? tr("This is on a server, which has no trash. It cannot be undone.")
                                             : tr("This cannot be undone."));
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    box.setDefaultButton(QMessageBox::Yes);
    if (box.exec() != QMessageBox::Yes)
        return;
    m_jobs->add(new DeleteJob(paths, !permanent));
    if (!p->hasSelectionOverride())
        p->unmarkAll();
}

void MainWindow::makeFolder(const QString& given)
{
    Panel* p = active();
    QString why;
    if (p->inArchive() && !canUpdateArchive(p->archivePath(), &why)) {
        p->showMessage(why, true);
        return;
    }
    bool ok = !given.isEmpty();
    const QString name = ok ? given
                            : QInputDialog::getText(this, tr("New folder"), tr("Name (use / for nested folders):"),
                                                    QLineEdit::Normal, QString(), &ok)
                                  .trimmed();
    if (!ok || name.isEmpty())
        return;
    if (p->inArchive()) {
        ArchiveChanges c;
        c.mkdir << joinPath(p->innerPath(p->path()), name);
        if (changeArchive(p, c, tr("New folder in %1").arg(baseName(p->archivePath()))))
            p->expectName(name.section(QLatin1Char('/'), 0, 0, QString::SectionSkipEmpty));
        return;
    }
    const QString full = name.startsWith(QLatin1Char('/')) && !p->isRemote() ? name : joinPath(p->path(), name);
    const QString first = name.section(QLatin1Char('/'), 0, 0, QString::SectionSkipEmpty);
    QPointer<Panel> panel(p);
    QPointer<MainWindow> self(this);
    ioPool()->start([self, panel, full, first] {
        // For undo: the topmost folder that didn't exist yet.
        QString topNew = full;
        const bool remote = isRemoteLocation(full);
        for (QString q = full; !remote && q != QLatin1String("/") && !q.isEmpty(); q = parentPath(q)) {
            if (QFileInfo::exists(q))
                break;
            topNew = q;
        }
        const int err = makeDirectoriesAt(full);
        postToUi([self, panel, err, full, first, topNew, remote] {
            if (self && !err && !remote)
                self->pushUndo({UndoEntry::Kind::Create, tr("new folder %1").arg(baseName(topNew)), {}, {{topNew, {}}}});
            if (!panel)
                return;
            if (err)
                panel->showMessage(QObject::tr("Cannot create %1: %2").arg(full, errnoString(err)), true);
            else
                panel->expectName(first);
        });
    });
}

void MainWindow::viewCurrent()
{
    Panel* p = active();
    const FileEntry* e = p->currentEntry();
    if (!e)
        return;
    if (e->isDir) {
        p->computeCurrentSize();
        return;
    }
    if (p->inArchive() || p->isRemote()) {
        withLocalCopy(p, p->currentPath(), [this](const QString& local) {
            (new ViewerWindow({local}, 0, this))->show();
        });
        return;
    }
    // The viewer can step through all files of the folder with N / P.
    QStringList files;
    int index = 0;
    const FileModel* m = p->tab()->model();
    for (int r = 0; r < m->count(); ++r) {
        const FileEntry* f = m->entry(r);
        if (f->isDir)
            continue;
        if (f == e)
            index = int(files.size());
        files.append(m->filePath(r));
    }
    auto* viewer = new ViewerWindow(files, index, this);
    viewer->show();
}

void MainWindow::editCurrent()
{
    const QString path = active()->currentPath();
    const FileEntry* e = active()->currentEntry();
    if (path.isEmpty() || !e || e->isDir)
        return;
    if (active()->inArchive()) {
        editInArchive(active(), path);
        return;
    }
    if (isRemoteLocation(path)) {
        editRemote(active(), path);
        return;
    }
    const QString editor = QSettings().value(QStringLiteral("tools/editor")).toString();
    if (!editor.isEmpty()) {
        QStringList args = QProcess::splitCommand(editor);
        const QString program = args.takeFirst();
        args.append(path);
        if (QProcess::startDetached(program, args, active()->path()))
            return;
    }
    openFile(path);
}

// Mime types for a file name, most specific first. Name-based only: no disk I/O.
static QStringList mimeChainFor(const QString& path)
{
    static QMimeDatabase db;
    QStringList chain;
    const QList<QMimeType> types = db.mimeTypesForFileName(baseName(path));
    if (!types.isEmpty())
        chain << types.first().name() << types.first().aliases() << types.first().allAncestors();
    chain << QStringLiteral("application/octet-stream");
    return chain;
}

void MainWindow::openFile(const QString& path)
{
    // Launch the default app ourselves: xdg-open / the portal may show an
    // app chooser every time on some desktops, and this is faster anyway.
    DesktopApps& apps = DesktopApps::instance();
    if (apps.isLoaded()) {
        const DesktopApp app = apps.defaultFor(mimeChainFor(path));
        if (!app.id.isEmpty() && app.id != QLatin1String("tandem-fm.desktop")) {
            if (DesktopApps::launch(app, {path}, parentPath(path), apps.defaultTerminal()))
                return;
        }
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
        active()->showMessage(tr("No application to open %1").arg(baseName(path)), true);
}

void MainWindow::handleDrop(const QList<QUrl>& urls, const QString& targetDir, Qt::DropAction action)
{
    QStringList paths = localPaths(urls);
    // Dropping something onto the folder it already lives in does nothing.
    paths.removeIf([&](const QString& p) { return parentPath(p) == targetDir || p == targetDir; });
    if (paths.isEmpty())
        return;
    const auto mode = action == Qt::MoveAction ? CopyJob::Mode::Move : CopyJob::Mode::Copy;
    if (Panel* ap = archivePanelFor(targetDir)) {
        addToArchive(ap, paths, ap->innerPath(targetDir), mode == CopyJob::Mode::Move);
        return;
    }
    m_jobs->add(new CopyJob(mode, paths, targetDir));
}

void MainWindow::copyToClipboard(bool cut)
{
    const QStringList paths = active()->selectedPaths();
    if (paths.isEmpty())
        return;
    if (active()->inArchive()) {
        active()->showMessage(tr("The clipboard can't hold archive contents yet. Use F5 to extract."), true);
        return;
    }
    auto* mime = new QMimeData;
    QList<QUrl> urls;
    QByteArray gnome = cut ? "cut" : "copy";
    for (const QString& p : paths) {
        const QUrl u = QUrl::fromLocalFile(p);
        urls.append(u);
        gnome += '\n' + u.toEncoded();
    }
    mime->setUrls(urls);
    mime->setData(QStringLiteral("x-special/gnome-copied-files"), gnome);
    mime->setData(QStringLiteral("application/x-kde-cutselection"), cut ? "1" : "0");
    mime->setText(paths.join(QLatin1Char('\n')));
    QApplication::clipboard()->setMimeData(mime);
    active()->showMessage(cut ? tr("Cut %1").arg(describeItems(paths)) : tr("Copied %1").arg(describeItems(paths)));
}

void MainWindow::copyPathsAsText()
{
    const QStringList paths = active()->selectedPaths();
    if (paths.isEmpty())
        return;
    QApplication::clipboard()->setText(paths.join(QLatin1Char('\n')));
    active()->showMessage(plural(tr("Copied %n path(s)", nullptr, int(paths.size())), int(paths.size())));
}

void MainWindow::paste()
{
    pasteInto(active()->path());
}

void MainWindow::pasteInto(const QString& dir)
{
    const QMimeData* mime = QApplication::clipboard()->mimeData();
    if (!mime || !mime->hasUrls())
        return;
    const QStringList paths = localPaths(mime->urls());
    if (paths.isEmpty())
        return;
    const bool cut = mime->data(QStringLiteral("x-special/gnome-copied-files")).startsWith("cut") ||
                     mime->data(QStringLiteral("application/x-kde-cutselection")) == "1";
    if (Panel* ap = archivePanelFor(dir)) {
        if (addToArchive(ap, paths, ap->innerPath(dir), cut) && cut)
            QApplication::clipboard()->clear();
        return;
    }
    m_jobs->add(new CopyJob(cut ? CopyJob::Mode::Move : CopyJob::Mode::Copy, paths, dir));
    if (cut)
        QApplication::clipboard()->clear();
}

void MainWindow::swapPanels()
{
    const QString l = m_left->path();
    const QString r = m_right->path();
    m_left->navigate(r);
    m_right->navigate(l);
}

void MainWindow::openInPanel(Panel* target)
{
    const FileEntry* e = active()->currentEntry();
    const QString p = (e && e->isDir && !e->isParent) ? active()->currentPath() : active()->path();
    target->navigate(p);
}

void MainWindow::showShortcuts()
{
    ShortcutsDialog dlg(this);
    dlg.exec();
}

// --------------------------------------------------------------- quick view

// A third column right of both panels: copy/move targets stay visible.
QString MainWindow::previewMode() const
{
    const QString m = QSettings().value(QStringLiteral("view/previewPosition"), QStringLiteral("each")).toString();
    return m == QLatin1String("bottom") || m == QLatin1String("right") ? m : QStringLiteral("each");
}

void MainWindow::closePreview()
{
    if (!m_quickView)
        return;
    QSettings settings;
    if (m_quickView2)
        settings.setValue(QStringLiteral("view/previewHeightEach"), m_leftCol->sizes().value(1));
    else if (m_vsplit->indexOf(m_quickView) >= 0)
        settings.setValue(QStringLiteral("view/previewHeight"), m_vsplit->sizes().value(1));
    else
        settings.setValue(QStringLiteral("view/previewWidth"), m_splitter->sizes().value(2));
    for (ViewerWindow* v : {m_quickView, m_quickView2}) {
        if (!v)
            continue;
        v->hide();
        v->deleteLater();
    }
    m_quickView = m_quickView2 = nullptr;
}

// Previews: one below each panel (default), one below both, or one on the
// right. Never in place of a panel.
void MainWindow::toggleQuickView()
{
    QSettings settings;
    if (m_quickView) {
        closePreview();
        settings.setValue(QStringLiteral("view/preview"), false);
        active()->view()->setFocus();
        return;
    }
    auto makePreview = [this](const char* name) {
        auto* v = new ViewerWindow({}, -1, this, true);
        v->setObjectName(QLatin1String(name));
        return v;
    };
    const QString mode = previewMode();
    if (mode == QLatin1String("each")) {
        int total = m_leftCol->height() - m_leftCol->handleWidth();
        if (total < 300)
            total = 700;  // not laid out yet; the splitter rescales proportionally later
        const int height = qBound(140, settings.value(QStringLiteral("view/previewHeightEach"), total * 2 / 5).toInt(),
                                  qMax(140, total * 2 / 3));
        m_quickView = makePreview("quickView");
        m_quickView2 = makePreview("quickViewRight");
        m_leftCol->addWidget(m_quickView);
        m_rightCol->addWidget(m_quickView2);
        for (QSplitter* col : {m_leftCol, m_rightCol})
            col->setSizes({qMax(150, total - height), height});
    } else if (mode == QLatin1String("bottom")) {
        int total = m_vsplit->height() - m_vsplit->handleWidth();
        if (total < 300)
            total = 700;
        const int height = qBound(160, settings.value(QStringLiteral("view/previewHeight"), total * 2 / 5).toInt(),
                                  qMax(160, total * 2 / 3));
        m_quickView = makePreview("quickView");
        m_vsplit->addWidget(m_quickView);
        m_vsplit->setSizes({qMax(150, total - height), height});
    } else {
        int total = m_splitter->width() - 2 * m_splitter->handleWidth();
        if (total < 400)
            total = 1200;
        const int width = qBound(220, settings.value(QStringLiteral("view/previewWidth"), total * 3 / 10).toInt(),
                                 qMax(220, total / 2));
        m_quickView = makePreview("quickView");
        m_splitter->addWidget(m_quickView);
        // Both panels give up room in proportion; their ratio stays.
        const int remaining = qMax(200, total - width);
        const int left = int(remaining * m_panelRatio);
        m_splitter->setSizes({left, remaining - left, width});
    }
    settings.setValue(QStringLiteral("view/preview"), true);
    updateQuickView();
    active()->view()->setFocus();
}

void MainWindow::updateQuickView()
{
    if (m_quickView2) {  // each panel previews its own cursor
        updatePreviewFor(m_quickView, m_left);
        updatePreviewFor(m_quickView2, m_right);
    } else if (m_quickView) {
        updatePreviewFor(m_quickView, active());
    }
}

void MainWindow::updatePreviewFor(ViewerWindow* preview, Panel* p)
{
    const FileEntry* e = p->currentEntry();
    if (!e || e->isParent) {
        preview->showNote(tr("Move the cursor onto a file"));
    } else if (p->inArchive()) {
        preview->showNote(tr("“%1” is inside an archive\n\nF3 opens it in the viewer").arg(e->name));
    } else if (p->isRemote()) {
        // On a server: small files are fetched (and cached) for the preview.
        const QString remote = p->currentPath();
        if (preview->property("remoteWanted").toString() == remote)
            return;
        preview->setProperty("remoteWanted", remote);
        if (e->isDir) {  // its contents; the total in one request afterwards
            preview->showFile(remote);
            QPointer<ViewerWindow> pv(preview);
            mediaPool()->start([pv, remote] {
                const int64_t total = directorySizeAt(remote, nullptr);
                postToUi([pv, remote, total] {
                    if (pv)
                        pv->setFolderTotal(remote, total);
                });
            });
            return;
        }
        if (e->size > 16 * 1024 * 1024) {
            preview->showNote(tr("“%1” is on a server (%2)\n\nF3 downloads and opens it").arg(e->name, formatSize(e->size)));
            return;
        }
        const QString local = remoteCachePath(remote);
        QPointer<ViewerWindow> pv(preview);
        const int64_t size = e->size, mtime = e->mtime;
        preview->showNote(tr("Fetching “%1” …").arg(e->name));
        mediaPool()->start([pv, remote, local, size, mtime] {
            const int err = cachedCopyIsFresh(local, size, mtime) ? 0 : downloadFile(remote, local, nullptr);
            postToUi([pv, remote, local, err] {
                if (!pv || pv->property("remoteWanted").toString() != remote)
                    return;
                if (err)
                    pv->showNote(tr("Cannot fetch: %1").arg(errnoString(err)));
                else
                    pv->showFile(local);
            });
        });
    } else if (preview->currentPath() != p->currentPath()) {
        preview->setProperty("remoteWanted", QVariant());
        preview->setGitHint(p->tab()->model()->gitStatus(e->name));
        preview->showFile(p->currentPath());
        // Prepare the neighbours, so moving on is instant (like yazi's preloaders).
        const FileModel* m = p->tab()->model();
        const int row = p->view()->currentRow();
        QStringList around;
        for (int r : {row + 1, row + 2, row - 1, row + 3}) {
            const FileEntry* n = m->entry(r);
            if (n && !n->isDir && !n->isParent)
                around.append(m->filePath(r));
        }
        preview->preload(around);
    }
}

// ------------------------------------------------- command palette & jump

void MainWindow::commandPalette()
{
    QList<PopupItem> items;
    for (QAction* a : actions()) {
        if (a->text().isEmpty() || !a->isEnabled() || a->menu())
            continue;
        QString title = a->text().remove(QLatin1Char('&'));
        QString hint = a->shortcut().toString(QKeySequence::NativeText);
        if (const int tab = int(title.indexOf(QLatin1Char('\t'))); tab >= 0) {
            hint = title.mid(tab + 1);
            title.truncate(tab);
        }
        if (a->isCheckable())
            title += a->isChecked() ? tr("  (on)") : tr("  (off)");
        PopupItem it;
        it.title = title;
        it.subtitle = a->property("category").toString();
        it.hint = hint;
        it.icon = a->icon();
        QPointer<QAction> action(a);
        it.run = [action] {
            if (action)
                action->trigger();
        };
        items.append(it);
    }
    std::sort(items.begin(), items.end(), [](const PopupItem& a, const PopupItem& b) {
        return QString::compare(a.title, b.title, Qt::CaseInsensitive) < 0;
    });
    m_popup->keyHandler = nullptr;
    m_popup->open(tr("Type a command…"), items);
}

QList<PopupItem> MainWindow::folderItems() const
{
    static const QIcon pin = IconCache::themed("pin", "emblem-favorite");
    static const QIcon folder = IconCache::themed("folder");
    const QString home = QDir::homePath();
    QList<PopupItem> items;
    for (const FolderHistory::Item& f : FolderHistory::instance().items()) {
        PopupItem it;
        it.key = f.path;
        it.title = f.path == QLatin1String("/") ? f.path : baseName(f.path);
        it.subtitle = f.path.startsWith(home) ? QLatin1Char('~') + f.path.mid(home.size()) : f.path;
        it.matchSubtitle = true;
        it.icon = f.pinned ? pin : folder;
        // Pins on top; otherwise frecency nudges the fuzzy ranking (zoxide-like).
        it.bonus = f.pinned ? 1000 : int(12 * std::log2(1 + f.frecency));
        it.hint = !f.key.isEmpty() ? f.key : (f.pinned ? tr("pinned") : QString());
        const QString path = f.path;
        it.run = [this, path] { active()->navigate(path); };
        items.append(it);
    }
    return items;
}

void MainWindow::jumpToFolder()
{
    m_popup->keyHandler = [this](QKeyEvent* e, const PopupItem* it) {
        if (!it)
            return false;
        if ((e->modifiers() & Qt::ControlModifier) && e->key() == Qt::Key_P) {
            FolderHistory::instance().setPinned(it->key, !FolderHistory::instance().isPinned(it->key));
            m_popup->setItems(folderItems());
            return true;
        }
        if ((e->modifiers() & Qt::ShiftModifier) && e->key() == Qt::Key_Delete) {
            FolderHistory::instance().forget(it->key);
            m_popup->setItems(folderItems());
            return true;
        }
        return false;
    };
    m_popup->open(tr("Jump to a folder: type parts of its path"), folderItems(),
                  tr("Enter: go   ·   Ctrl+P: pin / unpin   ·   Shift+Del: forget   ·   Ctrl+D: bookmark keys"));
}

// ------------------------------------------------------------ icon themes

void MainWindow::setZoom(bool grid, int value)
{
    if (grid) {
        ThumbGrid::setDefaultSize(value);
        QSettings().setValue(QStringLiteral("view/gridSize"), ThumbGrid::defaultSize());
    } else {
        FileView::setListZoom(value);
        QSettings().setValue(QStringLiteral("view/listZoom"), FileView::listZoom());
    }
    for (Panel* p : {m_left, m_right}) {
        if (grid && p->view()->thumbGrid())
            p->view()->thumbGrid()->setCellSize(ThumbGrid::defaultSize());
        else if (!grid)
            p->view()->zoomChanged();
        p->syncZoom();
    }
}

void MainWindow::repaintFileViews()
{
    for (Panel* p : {m_left, m_right}) {
        p->view()->viewport()->update();
        if (p->view()->thumbGrid())
            p->view()->thumbGrid()->update();
    }
}

void MainWindow::setIconTheme(const QString& id)
{
    QSettings().setValue(QStringLiteral("view/iconTheme"), id);
    IconCache::setTheme(id);
    if (id.isEmpty())
        applyTheme(*qApp);  // the desktop's icons, in the variant that suits the theme
    for (Panel* p : {m_left, m_right})
        p->view()->viewport()->update();
    update();
}

// ----------------------------------------------------------------- archives

bool MainWindow::refuseInArchive(Panel* p)
{
    if (!p->inArchive())
        return false;
    p->showMessage(tr("This doesn't work inside archives. F5 extracts."), true);
    return true;
}

Panel* MainWindow::archivePanelFor(const QString& path) const
{
    for (Panel* p : {active(), other()})
        if (p->inArchive() && (path == p->archivePath() || path.startsWith(p->archivePath() + QLatin1Char('/'))))
            return p;
    return nullptr;
}

bool MainWindow::changeArchive(Panel* p, ArchiveChanges changes, const QString& title)
{
    QString why;
    if (!canUpdateArchive(p->archivePath(), &why)) {
        p->showMessage(why, true);
        return false;
    }
    auto* job = new ArchiveUpdateJob(p->archivePath(), std::move(changes), title);
    job->setStartImmediately(true);  // small edits shouldn't wait behind a big copy
    m_jobs->add(job);
    return true;
}

bool MainWindow::addToArchive(Panel* p, const QStringList& localPaths, const QString& innerDir, bool move)
{
    for (const QString& s : localPaths) {
        if (isRemoteLocation(s) || !QFileInfo::exists(s)) {
            p->showMessage(tr("Only local files can go into an archive"), true);
            return false;
        }
        if (s == p->archivePath()) {
            p->showMessage(tr("An archive can't go into itself"), true);
            return false;
        }
    }
    ArchiveChanges c;
    for (const QString& s : localPaths)
        c.add.append({s, joinPath(innerDir, baseName(s))});
    c.moveSources = move;
    const QString name = baseName(p->archivePath());
    return changeArchive(p, c, move ? tr("Move into %1").arg(name) : tr("Copy into %1").arg(name));
}

void MainWindow::withExtracted(Panel* p, const QString& path, std::function<void(const QString&)> fn)
{
    const QString archive = p->archivePath();
    const QString inner = p->innerPath(path);
    p->showMessage(tr("Extracting %1 …").arg(baseName(path)));
    QPointer<Panel> panel(p);
    QPointer<MainWindow> self(this);
    ioPool()->start([self, panel, archive, inner, fn] {
        QString error;
        const QString local = extractToCache(archive, inner, &error);
        postToUi([self, panel, local, error, fn] {
            if (!self)
                return;
            if (local.isEmpty()) {
                if (panel)
                    panel->showMessage(tr("Cannot extract: %1").arg(error), true);
                return;
            }
            if (panel)
                panel->showMessage(QString());
            fn(local);
        });
    });
}

void MainWindow::openFrom(Panel* p, const QString& path)
{
    if (p->inArchive() || isRemoteLocation(path))
        withLocalCopy(p, path, [this](const QString& local) { openFile(local); });
    else
        openFile(path);
}

bool MainWindow::refuseOnServer(Panel* p, const QString& what)
{
    if (!p->isRemote())
        return false;
    p->showMessage(tr("%1 doesn't work on servers yet").arg(what), true);
    return true;
}

void MainWindow::withLocalCopy(Panel* p, const QString& path, std::function<void(const QString&)> fn,
                               const QString& localOverride)
{
    if (p->inArchive()) {
        withExtracted(p, path, std::move(fn));
        return;
    }
    if (!isRemoteLocation(path)) {
        fn(path);
        return;
    }
    const QString local = localOverride.isEmpty() ? remoteCachePath(path) : localOverride;
    p->showMessage(tr("Downloading %1 …").arg(baseName(path)));
    QPointer<Panel> panel(p);
    QPointer<MainWindow> self(this);
    ioPool()->start([self, panel, path, local, fn] {
        FileStat st;
        int err = statAt(path, st);
        if (err == 0 && !cachedCopyIsFresh(local, st.size, st.mtime))
            err = downloadFile(path, local, nullptr);
        postToUi([self, panel, path, local, err, fn] {
            if (!self)
                return;
            if (err) {
                if (panel)
                    panel->showMessage(tr("Cannot download %1: %2").arg(baseName(path), errnoString(err)), true);
                return;
            }
            if (panel)
                panel->showMessage(QString());
            fn(local);
        });
    });
}

void MainWindow::editedFileChanged(const QString& local)
{
    m_changedEdits.insert(local);
    m_uploadTimer->start();
}

void MainWindow::uploadEdited()
{
    for (const QString& f : std::as_const(m_changedEdits)) {
        if (!QFileInfo::exists(f))
            continue;  // mid-save; the new file triggers again
        if (!m_editWatch->files().contains(f))
            m_editWatch->addPath(f);
        if (const auto it = m_editedArchive.constFind(f); it != m_editedArchive.cend()) {
            ArchiveChanges c;
            c.add.append({f, it->second});
            c.replaceExisting = true;
            auto* job = new ArchiveUpdateJob(it->first, c, tr("Save %1 into %2").arg(baseName(it->second), baseName(it->first)));
            job->setStartImmediately(true);
            m_jobs->add(job);
            active()->showMessage(tr("Saving %1 into %2 …").arg(baseName(it->second), baseName(it->first)));
            continue;
        }
        const QString remote = m_editedRemote.value(f);
        if (remote.isEmpty())
            continue;
        auto* job = new CopyJob(QList<QPair<QString, QString>>{{f, remote}});
        job->setOverwriteExisting();
        job->setStartImmediately(true);
        m_jobs->add(job);
        active()->showMessage(tr("Uploading %1 …").arg(baseName(remote)));
    }
    m_changedEdits.clear();
}

// F4 on a server: edit a downloaded copy; every save is uploaded again.
void MainWindow::editRemote(Panel* p, const QString& path)
{
    const QString editCopy = remoteCachePath(path).replace(QStringLiteral("/tandem-fm/remote/"), QStringLiteral("/tandem-fm/edit/"));
    if (m_editedRemote.value(editCopy) == path && QFileInfo::exists(editCopy)) {
        // Already being edited: reopen that copy, don't fetch over changes.
        const QString editor = QSettings().value(QStringLiteral("tools/editor")).toString();
        QStringList args = QProcess::splitCommand(editor);
        if (args.isEmpty() || !QProcess::startDetached(args.takeFirst(), args << editCopy, parentPath(editCopy)))
            openFile(editCopy);
        return;
    }
    withLocalCopy(p, path, [this, path](const QString& local) {
        m_editedRemote.insert(local, path);
        watchEdit(local);
    }, editCopy);
}

// Watches an edit copy (every save goes back) and opens it in the editor.
void MainWindow::watchEdit(const QString& local)
{
    if (!m_editWatch) {
        m_editWatch = new QFileSystemWatcher(this);
        m_uploadTimer = new QTimer(this);
        m_uploadTimer->setSingleShot(true);
        m_uploadTimer->setInterval(400);  // editors write in several steps
        connect(m_editWatch, &QFileSystemWatcher::fileChanged, this, &MainWindow::editedFileChanged);
        // Editors that save by writing a new file and renaming it drop the
        // file watch; the folder watch catches those.
        connect(m_editWatch, &QFileSystemWatcher::directoryChanged, this, [this](const QString& dir) {
            for (const QString& f : m_editedRemote.keys() + m_editedArchive.keys())
                if (parentPath(f) == dir && !m_editWatch->files().contains(f))
                    editedFileChanged(f);
        });
        connect(m_uploadTimer, &QTimer::timeout, this, &MainWindow::uploadEdited);
    }
    m_editWatch->addPath(local);
    m_editWatch->addPath(parentPath(local));
    const QString editor = QSettings().value(QStringLiteral("tools/editor")).toString();
    if (!editor.isEmpty()) {
        QStringList args = QProcess::splitCommand(editor);
        const QString program = args.takeFirst();
        args.append(local);
        if (QProcess::startDetached(program, args, parentPath(local)))
            return;
    }
    openFile(local);
}

// F4 inside an archive: edit a copy; every save is written back into the archive.
void MainWindow::editInArchive(Panel* p, const QString& path)
{
    QString why;
    if (!canUpdateArchive(p->archivePath(), &why)) {
        p->showMessage(tr("%1. Opening a read-only copy.").arg(why), true);
        openFrom(p, path);
        return;
    }
    const QString archive = p->archivePath();
    const QString inner = p->innerPath(path);
    const QString editCopy =
        QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/tandem-fm/edit/archive/") +
        QString::fromLatin1(QCryptographicHash::hash((archive + QLatin1Char('\n') + inner).toUtf8(), QCryptographicHash::Sha1)
                                .toHex()
                                .left(16)) +
        QLatin1Char('/') + baseName(inner);
    if (m_editedArchive.value(editCopy) == qMakePair(archive, inner) && QFileInfo::exists(editCopy)) {
        watchEdit(editCopy);  // already being edited: reopen, don't extract over changes
        return;
    }
    withExtracted(p, path, [this, editCopy, archive, inner](const QString& extracted) {
        QDir().mkpath(parentPath(editCopy));
        QFile::remove(editCopy);
        if (!QFile::copy(extracted, editCopy)) {
            active()->showMessage(tr("Cannot copy %1 for editing").arg(baseName(inner)), true);
            return;
        }
        QFile::setPermissions(editCopy, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);
        m_editedArchive.insert(editCopy, {archive, inner});
        watchEdit(editCopy);
    });
}

void MainWindow::pack()
{
    Panel* src = active();
    const QStringList paths = src->selectedPaths();
    if (paths.isEmpty())
        return;
    if (src->inArchive()) {
        src->showMessage(tr("Extract first (F5), then pack."), true);
        return;
    }
    if (refuseOnServer(src, tr("Packing")) || refuseOnServer(other(), tr("Packing")))
        return;
    bool now = false;
    const QString base = paths.size() == 1 ? baseName(paths.first()) : baseName(src->path());
    const QString dir = other()->inArchive() ? src->path() : other()->path();
    const QString text = askDestination(
        this, tr("Pack"),
        tr("Pack %1 into (the extension picks the format: .zip .7z .tar.gz .tar.xz .tar.zst .tar):")
            .arg(describeItems(paths)),
        joinPath(dir, base + QStringLiteral(".zip")), &now);
    if (text.isEmpty())
        return;
    QString target = text.startsWith(QLatin1Char('/')) || text.startsWith(QLatin1Char('~')) ? text
                                                                                             : joinPath(src->path(), text);
    auto* packJob = new PackJob(paths, expandUserPath(target));
    packJob->setStartImmediately(now);
    m_jobs->add(packJob);
    if (!src->hasSelectionOverride())
        src->unmarkAll();
}

void MainWindow::unpack(bool here)
{
    Panel* src = active();
    Panel* dst = here ? src : other();
    if (dst->inArchive()) {
        refuseInArchive(dst);
        return;
    }
    if (refuseOnServer(src, tr("Unpacking")) || refuseOnServer(dst, tr("Unpacking")))
        return;
    bool now = false;
    QStringList archives;
    for (const QString& p : src->selectedPaths())
        if (isArchiveName(baseName(p)))
            archives.append(p);
    if (src->inArchive() || archives.isEmpty()) {
        src->showMessage(tr("Select one or more archives to unpack."), true);
        return;
    }
    const QString dest = here ? dst->path()
                              : askDestination(this, tr("Unpack"),
                                               tr("Unpack %1 to (archives with several top-level items get their own "
                                                  "folder):")
                                                   .arg(describeItems(archives)),
                                               dst->path(), &now);
    if (dest.isEmpty())
        return;
    for (const QString& a : std::as_const(archives)) {
        auto* job = new ExtractJob(a, {QString()}, expandUserPath(dest), true);
        job->setStartImmediately(now);
        m_jobs->add(job);
    }
    if (!src->hasSelectionOverride())
        src->unmarkAll();
}

// ------------------------------------------------------------ multi-rename

bool MainWindow::collectRenameItems(Panel* p, QList<RenameItem>& items, QSet<QString>& existing)
{
    if (refuseInArchive(p))
        return false;
    const FileModel* m = p->tab()->model();
    for (const QString& path : p->selectedPaths()) {
        const FileEntry* e = m->entry(m->rowOf(baseName(path)));
        if (e)
            items.append({e->name, e->isDir, e->mtime});
    }
    for (int r = 0; r < m->count(); ++r)
        if (!m->entry(r)->isParent)
            existing.insert(m->entry(r)->name);
    return !items.isEmpty();
}

// Ctrl+Shift+F5: links to the selected items, in the other panel by default.
void MainWindow::createLink()
{
    Panel* p = active();
    const QStringList targets = p->selectedPaths();
    if (targets.isEmpty() || refuseInArchive(p) || other()->inArchive())
        return;
    bool anyDir = false;
    for (const QString& t : targets)
        anyDir |= QFileInfo(t).isDir();
    const bool remote = p->isRemote() || other()->isRemote();
    const bool single = targets.size() == 1;

    QDialog dlg(this);
    dlg.setWindowTitle(single ? tr("Link to %1").arg(baseName(targets.first())) : plural(tr("Links to %n item(s)", nullptr, int(targets.size())), int(targets.size())));
    dlg.setMinimumWidth(560);
    auto* layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(8);
    layout->addWidget(new QLabel(single ? tr("Create the link as:") : tr("Create the links in:"), &dlg));
    auto* where = new QLineEdit(single ? joinPath(other()->path(), baseName(targets.first())) : other()->path(), &dlg);
    layout->addWidget(where);
    setThemeStyle(&dlg, QStringLiteral(
        "QToolButton#segment { padding: 5px 14px; border: 1px solid @control; background: @base; }"
        "QToolButton#segment:checked { background: @accent; color: @accentText; border-color: @accent; }"));
    auto* kinds = new QHBoxLayout;
    kinds->setSpacing(0);
    auto* group = new QButtonGroup(&dlg);
    const QList<std::tuple<LinkKind, QString, QString>> choices = {
        {LinkKind::Symbolic, tr("Symbolic"), tr("Points to the full path; works anywhere, breaks if the target moves")},
        {LinkKind::Relative, tr("Relative"), tr("Points to ../the/target; keeps working when both move together (backups, projects)")},
        {LinkKind::Hard, tr("Hard link"), tr("A second name for the same file: same drive only, files only, no breaking")},
    };
    auto* help = new QLabel(&dlg);
    help->setWordWrap(true);
    setThemeStyle(help, QStringLiteral("color: @dimText;"));
    const int saved = QSettings().value(QStringLiteral("links/kind"), 0).toInt();
    for (const auto& [kind, label, tip] : choices) {
        auto* b = new QToolButton(&dlg);
        b->setObjectName(QStringLiteral("segment"));
        b->setText(label);
        b->setCheckable(true);
        b->setEnabled(!(kind == LinkKind::Hard && (anyDir || remote)));
        group->addButton(b, int(kind));
        kinds->addWidget(b);
        connect(b, &QToolButton::toggled, help, [help, tip](bool on) {
            if (on)
                help->setText(tip);
        });
    }
    kinds->addStretch(1);
    QAbstractButton* start = group->button(saved);
    (start && start->isEnabled() ? start : group->button(0))->setChecked(true);
    layout->addLayout(kinds);
    layout->addWidget(help);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(single ? tr("Create link") : tr("Create links"));
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);
    where->setFocus();
    where->selectAll();
    if (dlg.exec() != QDialog::Accepted)
        return;
    const auto kind = LinkKind(group->checkedId());
    QSettings().setValue(QStringLiteral("links/kind"), int(kind));
    QString dest = expandUserPath(where->text().trimmed());
    if (!dest.startsWith(QLatin1Char('/')) && !isRemoteLocation(dest))
        dest = joinPath(p->path(), dest);
    QList<QPair<QString, QString>> made;
    QStringList errors;
    for (const QString& t : targets) {
        const QString link = single && !QFileInfo(dest).isDir() ? dest : joinPath(dest, baseName(t));
        if (const int e = makeLink(t, link, kind))
            errors << tr("%1: %2").arg(baseName(link), errnoString(e));
        else
            made.append({link, QString()});
    }
    if (!made.isEmpty())
        pushUndo({UndoEntry::Kind::Create, plural(tr("%n link(s)", nullptr, int(made.size())), int(made.size())), {}, made});
    p->showMessage(errors.isEmpty() ? plural(tr("Created %n link(s)", nullptr, int(made.size())), int(made.size()))
                                    : errors.first(),
                   !errors.isEmpty());
    for (Panel* panel : {m_left, m_right})
        if (panel->isRemote())
            panel->refresh();
}

// Alt+A. Reading permissions (ACLs, flags) can touch slow mounts: off the UI thread.
void MainWindow::editPermissions()
{
    Panel* p = active();
    const QStringList paths = p->selectedPaths();
    if (paths.isEmpty() || refuseInArchive(p))
        return;
    const bool remote = p->isRemote();
    QPointer<MainWindow> self(this);
    QPointer<Panel> panel(p);
    ioPool()->start([self, panel, paths, remote] {
        auto infos = std::make_shared<QList<PermInfo>>();
        auto accounts = std::make_shared<PermAccounts>(remote ? PermAccounts{} : PermAccounts::local());
        Backend* b = remote ? backendFor(paths.first()) : nullptr;
        for (const QString& path : paths) {
            PermInfo i;
            if (!remote) {
                i = readPermissions(path);
            } else if (!b || !b->readPermissions(path, i)) {  // no shell there: what stat says
                FileStat st;
                i.error = statAt(path, st);
                i.isDir = st.isDir;
                i.mode = st.mode & 07777;
                i.uid = st.uid;
                i.gid = st.gid;
                if (path.startsWith(QLatin1String("smb:"))) {  // Windows shares: no Linux permissions
                    i.posixPerms = false;
                    i.fsName = tr("a Windows (SMB) share");
                }
            }
            infos->append(i);
        }
        if (b && !infos->isEmpty() && infos->first().aclSupported)
            b->listAccounts(paths.first(), accounts->users, accounts->groups);
        postToUi([self, panel, paths, remote, infos, accounts] {
            if (!self || !panel)
                return;
            for (int k = 0; k < infos->size(); ++k)
                if (infos->at(k).error) {
                    panel->showMessage(tr("Cannot read %1: %2").arg(baseName(paths[k]), errnoString(infos->at(k).error)), true);
                    return;
                }
            PermissionsDialog dlg(paths, *infos, remote, *accounts, self);
            if (dlg.exec() != QDialog::Accepted)
                return;
            const PermChange change = dlg.change();
            if (change.isEmpty())
                return;
            if (!remote) {
                auto* job = new PermissionsJob(paths, change);
                job->setStartImmediately(true);
                self->m_jobs->add(job);
                return;
            }
            // Servers: their own tools when they let us, else the mode bits one by one.
            ioPool()->start([panel, paths, infos, change] {
                QStringList errors;
                for (int k = 0; k < paths.size(); ++k) {
                    Backend* b = backendFor(paths[k]);
                    QString what;
                    int e = b ? b->applyPermissions(paths[k], change, &what) : ENOTSUP;
                    if (e == ENOTSUP && b) {
                        what.clear();
                        e = b->chmod(paths[k], ((infos->at(k).mode & ~change.clearBits) | change.setBits) & 07777);
                    }
                    if (e)
                        errors << tr("%1: %2").arg(baseName(paths[k]), what.isEmpty() ? errnoString(e) : what);
                }
                postToUi([panel, errors] {
                    if (!panel)
                        return;
                    if (!errors.isEmpty())
                        panel->showMessage(errors.first(), true);
                    panel->refresh();
                });
            });
        });
    });
}

void MainWindow::multiRename()
{
    Panel* p = active();
    QList<RenameItem> items;
    QSet<QString> existing;
    if (!collectRenameItems(p, items, existing))
        return;
    MultiRenameDialog dlg(items, p->path(), existing, this);
    const int result = dlg.exec();
    if (result == MultiRenameDialog::EditInEditor)
        startEditorRename(p, items, existing);
    else if (result == QDialog::Accepted)
        runRenamePlan(p, dlg.plan());
}

void MainWindow::renameInEditor()
{
    Panel* p = active();
    QList<RenameItem> items;
    QSet<QString> existing;
    if (collectRenameItems(p, items, existing))
        startEditorRename(p, items, existing);
}

void MainWindow::startEditorRename(Panel* p, const QList<RenameItem>& items, const QSet<QString>& existing)
{
    auto* session = new EditNamesSession(items, existing, this);
    QPointer<Panel> panel(p);
    const QString dir = p->path();
    connect(session, &EditNamesSession::message, this, [panel](const QString& text, bool error) {
        if (panel)
            panel->showMessage(text, error);
    });
    connect(session, &EditNamesSession::confirmed, this, [this, panel, dir](const RenamePlan& plan) {
        if (panel && panel->path() == dir)
            runRenamePlan(panel, plan);
        else if (panel)
            panel->showMessage(tr("The folder changed while editing; nothing renamed"), true);
    });
    connect(session, &EditNamesSession::done, session, &QObject::deleteLater);
    session->start();
}

void MainWindow::runRenamePlan(Panel* p, const RenamePlan& plan)
{
    const QString dir = p->path();
    QPointer<Panel> panel(p);
    QPointer<MainWindow> self(this);
    ioPool()->start([self, panel, plan, dir] {
        QStringList errors;
        const auto done = executeRenamePlan(dir, plan, &errors);
        postToUi([self, panel, done, errors, dir] {
            if (!self)
                return;
            UndoEntry undo;
            undo.kind = UndoEntry::Kind::Rename;
            undo.dir = dir;
            undo.pairs = done;
            undo.description = plural(tr("rename of %n item(s)", nullptr, int(done.size())), int(done.size()));
            self->pushUndo(undo);
            if (!panel)
                return;
            if (!errors.isEmpty())
                panel->showMessage(plural(tr("%n rename(s) failed: %1", nullptr, int(errors.size())), int(errors.size())).arg(errors.first()), true);
            else
                panel->showMessage(plural(tr("Renamed %n item(s). Ctrl+Z undoes it.", nullptr, int(done.size())), int(done.size())));
            if (!done.isEmpty())
                panel->expectName(done.first().first);
            panel->unmarkAll();
        });
    });
}

// ------------------------------------------------------------ compare & sync

void MainWindow::compareFolders()
{
    Panel* panels[2] = {m_left, m_right};
    for (Panel* p : panels) {
        if (p->inArchive() || (p->tab() && p->tab()->isSearch())) {
            active()->showMessage(tr("Comparing needs two plain folders"), true);
            return;
        }
    }
    // Uses what the panels already show: instant, no disk access.
    QHash<QString, const FileEntry*> byName[2];
    for (int s = 0; s < 2; ++s) {
        FileModel* m = panels[s]->tab()->model();
        for (int r = 0; r < m->count(); ++r)
            if (const FileEntry* e = m->entry(r); e && !e->isParent)
                byName[s].insert(e->name, e);
    }
    QSet<QString> marks[2];
    int same = 0;
    for (int s = 0; s < 2; ++s) {
        for (auto it = byName[s].cbegin(); it != byName[s].cend(); ++it) {
            const FileEntry* mine = it.value();
            const FileEntry* theirs = byName[1 - s].value(it.key());
            if (!theirs) {
                marks[s].insert(it.key());  // only here
            } else if (!mine->isDir && !theirs->isDir) {
                const int64_t dt = mine->mtime - theirs->mtime;
                if (dt > 2)
                    marks[s].insert(it.key());  // newer here
                else if (s == 0 && dt >= -2 && mine->size == theirs->size)
                    ++same;
                else if (s == 0 && dt >= -2)
                    marks[0].insert(it.key()), marks[1].insert(it.key());  // same time, other size
            }
        }
    }
    for (int s = 0; s < 2; ++s)
        panels[s]->tab()->model()->setMarkedNames(marks[s]);
    if (marks[0].isEmpty() && marks[1].isEmpty())
        active()->showMessage(tr("No differences (%1 identical files; folder contents not compared)").arg(same));
    else
        active()->showMessage(tr("Marked what is newer or missing on the other side: %1 left, %2 right · %3 identical")
                                  .arg(marks[0].size())
                                  .arg(marks[1].size())
                                  .arg(same));
}

void MainWindow::synchronizeFolders()
{
    if (m_left->inArchive() || m_right->inArchive() || (m_left->tab() && m_left->tab()->isSearch()) ||
        (m_right->tab() && m_right->tab()->isSearch())) {
        active()->showMessage(tr("Synchronizing needs two plain folders"), true);
        return;
    }
    if (m_left->path() == m_right->path()) {
        active()->showMessage(tr("Both panels show the same folder"), true);
        return;
    }
    auto* dlg = new SyncDialog(m_left->path(), m_right->path(), [this](const SyncPlan& plan) {
        QList<Job*> jobs;
        if (!plan.copies.isEmpty()) {
            auto* copy = new CopyJob(plan.copies);
            copy->setOverwriteExisting();
            m_jobs->add(copy);
            jobs << copy;
        }
        if (!plan.deletions.isEmpty()) {
            auto* del = new DeleteJob(plan.deletions, true);
            m_jobs->add(del);
            jobs << del;
        }
        return jobs;
    }, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
}

// ------------------------------------------------------------ run command

void MainWindow::openCommandBar()
{
    Panel* p = active();
    if (refuseInArchive(p) || refuseOnServer(p, tr("Running commands")))
        return;
    const QStringList sel = p->selectedPaths();
    QString what = sel.isEmpty() ? tr("nothing selected")
                                 : (sel.size() == 1 ? baseName(sel.first())
                                                    : plural(tr("%n item(s)", nullptr, int(sel.size())), sel.size()));
    m_commandBar->open(tr("Run in %1  ·  %s = %2").arg(p->path(), what));
}

void MainWindow::runCommand(const QString& command, bool inTerminal)
{
    Panel* p = active();
    CommandContext ctx;
    ctx.selection = p->selectedPaths();
    ctx.current = p->currentPath();
    ctx.dir = p->path();
    ctx.otherDir = other()->path();
    const QString expanded = expandCommand(command, ctx);

    if (inTerminal) {
        QString terminal = QSettings().value(QStringLiteral("tools/terminal")).toString();
        if (terminal.isEmpty())
            terminal = DesktopApps::instance().defaultTerminal();
        if (terminal.isEmpty()) {
            p->showMessage(tr("No terminal found. Set tools/terminal in the config."), true);
            return;
        }
        // Keep the window open afterwards so the output can be read.
        const QString wrapped = expanded + QStringLiteral("; printf '\n[exit %s] Enter closes ' \"$?\"; read _");
        QStringList argv = DesktopApps::terminalCommand(terminal, {QStringLiteral("sh"), QStringLiteral("-c"), wrapped});
        const QString program = argv.takeFirst();
        if (!QProcess::startDetached(program, argv, ctx.dir))
            p->showMessage(tr("Could not start %1").arg(program), true);
        return;
    }

    auto* proc = new QProcess(this);
    proc->setWorkingDirectory(ctx.dir);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    QPointer<Panel> panel(p);
    p->showMessage(tr("Running: %1").arg(command));
    connect(proc, &QProcess::finished, this, [this, proc, panel, command](int code, QProcess::ExitStatus status) {
        const QString out = QString::fromLocal8Bit(proc->readAll());
        proc->deleteLater();
        const bool ok = status == QProcess::NormalExit && code == 0;
        if (out.trimmed().isEmpty()) {
            if (panel)
                panel->showMessage(ok ? tr("Done: %1").arg(command) : tr("“%1” failed (exit %2)").arg(command).arg(code), !ok);
            return;
        }
        // Output: show it in a small window (non-modal; panels stay usable).
        auto* dlg = new QDialog(this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setWindowTitle(command);
        dlg->resize(760, 460);
        auto* l = new QVBoxLayout(dlg);
        auto* text = new QPlainTextEdit(out, dlg);
        text->setReadOnly(true);
        text->setFont(monoFont());
        text->setLineWrapMode(QPlainTextEdit::NoWrap);
        auto* status2 = new QLabel(ok ? tr("Finished") : tr("Exit code %1").arg(code), dlg);
        setThemeStyle(status2, ok ? QStringLiteral("color: @dimText;") : QStringLiteral("color: @error;"));
        l->addWidget(text, 1);
        l->addWidget(status2);
        dlg->show();
    });
    proc->start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), expanded});
}

// ------------------------------------------------------------ notifications

// Long operations that finish while you're in another window get a desktop
// notification (freedesktop Notifications over D-Bus, non-blocking).
void MainWindow::notifyFinished(Job* job)
{
    const qint64 took = QDateTime::currentMSecsSinceEpoch() - job->property("startedAt").toLongLong();
    if (isActiveWindow() || took < 5000 || !QSettings().value(QStringLiteral("view/notifications"), true).toBool())
        return;
    const Job::Snapshot s = job->snapshot();
    QString summary, body;
    if (s.state == Job::State::Cancelled) {
        summary = tr("%1 cancelled").arg(job->title());
    } else if (s.errorCount) {
        summary = plural(tr("%1 finished with %n problem(s)", nullptr, s.errorCount), s.errorCount).arg(job->title());
        body = job->errors().constFirst();
    } else {
        summary = tr("%1 finished").arg(job->title());
        body = plural(tr("%n item(s)", nullptr, int(s.filesDone)), int(s.filesDone));
        if (s.bytesDone > 0)
            body += QStringLiteral(" · ") + formatSize(s.bytesDone);
        body += QStringLiteral(" · ") + formatDuration(took / 1000);
    }
#ifndef Q_OS_MACOS
    QDBusInterface notify(QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("/org/freedesktop/Notifications"),
                          QStringLiteral("org.freedesktop.Notifications"), QDBusConnection::sessionBus());
    if (!notify.isValid())
        return;
    notify.asyncCall(QStringLiteral("Notify"), QStringLiteral("Tandem"), uint(0), QStringLiteral("system-file-manager"),
                     summary, body, QStringList(), QVariantMap(), int(-1));
#endif
}

// --------------------------------------------------------------------- undo

void MainWindow::pushUndo(UndoEntry entry)
{
    if (entry.pairs.isEmpty())
        return;
    m_undo.append(std::move(entry));
    while (m_undo.size() > 50)
        m_undo.removeFirst();
    updateUndoAction();
}

void MainWindow::updateUndoAction()
{
    QAction* a = m_actions.value(QStringLiteral("undo"));
    if (!a)
        return;
    a->setEnabled(!m_undo.isEmpty());
    a->setText(m_undo.isEmpty() ? tr("Undo") : tr("Undo: %1").arg(m_undo.last().description));
}

void MainWindow::recordJobUndo(Job* job)
{
    const auto records = job->undoRecords();
    if (records.isEmpty())
        return;
    UndoEntry e;
    e.pairs = records;
    const QString what = plural(tr("%n item(s)", nullptr, int(records.size())), int(records.size()));
    if (auto* c = dynamic_cast<CopyJob*>(job)) {
        e.kind = c->mode() == CopyJob::Mode::Move ? UndoEntry::Kind::Move : UndoEntry::Kind::Copy;
        e.description = (c->mode() == CopyJob::Mode::Move ? tr("move %1") : tr("copy %1")).arg(what);
    } else if (dynamic_cast<DeleteJob*>(job)) {
        e.kind = UndoEntry::Kind::Trash;
        e.description = tr("move %1 to trash").arg(what);
    } else if (dynamic_cast<PackJob*>(job)) {
        e.kind = UndoEntry::Kind::Copy;
        e.description = tr("pack %1").arg(baseName(records.first().first));
    } else {
        return;
    }
    pushUndo(e);
}

void MainWindow::undo()
{
    if (m_undo.isEmpty())
        return;
    const UndoEntry e = m_undo.takeLast();
    updateUndoAction();
    Panel* p = active();
    p->showMessage(tr("Undoing %1 …").arg(e.description));
    QPointer<Panel> panel(p);
    QPointer<MainWindow> self(this);
    auto report = [self, panel, e](const QStringList& errors) {
        if (!self || !panel)
            return;
        if (errors.isEmpty())
            panel->showMessage(tr("Undid %1").arg(e.description));
        else
            panel->showMessage(tr("Undo of %1 incomplete: %2").arg(e.description, errors.first()), true);
    };

    switch (e.kind) {
    case UndoEntry::Kind::Rename: {
        RenamePlan plan;
        for (const auto& [now, before] : e.pairs)
            plan.rows.append({now, before, RenameRow::Status::Ok, {}});
        ioPool()->start([plan, dir = e.dir, report] {
            QStringList errors;
            executeRenamePlan(dir, plan, &errors);
            postToUi([report, errors] { report(errors); });
        });
        break;
    }
    case UndoEntry::Kind::Move: {
        // Back where they came from; one job per original folder.
        QMap<QString, QStringList> byFolder;
        for (const auto& [now, before] : e.pairs) {
            if (baseName(now) == baseName(before))
                byFolder[parentPath(before)].append(now);
            else
                m_jobs->add(new CopyJob(CopyJob::Mode::Move, {now}, before));
        }
        for (auto it = byFolder.cbegin(); it != byFolder.cend(); ++it) {
            auto* job = new CopyJob(CopyJob::Mode::Move, it.value(), it.key());
            job->setProperty("isUndo", true);
            m_jobs->add(job);
        }
        p->showMessage(tr("Undoing %1 (see the operations list)").arg(e.description));
        break;
    }
    case UndoEntry::Kind::Copy: {
        QStringList created;
        for (const auto& pair : e.pairs)
            created.append(pair.first);
        auto* job = new DeleteJob(created, true);
        job->setProperty("isUndo", true);
        m_jobs->add(job);
        break;
    }
    case UndoEntry::Kind::Trash:
        ioPool()->start([pairs = e.pairs, report] {
            const QStringList errors = restoreFromTrash(pairs);
            postToUi([report, errors] { report(errors); });
        });
        break;
    case UndoEntry::Kind::Create: {
        QStringList created;
        for (const auto& pair : e.pairs)
            created.append(pair.first);
        ioPool()->start([created, report] {
            const QStringList errors = removeIfStillEmpty(created);
            postToUi([report, errors] { report(errors); });
        });
        break;
    }
    }
}

// ---------------------------------------------------------- context menus

void MainWindow::showContextMenu(Panel* panel, const QPoint& pos, bool onItem)
{
    setActive(panel);
    QMenu menu(this);
    auto act = [this](const char* id) { return m_actions.value(QLatin1String(id)); };
    const bool canPaste = QApplication::clipboard()->mimeData() && QApplication::clipboard()->mimeData()->hasUrls();

    if (!onItem && panel->inArchive()) {
        menu.addAction(act("refresh"));
        QMenu* sort = menu.addMenu(tr("Sort by"));
        for (const char* id : {"sortName", "sortExt", "sortDate", "sortSize"})
            sort->addAction(act(id));
        menu.addAction(m_hiddenAction);
        menu.exec(pos);
        return;
    }
    if (!onItem) {
        menu.addAction(act("mkdir"));
        menu.addAction(act("mkfile"));
        menu.addSeparator();
        act("paste")->setEnabled(canPaste);
        menu.addAction(act("paste"));
        menu.addSeparator();
        menu.addAction(act("terminal"));
        menu.addSeparator();
        QMenu* sort = menu.addMenu(tr("Sort by"));
        for (const char* id : {"sortName", "sortExt", "sortDate", "sortSize"})
            sort->addAction(act(id));
        menu.addAction(m_hiddenAction);
        menu.addAction(act("refresh"));
        menu.addSeparator();
        menu.addAction(act("markAll"));
        menu.addAction(act("unmarkAll"));
        addPluginItems(&menu, panel, false);
        menu.exec(pos);
        act("paste")->setEnabled(true);
        return;
    }

    const QStringList paths = panel->selectedPaths();
    const FileEntry* e = panel->currentEntry();
    const bool single = paths.size() == 1;
    const bool dir = single && e && e->isDir;

    if (panel->inArchive()) {
        QAction* open = menu.addAction(IconCache::themed(dir ? "folder-open" : "document-open"), tr("Open"), this,
                                       [panel] { panel->openCurrent(); });
        QFont bold = open->font();
        bold.setBold(true);
        open->setFont(bold);
        if (single && !dir)
            menu.addAction(IconCache::themed("document-preview"), tr("View"), this, [this] { viewCurrent(); });
        menu.addSeparator();
        act("copy")->setText(tr("Extract to other panel…"));
        menu.addAction(act("copy"));
        menu.addAction(act("copyPath"));
        menu.exec(pos);
        act("copy")->setText(tr("Copy…"));
        return;
    }
    const bool archiveFile = single && e && !e->isDir && isArchiveName(e->name);
    const bool allArchives = std::all_of(paths.cbegin(), paths.cend(), [](const QString& p) { return isArchiveName(baseName(p)); });

    if (paths.size() > 1) {
        QAction* title = menu.addAction(tr("%n items", nullptr, int(paths.size())));
        title->setEnabled(false);
        menu.addSeparator();
    }
    QAction* open = menu.addAction(IconCache::themed(dir ? "folder-open" : "document-open"), tr("Open"),
                                   this, [panel] { panel->openCurrent(); });
    QFont bold = open->font();
    bold.setBold(true);
    open->setFont(bold);
    if (dir) {
        const QString p = paths.first();
        menu.addAction(IconCache::themed("tab-new"), tr("Open in new tab"), this, [panel, p] { panel->addTab(p); });
        menu.addAction(tr("Open in other panel"), this, [this, p] { other()->navigate(p); });
    } else {
        addOpenWithMenu(&menu, paths);
        if (single)
            menu.addAction(act("edit"));
    }
    menu.addSeparator();
    menu.addAction(act("clipCut"));
    menu.addAction(act("clipCopy"));
    if (dir && canPaste) {
        const QString p = paths.first();
        menu.addAction(IconCache::themed("edit-paste"), tr("Paste into folder"), this, [this, p] { pasteInto(p); });
    }
    menu.addSeparator();
    if (archiveFile) {
        const QString p = paths.first();
        menu.addAction(IconCache::themed("archive-extract", "package-x-generic"), tr("Browse archive"), this,
                       [panel, p] { panel->navigate(p); });
    }
    if (allArchives && !paths.isEmpty()) {
        menu.addAction(act("unpackHere"));
        menu.addAction(act("unpack"));
    }
    menu.addAction(act("pack"));
    menu.addSeparator();
    act("copy")->setText(tr("Copy to other panel…"));
    act("move")->setText(tr("Move to other panel…"));
    menu.addAction(act("copy"));
    menu.addAction(act("move"));
    if (single)
        menu.addAction(act("rename"));
    menu.addAction(act("multiRename"));
    menu.addAction(act("editorRename"));
    menu.addAction(act("permissions"));
    menu.addAction(act("link"));
    menu.addAction(act("checksums"));
    menu.addAction(act("copyPath"));
    addPluginItems(&menu, panel, true);
    menu.addSeparator();
    if (e && e->isDir)
        menu.addAction(IconCache::themed("view-statistics"), tr("Calculate size"), this,
                       [panel] { panel->computeCurrentSize(); });
    const QString termDir = dir ? paths.first() : panel->path();
    menu.addAction(IconCache::themed("utilities-terminal"), tr("Open terminal here"), this,
                   [this, termDir] { openTerminal(termDir); });
    menu.addSeparator();
    menu.addAction(act("trash"));
    menu.addAction(act("delete"));

    menu.exec(pos);
    act("copy")->setText(tr("Copy…"));
    act("move")->setText(tr("Move…"));
}

// ------------------------------------------------------------- plugins

void MainWindow::loadPlugins(std::function<void()> then)
{
    QPointer<MainWindow> self(this);
    ioPool()->start([self, then] {
        auto found = std::make_shared<QList<Plugin>>(discoverPlugins());
        postToUi([self, found, then] {
            if (!self)
                return;
            self->m_plugins = *found;
            self->installPluginCommands();
            if (then)
                then();
        });
    });
}

void MainWindow::installPluginCommands()
{
    qDeleteAll(m_pluginActions);
    m_pluginActions.clear();
    m_pluginMenu->clear();
    for (const Plugin& plugin : std::as_const(m_plugins)) {
        if (!plugin.error.isEmpty() || pluginTrust(plugin) != PluginTrust::On)
            continue;
        if (!m_pluginMenu->isEmpty())
            m_pluginMenu->addSeparator();
        for (const PluginAction& action : plugin.actions) {
            QList<QKeySequence> keys;
            for (const QString& k : action.keys)
                keys << QKeySequence::fromString(k, QKeySequence::PortableText);
            QAction* a = addCommand(
                m_pluginMenu, action.title, keys,
                [this, plugin, action] {
                    Panel* p = active();
                    if (action.choices.isEmpty()) {
                        runPluginAction(plugin, action, p);
                        return;
                    }
                    // Entries to pick from, at the cursor.
                    auto* menu = new QMenu(this);
                    menu->setAttribute(Qt::WA_DeleteOnClose);
                    fillPluginChoices(menu, plugin, action, p);
                    const QRect r = p->view()->visualRect(p->view()->currentIndex());
                    menu->popup(p->view()->viewport()->mapToGlobal(r.isValid() ? r.bottomLeft() + QPoint(40, 0)
                                                                               : QPoint(40, 40)));
                },
                QStringLiteral("plugin-%1-%2").arg(plugin.id, action.id));
            a->setProperty("category", plugin.name);
            m_pluginActions.append(a);
        }
    }
    m_pluginMenu->menuAction()->setVisible(!m_pluginActions.isEmpty());
    applyKeyBindings(m_pluginActions);
}

PluginContext MainWindow::pluginContext(Panel* p) const
{
    PluginContext ctx;
    ctx.selection = p->selectedPaths();
    ctx.current = p->currentPath();
    ctx.dir = p->path();
    ctx.otherDir = (p == m_left ? m_right : m_left)->path();
    ctx.local = !p->isRemote() && !p->inArchive();
    return ctx;
}

void MainWindow::runPluginAction(const Plugin& plugin, const PluginAction& action, Panel* p, const QString& choice)
{
    const PluginContext ctx = pluginContext(p);
    if (!pluginApplies(action, ctx)) {
        p->showMessage(action.localOnly && !ctx.local ? tr("%1 works on local files only").arg(action.title)
                                                      : tr("%1 doesn't apply to these files").arg(action.title),
                       true);
        return;
    }
    auto* run = new PluginRun(plugin, action, ctx, choice, this);
    QPointer<Panel> panel(p);
    const QString title = action.title;
    connect(run, &PluginRun::request, this, [panel](const PluginRequest& r) {
        if (!panel)
            return;
        if (!r.error.isEmpty())
            panel->showMessage(r.error, true);
        else if (!r.message.isEmpty())
            panel->showMessage(r.message);
        if (!r.cd.isEmpty())
            panel->navigate(r.cd);
        else if (r.refresh)
            panel->refresh();
        if (!r.select.isEmpty())
            panel->markNames(r.select);
    });
    connect(run, &PluginRun::finished, this, [panel, title](bool ok, const QString& text) {
        if (!panel)
            return;
        if (!text.isEmpty())
            panel->showMessage(text, !ok);
        else if (!ok)
            panel->showMessage(tr("%1 failed").arg(title), true);
    });
    p->showMessage(tr("%1 …").arg(choice.isEmpty() ? title : QStringLiteral("%1: %2").arg(title, choice)));
    run->start();
}

void MainWindow::fillPluginChoices(QMenu* menu, const Plugin& plugin, const PluginAction& action, Panel* p)
{
    QAction* loading = menu->addAction(tr("Loading…"));
    loading->setEnabled(false);
    QPointer<QMenu> target(menu);
    QPointer<Panel> panel(p);
    listPluginChoices(plugin, action, pluginContext(p), menu,
                      [this, target, panel, plugin, action](const QList<QPair<QString, QString>>& entries,
                                                            const QString& error) {
                          if (!target || !panel)
                              return;
                          target->clear();
                          if (!error.isEmpty() || entries.isEmpty()) {
                              target->addAction(error.isEmpty() ? tr("Nothing to choose") : error)->setEnabled(false);
                              return;
                          }
                          for (const auto& [label, value] : entries) {
                              const QString v = value;
                              target->addAction(label, this, [this, panel, plugin, action, v] {
                                  if (panel)
                                      runPluginAction(plugin, action, panel, v);
                              });
                          }
                      });
}

// The plugin actions that fit what was right-clicked: the selection, or
// the folder when clicking beside the files.
void MainWindow::addPluginItems(QMenu* menu, Panel* p, bool onItem)
{
    const PluginContext ctx = pluginContext(p);
    bool first = true;
    for (const Plugin& plugin : std::as_const(m_plugins)) {
        if (!plugin.error.isEmpty() || pluginTrust(plugin) != PluginTrust::On)
            continue;
        for (const PluginAction& action : plugin.actions) {
            if (action.forFolder == onItem || !pluginApplies(action, ctx))
                continue;
            if (first)
                menu->addSeparator();
            first = false;
            if (action.choices.isEmpty()) {
                menu->addAction(action.title, this, [this, plugin, action, p] { runPluginAction(plugin, action, p); });
                continue;
            }
            QMenu* sub = menu->addMenu(action.title);
            sub->addAction(tr("Loading…"))->setEnabled(false);  // empty menus don't open
            connect(sub, &QMenu::aboutToShow, this, [this, sub, plugin, action, p] {
                if (sub->property("filled").toBool())
                    return;
                sub->setProperty("filled", true);
                fillPluginChoices(sub, plugin, action, p);
            });
        }
    }
}

void MainWindow::fillBookmarksMenu(QMenu* menu)
{
    for (QAction* a : menu->actions())  // what the last opening added
        if (a->property("bookmarkEntry").toBool())
            delete a;
    auto add = [&](QAction* a) {
        a->setProperty("bookmarkEntry", true);
        return a;
    };
    const FolderHistory& h = FolderHistory::instance();
    if (!h.isLoaded()) {
        add(menu->addAction(tr("Loading…")))->setEnabled(false);
        return;
    }
    QList<QPair<QString, QString>> entries;  // path, key ("" for a plain pin)
    for (const FolderHistory::Bookmark& b : h.bookmarks())
        entries.append({b.path, b.key});
    QStringList pins;
    for (const FolderHistory::Item& it : h.items())
        if (it.pinned && it.key.isEmpty())
            pins << it.path;
    if (entries.isEmpty() && pins.isEmpty()) {
        add(menu->addAction(tr("No bookmarks yet: Ctrl+Shift+D bookmarks the shown folder")))->setEnabled(false);
        return;
    }
    // Names, with the folder above when two would look the same.
    QHash<QString, int> seen;
    for (const auto& e : std::as_const(entries))
        ++seen[baseName(e.first)];
    for (const QString& p : std::as_const(pins))
        ++seen[baseName(p)];
    auto label = [&](const QString& path) {
        if (path == QDir::homePath())
            return tr("Home");
        if (path == QLatin1String("/"))
            return QStringLiteral("/");
        const QString name = baseName(path);
        return seen.value(name) > 1 ? QStringLiteral("%1  (%2)").arg(name, baseName(parentPath(path))) : name;
    };
    static const QIcon folder = IconCache::themed("folder");
    static const QIcon pin = IconCache::themed("pin", "emblem-favorite");
    auto entry = [&](const QString& path, const QString& key, const QIcon& icon) {
        QAction* a = add(menu->addAction(icon, key.isEmpty() ? label(path) : label(path) + QLatin1Char('\t') + key));
        a->setToolTip(tildePath(path));
        a->setStatusTip(tildePath(path));
        connect(a, &QAction::triggered, this, [this, path] { active()->navigate(path); });
    };
    for (const auto& [path, key] : std::as_const(entries))
        entry(path, key, folder);
    if (!entries.isEmpty() && !pins.isEmpty())
        add(menu->addSeparator());
    for (const QString& p : std::as_const(pins))
        entry(p, QString(), pin);
    menu->setToolTipsVisible(true);
}

void MainWindow::addOpenWithMenu(QMenu* menu, const QStringList& paths)
{
    QMenu* sub = menu->addMenu(tr("Open with"));
    DesktopApps& registry = DesktopApps::instance();
    if (!registry.isLoaded()) {
        sub->addAction(tr("Loading applications…"))->setEnabled(false);
        return;
    }
    // Apps that can open every selected file, best match first.
    QList<DesktopApp> apps;
    bool first = true;
    for (const QString& p : paths) {
        const QList<DesktopApp> forThis = registry.appsFor(mimeChainFor(p));
        if (first) {
            apps = forThis;
            first = false;
        } else {
            apps.removeIf([&](const DesktopApp& a) {
                return std::none_of(forThis.begin(), forThis.end(), [&](const DesktopApp& b) { return b.id == a.id; });
            });
        }
    }
    const QString dir = active()->path();
    const QString terminal = registry.defaultTerminal();

    // One file type across the selection? Then defaults can be shown and set.
    static QMimeDatabase db;
    QMimeType type;
    for (const QString& p : paths) {
        const QList<QMimeType> t = db.mimeTypesForFileName(baseName(p));
        const QMimeType mt = t.isEmpty() ? db.mimeTypeForName(QStringLiteral("application/octet-stream")) : t.first();
        if (!type.isValid())
            type = mt;
        else if (type != mt) {
            type = QMimeType();
            break;
        }
    }
    const QString defaultId = type.isValid() ? registry.defaultFor(mimeChainFor(paths.first())).id : QString();

    int n = 0;
    for (const DesktopApp& app : std::as_const(apps)) {
        if (++n > 20)
            break;
        const QString label = app.id == defaultId ? tr("%1 (default)").arg(app.name) : app.name;
        sub->addAction(QIcon::fromTheme(app.icon), label, this, [this, app, paths, dir, terminal] {
            if (!DesktopApps::launch(app, paths, dir, terminal))
                active()->showMessage(tr("Could not start %1").arg(app.name), true);
        });
    }
    if (apps.isEmpty())
        sub->addAction(tr("No registered applications"))->setEnabled(false);

    if (type.isValid() && !apps.isEmpty()) {
        sub->addSeparator();
        QMenu* def = sub->addMenu(tr("Always open “%1” files with").arg(type.comment()));
        const QString mime = type.name();
        const QString what = type.comment();
        for (const DesktopApp& app : std::as_const(apps)) {
            QAction* a = def->addAction(QIcon::fromTheme(app.icon), app.name);
            a->setCheckable(true);
            a->setChecked(app.id == defaultId);
            connect(a, &QAction::triggered, this, [this, mime, what, app] {
                QPointer<MainWindow> self(this);
                ioPool()->start([self, mime, what, app] {
                    QString error;
                    const bool ok = DesktopApps::instance().setDefault(mime, app.id, &error);
                    postToUi([self, ok, error, what, app] {
                        if (!self)
                            return;
                        if (ok)
                            self->active()->showMessage(tr("“%1” files now open with %2").arg(what, app.name));
                        else
                            self->active()->showMessage(tr("Could not save default: %1").arg(error), true);
                    });
                });
            });
        }
    }
    sub->addSeparator();
    sub->addAction(tr("Other command…"), this, [this, paths, dir] {
        bool ok = false;
        static QString last;
        const QString cmd = QInputDialog::getText(this, tr("Open with"), tr("Command (files are appended):"),
                                                  QLineEdit::Normal, last, &ok)
                                .trimmed();
        if (!ok || cmd.isEmpty())
            return;
        last = cmd;
        DesktopApp custom;
        custom.name = cmd;
        custom.exec = cmd;
        if (!DesktopApps::launch(custom, paths, dir, {}))
            active()->showMessage(tr("Could not start %1").arg(cmd), true);
    });
}

void MainWindow::openTerminal(const QString& dir)
{
    QString where = dir.isEmpty() ? active()->path() : dir;
    if (isRemoteLocation(where)) {
        active()->showMessage(tr("A terminal on the server: use ssh (Tandem doesn't open one there yet)"), true);
        return;
    }
    if (active()->inArchive() && where.startsWith(active()->archivePath()))
        where = parentPath(active()->archivePath());  // the folder holding the archive
#ifdef Q_OS_MACOS
    if (QSettings().value(QStringLiteral("tools/terminal")).toString().isEmpty()) {
        if (!QProcess::startDetached(QStringLiteral("/usr/bin/open"),
                                     {QStringLiteral("-a"), QStringLiteral("Terminal"), where}))
            active()->showMessage(tr("Could not open Terminal"), true);
        return;
    }
#endif
    QString terminal = QSettings().value(QStringLiteral("tools/terminal")).toString();
    if (terminal.isEmpty())
        terminal = DesktopApps::instance().defaultTerminal();
    if (terminal.isEmpty()) {
        active()->showMessage(tr("No terminal found. Set tools/terminal in the config."), true);
        return;
    }
    QStringList args = QProcess::splitCommand(terminal);
    const QString program = args.takeFirst();
    if (!QProcess::startDetached(program, args, where))
        active()->showMessage(tr("Could not start %1").arg(program), true);
}

void MainWindow::createNew()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Create"), tr("Name (end with / for a folder):"),
                                               QLineEdit::Normal, QString(), &ok)
                             .trimmed();
    if (!ok || name.isEmpty())
        return;
    if (name.endsWith(QLatin1Char('/')))
        makeFolder(name);
    else
        makeFile(name);
}

void MainWindow::runCommandId(const QString& id)
{
    if (id == QLatin1String("create")) {
        createNew();
    } else if (id == QLatin1String("jobs")) {
        m_jobsPanel->setExpanded(!m_jobsPanel->isExpanded());
    } else if (id == QLatin1String("preview-up") || id == QLatin1String("preview-down")) {
        ViewerWindow* v = active() == m_right && m_quickView2 ? m_quickView2 : m_quickView;
        if (v)
            v->scrollPage(id == QLatin1String("preview-down") ? 1 : -1);
        else
            active()->showMessage(tr("No preview open (Ctrl+Q)"));
    } else {
        for (QAction* a : actions())
            if (a->property("commandId").toString() == id) {
                a->trigger();
                return;
            }
    }
}

void MainWindow::makeFile(const QString& given)
{
    Panel* p = active();
    QString why;
    if (p->inArchive() && !canUpdateArchive(p->archivePath(), &why)) {
        p->showMessage(why, true);
        return;
    }
    bool ok = !given.isEmpty();
    const QString name =
        ok ? given
           : QInputDialog::getText(this, tr("New file"), tr("File name:"), QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || name.isEmpty())
        return;
    if (name.contains(QLatin1Char('/'))) {
        p->showMessage(tr("A name cannot contain '/'"), true);
        return;
    }
    if (p->inArchive()) {  // an empty file goes in like any other
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) +
                            QStringLiteral("/tandem-fm/new/") + QString::number(QDateTime::currentMSecsSinceEpoch());
        QDir().mkpath(dir);
        const QString empty = joinPath(dir, name);
        if (createEmptyFile(empty) != 0) {
            p->showMessage(tr("Cannot create %1").arg(name), true);
            return;
        }
        ArchiveChanges c;
        c.add.append({empty, joinPath(p->innerPath(p->path()), name)});
        c.moveSources = true;  // the temp copy goes away afterwards
        if (changeArchive(p, c, tr("New file in %1").arg(baseName(p->archivePath()))))
            p->expectName(name);
        return;
    }
    const QString full = joinPath(p->path(), name);
    QPointer<Panel> panel(p);
    QPointer<MainWindow> self(this);
    ioPool()->start([self, panel, full, name] {
        const int err = createEmptyFile(full);
        postToUi([self, panel, err, full, name] {
            if (self && !err && !isRemoteLocation(full))
                self->pushUndo({UndoEntry::Kind::Create, tr("new file %1").arg(name), {}, {{full, {}}}});
            if (!panel)
                return;
            if (err)
                panel->showMessage(QObject::tr("Cannot create %1: %2").arg(name, errnoString(err)), true);
            else
                panel->expectName(name);
        });
    });
}

// --------------------------------------------------------------- settings

void MainWindow::loadSettings(const QStringList& startPaths)
{
    QSettings s;
    restoreGeometry(s.value(QStringLiteral("window/geometry")).toByteArray());
    if (!s.contains(QStringLiteral("window/geometry")))
        resize(1280, 800);
    const QVariantList panelSizes = s.value(QStringLiteral("window/panelSizes")).toList();
    if (panelSizes.size() == 2 && panelSizes[0].toInt() > 0 && panelSizes[1].toInt() > 0)
        m_panelRatio = double(panelSizes[0].toInt()) / (panelSizes[0].toInt() + panelSizes[1].toInt());
    m_splitter->setSizes({int(10000 * m_panelRatio), int(10000 * (1 - m_panelRatio))});
    connect(m_splitter, &QSplitter::splitterMoved, this, [this] {
        const QList<int> s = m_splitter->sizes();
        if (s.value(0) > 0 && s.value(1) > 0)
            m_panelRatio = double(s[0]) / (s[0] + s[1]);
    });
    menuBar()->setVisible(s.value(QStringLiteral("window/menuBar"), true).toBool());
    const QString iconTheme = s.value(QStringLiteral("view/iconTheme")).toString();
    if (!iconTheme.isEmpty())
        IconCache::setTheme(iconTheme);
    const bool gitOn = s.value(QStringLiteral("view/gitStatus"), true).toBool();
    m_actions.value(QStringLiteral("git"))->setChecked(gitOn);
    DirTab::setGitEnabled(gitOn);
    const bool vimKeys = s.value(QStringLiteral("input/vimKeys"), false).toBool();
    m_actions.value(QStringLiteral("vim"))->setChecked(vimKeys);
    m_left->setVimKeys(vimKeys);
    m_right->setVimKeys(vimKeys);
    FileView::setInactiveDim(s.value(QStringLiteral("view/inactiveDim"), 0).toInt() / 100.0);
    FileModel::setRelativeDates(s.value(QStringLiteral("view/relativeDates"), true).toBool());
    ThumbGrid::setDefaultSize(s.value(QStringLiteral("view/gridSize"), ThumbGrid::kDefaultSize).toInt());
    FileView::setListZoom(s.value(QStringLiteral("view/listZoom"), 100).toInt());
    for (Panel* p : {m_left, m_right}) {
        p->view()->zoomChanged();
        p->syncZoom();
    }
    setFileColorsEnabled(s.value(QStringLiteral("view/fileColors"), false).toBool());
    const bool arrowFolders = s.value(QStringLiteral("input/arrowFolders"), true).toBool();
    m_actions.value(QStringLiteral("arrowFolders"))->setChecked(arrowFolders);
    m_left->setArrowFolders(arrowFolders);
    m_right->setArrowFolders(arrowFolders);
    m_actions.value(QStringLiteral("notifications"))->setChecked(s.value(QStringLiteral("view/notifications"), true).toBool());
    const bool hidden = s.value(QStringLiteral("view/showHidden"), false).toBool();
    m_hiddenAction->setChecked(hidden);

    const struct {
        Panel* panel;
        const char* key;
        int argIndex;
    } panels[] = {{m_left, "left", 0}, {m_right, "right", 1}};
    const bool rows2 = s.value(QStringLiteral("view/rows2"), false).toBool();
    for (const auto& p : panels) {
        s.beginGroup(QLatin1String(p.key));
        SortSpec spec;
        spec.column = SortColumn(s.value(QStringLiteral("sortColumn"), 0).toInt());
        spec.descending = s.value(QStringLiteral("sortDescending"), false).toBool();
        p.panel->setSortSpec(spec);
        p.panel->setShowHidden(hidden);
        if (rows2)  // layouts saved before the new columns start over once
            p.panel->restoreHeaderState(s.value(QStringLiteral("header")).toByteArray());
        QStringList tabs = s.value(QStringLiteral("tabs")).toStringList();
        int current = s.value(QStringLiteral("currentTab"), 0).toInt();
        QList<int> numbers;  // ("slots" is a Qt macro)
        for (const QVariant& v : s.value(QStringLiteral("tabSlots")).toList())
            numbers.append(v.toInt());
        if (p.argIndex < startPaths.size()) {
            tabs = {expandUserPath(QDir::current().absoluteFilePath(startPaths[p.argIndex]))};
            current = 0;
        }
        QList<bool> grids;
        for (const QVariant& v : s.value(QStringLiteral("tabGrids")).toList())
            grids.append(v.toBool());
        p.panel->restoreTabs(tabs, current, numbers, grids);
        s.endGroup();
    }
    Panel* act = s.value(QStringLiteral("window/active")).toString() == QLatin1String("right") ? m_right : m_left;
    setActive(act);
    act->view()->setFocus();
    if (s.value(QStringLiteral("view/preview"), false).toBool())
        QTimer::singleShot(0, this, [this] {
            if (!m_quickView)
                toggleQuickView();
        });
}

void MainWindow::saveSettings()
{
    QSettings s;
    s.setValue(QStringLiteral("window/geometry"), saveGeometry());
    if (m_quickView2)
        s.setValue(QStringLiteral("view/previewHeightEach"), m_leftCol->sizes().value(1));
    else if (m_quickView && m_vsplit->indexOf(m_quickView) >= 0)
        s.setValue(QStringLiteral("view/previewHeight"), m_vsplit->sizes().value(1));
    else if (m_quickView)
        s.setValue(QStringLiteral("view/previewWidth"), m_splitter->sizes().value(2));
    s.setValue(QStringLiteral("window/panelSizes"),
               QVariantList{m_splitter->sizes().value(0), m_splitter->sizes().value(1)});
    s.setValue(QStringLiteral("window/menuBar"), menuBar()->isVisible());
    s.setValue(QStringLiteral("window/active"), m_active == m_right ? "right" : "left");
    s.setValue(QStringLiteral("view/showHidden"), m_hiddenAction->isChecked());
    s.remove(QStringLiteral("view/thumbnailFolders"));  // older versions: thumbnails per folder
    s.setValue(QStringLiteral("view/rows2"), true);      // the saved layout has the new columns
    for (auto [panel, key] : {std::pair{m_left, "left"}, std::pair{m_right, "right"}}) {
        s.beginGroup(QLatin1String(key));
        s.setValue(QStringLiteral("tabs"), panel->tabPaths());
        s.setValue(QStringLiteral("currentTab"), panel->currentTabIndex());
        QVariantList numbers;
        for (int n : panel->tabSlots())
            numbers.append(n);
        s.setValue(QStringLiteral("tabSlots"), numbers);
        QVariantList grids;
        for (bool g : panel->tabGrids())
            grids.append(g);
        s.setValue(QStringLiteral("tabGrids"), grids);
        s.setValue(QStringLiteral("sortColumn"), int(panel->sortSpec().column));
        s.setValue(QStringLiteral("sortDescending"), panel->sortSpec().descending);
        s.setValue(QStringLiteral("header"), panel->headerState());
        s.endGroup();
    }
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    if (m_jobs->runningCount() > 0) {
        const auto answer = QMessageBox::question(
            this, tr("Operations running"),
            plural(tr("%n operation(s) still running. Cancel them and quit?", nullptr, m_jobs->runningCount()), m_jobs->runningCount()));
        if (answer != QMessageBox::Yes) {
            e->ignore();
            return;
        }
    }
    saveSettings();
    if (FolderHistory::instance().isLoaded())
        FolderHistory::instance().saveNow();
    m_jobs->cancelAll();
    // Give jobs a moment to stop. A job stuck in the kernel on a dead network
    // mount can't be interrupted; don't let it hold the app hostage.
    QElapsedTimer t;
    t.start();
    while (m_jobs->runningCount() > 0 && t.elapsed() < 3000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    if (m_jobs->runningCount() > 0)
        std::quick_exit(0);
    e->accept();
}

}  // namespace tandem
