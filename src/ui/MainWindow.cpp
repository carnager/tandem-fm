#include "ui/MainWindow.h"

#include "core/DesktopApps.h"
#include "core/Util.h"
#include "ui/DirTab.h"
#include "ui/Dialogs.h"
#include "ui/FileModel.h"
#include "ui/FileView.h"
#include "ui/IconCache.h"
#include "ui/JobManager.h"
#include "ui/JobsPanel.h"
#include "ui/Panel.h"
#include "ui/Theme.h"
#include "ui/viewer/ViewerWindow.h"

#include <QAbstractButton>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
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
#include <QProcess>
#include <QScrollArea>
#include <QSettings>
#include <QSplitter>
#include <QVBoxLayout>

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
    m_splitter->addWidget(m_left);
    m_splitter->addWidget(m_right);
    layout->addWidget(m_splitter, 1);

    m_jobsPanel = new JobsPanel(m_jobs, central);
    layout->addWidget(m_jobsPanel);
    layout->addWidget(createFKeyBar());
    setCentralWidget(central);

    for (Panel* p : {m_left, m_right}) {
        connect(p, &Panel::activated, this, &MainWindow::setActive);
        connect(p, &Panel::switchRequested, this, [this] { other()->view()->setFocus(); });
        connect(p, &Panel::openFileRequested, this, &MainWindow::openFile);
        connect(p, &Panel::dropRequested, this, &MainWindow::handleDrop);
        connect(p, &Panel::contextMenuRequested, this, &MainWindow::showContextMenu);
    }
    // Directories without inotify (some FUSE/network mounts) need a manual
    // refresh once an operation that touched them is done.
    connect(m_jobs, &JobManager::jobFinished, this, [this] {
        for (Panel* p : {m_left, m_right})
            if (p->tab() && !p->tab()->isWatched())
                p->refresh();
    });

    createMenus();
    loadSettings(startPaths);
    DesktopApps::instance().loadAsync();
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
    setWindowTitle(QStringLiteral("Tandem — %1").arg(p->path()));
}

// ------------------------------------------------------------------ menus

QAction* MainWindow::addCommand(QMenu* menu, const QString& text, const QList<QKeySequence>& keys,
                                std::function<void()> fn)
{
    QAction* a = menu->addAction(text);
    a->setShortcuts(keys);
    a->setShortcutContext(Qt::WindowShortcut);
    connect(a, &QAction::triggered, this, [fn = std::move(fn)] { fn(); });
    addAction(a);  // shortcuts work even with the menu bar hidden
    return a;
}

void MainWindow::createMenus()
{
    using K = QKeySequence;
    QMenuBar* bar = menuBar();

    QMenu* files = bar->addMenu(tr("&Files"));
    m_actions[QStringLiteral("open")] = addCommand(files, tr("Open"), {}, [this] { active()->openCurrent(); });
    addCommand(files, tr("View"), {K(Qt::Key_F3)}, [this] { viewCurrent(); });
    m_actions[QStringLiteral("edit")] = addCommand(files, tr("Edit"), {K(Qt::Key_F4)}, [this] { editCurrent(); });
    files->addSeparator();
    m_actions[QStringLiteral("copy")] = addCommand(files, tr("Copy…"), {K(Qt::Key_F5)}, [this] { copyOrMove(CopyJob::Mode::Copy); });
    m_actions[QStringLiteral("move")] = addCommand(files, tr("Move…"), {K(Qt::Key_F6)}, [this] { copyOrMove(CopyJob::Mode::Move); });
    m_actions[QStringLiteral("rename")] = addCommand(files, tr("Rename"), {K(Qt::SHIFT | Qt::Key_F6)}, [this] { active()->startRename(); });
    m_actions[QStringLiteral("mkfile")] =
        addCommand(files, tr("New file…"), {K(Qt::SHIFT | Qt::Key_F4)}, [this] { makeFile(); });
    m_actions[QStringLiteral("mkdir")] = addCommand(files, tr("New folder…"), {K(Qt::Key_F7)}, [this] { makeFolder(); });
    m_actions[QStringLiteral("trash")] = addCommand(files, tr("Move to trash"), {K(Qt::Key_F8), K(Qt::Key_Delete)}, [this] { deleteSelected(false); });
    m_actions[QStringLiteral("delete")] = addCommand(files, tr("Delete permanently"), {K(Qt::SHIFT | Qt::Key_F8), K(Qt::SHIFT | Qt::Key_Delete)},
               [this] { deleteSelected(true); });
    files->addSeparator();
    m_actions[QStringLiteral("clipCopy")] = addCommand(files, tr("Copy to clipboard"), {K(Qt::CTRL | Qt::Key_C)}, [this] { copyToClipboard(false); });
    m_actions[QStringLiteral("clipCut")] = addCommand(files, tr("Cut to clipboard"), {K(Qt::CTRL | Qt::Key_X)}, [this] { copyToClipboard(true); });
    m_actions[QStringLiteral("paste")] = addCommand(files, tr("Paste"), {K(Qt::CTRL | Qt::Key_V)}, [this] { paste(); });
    m_actions[QStringLiteral("copyPath")] = addCommand(files, tr("Copy full paths as text"), {K(Qt::CTRL | Qt::SHIFT | Qt::Key_C)},
               [this] { copyPathsAsText(); });
    m_actions[QStringLiteral("terminal")] =
        addCommand(files, tr("Open terminal here"), {K(Qt::CTRL | Qt::ALT | Qt::Key_T)}, [this] { openTerminal(); });
    files->addSeparator();
    addCommand(files, tr("Quit"), {K(Qt::CTRL | Qt::Key_Q)}, [this] { close(); });

    QMenu* mark = bar->addMenu(tr("&Mark"));
    addCommand(mark, tr("Mark group…\tNum +"), {}, [this] { active()->markGroup(1); });
    addCommand(mark, tr("Unmark group…\tNum -"), {}, [this] { active()->markGroup(-1); });
    addCommand(mark, tr("Invert marks\tNum *"), {}, [this] { active()->markGroup(0); });
    m_actions[QStringLiteral("markAll")] = addCommand(mark, tr("Mark all"), {K(Qt::CTRL | Qt::Key_A)}, [this] { active()->markAll(true); });
    m_actions[QStringLiteral("unmarkAll")] = addCommand(mark, tr("Unmark all"), {K(Qt::CTRL | Qt::SHIFT | Qt::Key_A)}, [this] { active()->markAll(false); });
    mark->addSeparator();
    addCommand(mark, tr("Calculate all folder sizes"), {K(Qt::ALT | Qt::SHIFT | Qt::Key_Return)},
               [this] { active()->computeAllSizes(); });

    QMenu* go = bar->addMenu(tr("&Go"));
    addCommand(go, tr("Back"), {K(Qt::ALT | Qt::Key_Left)}, [this] { active()->back(); });
    addCommand(go, tr("Forward"), {K(Qt::ALT | Qt::Key_Right)}, [this] { active()->forward(); });
    addCommand(go, tr("Parent folder"), {K(Qt::CTRL | Qt::Key_PageUp)}, [this] { active()->goUp(); });
    addCommand(go, tr("Enter folder"), {K(Qt::CTRL | Qt::Key_PageDown)}, [this] { active()->openCurrent(); });
    addCommand(go, tr("Root"), {K(Qt::CTRL | Qt::Key_Backslash)}, [this] { active()->navigate(QStringLiteral("/")); });
    addCommand(go, tr("Home"), {K(Qt::CTRL | Qt::Key_QuoteLeft)}, [this] { active()->navigate(QDir::homePath()); });
    addCommand(go, tr("Go to path…"), {K(Qt::CTRL | Qt::Key_L)}, [this] { active()->focusPathEdit(); });
    go->addSeparator();
    addCommand(go, tr("Locations (left)"), {K(Qt::ALT | Qt::Key_F1)}, [this] { m_left->showMountMenu(); });
    addCommand(go, tr("Locations (right)"), {K(Qt::ALT | Qt::Key_F2)}, [this] { m_right->showMountMenu(); });
    addCommand(go, tr("Show in left panel"), {K(Qt::CTRL | Qt::Key_Left)}, [this] { openInPanel(m_left); });
    addCommand(go, tr("Show in right panel"), {K(Qt::CTRL | Qt::Key_Right)}, [this] { openInPanel(m_right); });
    addCommand(go, tr("Swap panels"), {K(Qt::CTRL | Qt::Key_U)}, [this] { swapPanels(); });
    addCommand(go, tr("Same folder in other panel"), {K(Qt::ALT | Qt::Key_Equal)},
               [this] { other()->navigate(active()->path()); });
    go->addSeparator();
    m_actions[QStringLiteral("refresh")] = addCommand(go, tr("Refresh"), {K(Qt::CTRL | Qt::Key_R)}, [this] { active()->refresh(); });

    QMenu* tabs = bar->addMenu(tr("&Tabs"));
    addCommand(tabs, tr("New tab"), {K(Qt::CTRL | Qt::Key_T)}, [this] {
        const QString p = active()->currentEntry() && active()->currentEntry()->isDir &&
                                  !active()->currentEntry()->isParent
                              ? active()->currentPath()
                              : active()->path();
        active()->addTab(p);
    });
    addCommand(tabs, tr("Close tab"), {K(Qt::CTRL | Qt::Key_W)}, [this] { active()->closeCurrentTab(); });
    addCommand(tabs, tr("Next tab"), {K(Qt::CTRL | Qt::Key_Tab)}, [this] { active()->cycleTab(1); });
    addCommand(tabs, tr("Previous tab"), {K(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab), K(Qt::CTRL | Qt::Key_Backtab)},
               [this] { active()->cycleTab(-1); });

    QMenu* view = bar->addMenu(tr("&View"));
    m_actions[QStringLiteral("sortName")] = addCommand(view, tr("Sort by name"), {K(Qt::CTRL | Qt::Key_F3)}, [this] { active()->sortBy(SortColumn::Name); });
    m_actions[QStringLiteral("sortExt")] = addCommand(view, tr("Sort by extension"), {K(Qt::CTRL | Qt::Key_F4)}, [this] { active()->sortBy(SortColumn::Ext); });
    m_actions[QStringLiteral("sortDate")] = addCommand(view, tr("Sort by date"), {K(Qt::CTRL | Qt::Key_F5)}, [this] { active()->sortBy(SortColumn::Time); });
    m_actions[QStringLiteral("sortSize")] = addCommand(view, tr("Sort by size"), {K(Qt::CTRL | Qt::Key_F6)}, [this] { active()->sortBy(SortColumn::Size); });
    view->addSeparator();
    m_hiddenAction = addCommand(view, tr("Show hidden files"), {K(Qt::CTRL | Qt::Key_H)}, [this] {
        m_left->setShowHidden(m_hiddenAction->isChecked());
        m_right->setShowHidden(m_hiddenAction->isChecked());
    });
    m_hiddenAction->setCheckable(true);
    QMenu* icons = view->addMenu(tr("Icon theme"));
    connect(icons, &QMenu::aboutToShow, this, [this, icons] { populateIconThemeMenu(icons); });
    addCommand(view, tr("Toggle menu bar"), {K(Qt::CTRL | Qt::Key_M)},
               [this] { menuBar()->setVisible(!menuBar()->isVisible()); });

    QMenu* help = bar->addMenu(tr("&Help"));
    addCommand(help, tr("Keyboard shortcuts"), {K(Qt::Key_F1)}, [this] { showShortcuts(); });
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

// --------------------------------------------------------------- commands

void MainWindow::copyOrMove(CopyJob::Mode mode)
{
    Panel* src = active();
    Panel* dst = other();
    const QStringList paths = src->selectedPaths();
    if (paths.isEmpty())
        return;
    const QString title = mode == CopyJob::Mode::Copy ? tr("Copy") : tr("Move");
    QString initial = dst->path();
    if (!initial.endsWith(QLatin1Char('/')))
        initial += QLatin1Char('/');
    const QString text = askDestination(this, title, tr("%1 %2 to:").arg(title, describeItems(paths)), initial);
    if (text.isEmpty())
        return;

    QString dest = text;
    if (!dest.startsWith(QLatin1Char('/')) && !dest.startsWith(QLatin1Char('~')))
        dest = joinPath(src->path(), dest);
    const bool intoDir = text.endsWith(QLatin1Char('/'));
    dest = expandUserPath(dest);
    // "name/" with a single source means: into that folder, even if new.
    if (intoDir && paths.size() == 1)
        dest = joinPath(dest, baseName(paths.first()));

    m_jobs->add(new CopyJob(mode, paths, dest));
    if (!src->hasSelectionOverride())
        src->unmarkAll();
}

void MainWindow::deleteSelected(bool permanent)
{
    Panel* p = active();
    const QStringList paths = p->selectedPaths();
    if (paths.isEmpty())
        return;
    QMessageBox box(this);
    box.setIcon(permanent ? QMessageBox::Warning : QMessageBox::Question);
    box.setWindowTitle(permanent ? tr("Delete permanently") : tr("Move to trash"));
    box.setText(permanent ? tr("Permanently delete %1?").arg(describeItems(paths))
                          : tr("Move %1 to the trash?").arg(describeItems(paths)));
    if (permanent)
        box.setInformativeText(tr("This cannot be undone."));
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    box.setDefaultButton(QMessageBox::Yes);
    if (box.exec() != QMessageBox::Yes)
        return;
    m_jobs->add(new DeleteJob(paths, !permanent));
    if (!p->hasSelectionOverride())
        p->unmarkAll();
}

void MainWindow::makeFolder()
{
    Panel* p = active();
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("New folder"), tr("Name (use / for nested folders):"),
                                               QLineEdit::Normal, QString(), &ok)
                             .trimmed();
    if (!ok || name.isEmpty())
        return;
    const QString full = name.startsWith(QLatin1Char('/')) ? name : joinPath(p->path(), name);
    const QString first = name.section(QLatin1Char('/'), 0, 0, QString::SectionSkipEmpty);
    QPointer<Panel> panel(p);
    ioPool()->start([panel, full, first] {
        const int err = makeDirectories(full);
        postToUi([panel, err, full, first] {
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
    m_jobs->add(new CopyJob(mode, paths, targetDir));
}

void MainWindow::copyToClipboard(bool cut)
{
    const QStringList paths = active()->selectedPaths();
    if (paths.isEmpty())
        return;
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
    active()->showMessage(tr("Copied %n path(s)", nullptr, int(paths.size())));
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
    static const char* rows[][2] = {
        {"Tab", "Switch panel"},
        {"Enter / Backspace", "Open / parent folder"},
        {"Type letters", "Quick search (Up/Down: next match)"},
        {"Insert, Space, Shift+↑↓", "Mark (Space also sizes folders)"},
        {"Num + / Num - / Num *", "Mark group / unmark group / invert"},
        {"F3 / F4", "Viewer (Esc closes, N/P next file, 1/3/4 modes) / edit"},
        {"F5 / F6", "Copy / move to other panel"},
        {"Shift+F6", "Rename in place"},
        {"F7 / Shift+F4", "New folder / new file"},
        {"F8, Del / Shift+F8", "Trash / delete permanently"},
        {"Ctrl+T / Ctrl+W / Ctrl+Tab", "New / close / next tab"},
        {"Alt+← / Alt+→", "History back / forward"},
        {"Ctrl+PgUp / Ctrl+\\ / Ctrl+`", "Parent / root / home"},
        {"Ctrl+L", "Type a path"},
        {"Alt+F1 / Alt+F2", "Locations for left / right panel"},
        {"Ctrl+← / Ctrl+→", "Show folder in left / right panel"},
        {"Ctrl+U", "Swap panels"},
        {"Ctrl+F3…F6", "Sort by name / ext / date / size"},
        {"Ctrl+H", "Show hidden files"},
        {"Ctrl+C / X / V", "Clipboard (works with other apps)"},
        {"Alt+Shift+Enter", "Calculate all folder sizes"},
        {"Ctrl+Alt+T", "Open terminal here"},
        {"Menu key, Shift+F10", "Context menu"},
        {"Esc", "Cancel a slow folder load"},
    };
    QString html = QStringLiteral("<table cellspacing='0' cellpadding='4'>");
    for (const auto& r : rows)
        html += QStringLiteral("<tr><td style='color:%1'><b>%2</b></td><td>%3</td></tr>")
                    .arg(theme().accent.name(), QString::fromUtf8(r[0]).toHtmlEscaped(), tr(r[1]));
    html += QStringLiteral("</table>");
    QMessageBox box(this);
    box.setWindowTitle(tr("Keyboard shortcuts"));
    box.setTextFormat(Qt::RichText);
    box.setText(html);
    box.exec();
}

// ------------------------------------------------------------ icon themes

void MainWindow::populateIconThemeMenu(QMenu* menu)
{
    menu->clear();
    const QString current = QSettings().value(QStringLiteral("view/iconTheme")).toString();
    auto* group = new QActionGroup(menu);
    QAction* sys = menu->addAction(tr("System default (%1)").arg(IconCache::systemTheme()));
    sys->setCheckable(true);
    sys->setChecked(current.isEmpty());
    group->addAction(sys);
    connect(sys, &QAction::triggered, this, [this] { setIconTheme({}); });
    menu->addSeparator();
    if (!m_iconThemesLoaded) {
        menu->addAction(tr("Loading…"))->setEnabled(false);
        return;
    }
    for (const IconTheme& t : std::as_const(m_iconThemes)) {
        QAction* a = menu->addAction(t.name == t.id ? t.name : QStringLiteral("%1  (%2)").arg(t.name, t.id));
        a->setCheckable(true);
        a->setChecked(t.id == current);
        group->addAction(a);
        connect(a, &QAction::triggered, this, [this, id = t.id] { setIconTheme(id); });
    }
}

void MainWindow::setIconTheme(const QString& id)
{
    QSettings().setValue(QStringLiteral("view/iconTheme"), id);
    IconCache::setTheme(id);
    for (Panel* p : {m_left, m_right})
        p->view()->viewport()->update();
    update();
}

// ---------------------------------------------------------- context menus

void MainWindow::showContextMenu(Panel* panel, const QPoint& pos, bool onItem)
{
    setActive(panel);
    QMenu menu(this);
    auto act = [this](const char* id) { return m_actions.value(QLatin1String(id)); };
    const bool canPaste = QApplication::clipboard()->mimeData() && QApplication::clipboard()->mimeData()->hasUrls();

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
        menu.exec(pos);
        act("paste")->setEnabled(true);
        return;
    }

    const QStringList paths = panel->selectedPaths();
    const FileEntry* e = panel->currentEntry();
    const bool single = paths.size() == 1;
    const bool dir = single && e && e->isDir;

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
    act("copy")->setText(tr("Copy to other panel…"));
    act("move")->setText(tr("Move to other panel…"));
    menu.addAction(act("copy"));
    menu.addAction(act("move"));
    if (single)
        menu.addAction(act("rename"));
    menu.addAction(act("copyPath"));
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
    const QString where = dir.isEmpty() ? active()->path() : dir;
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

void MainWindow::makeFile()
{
    Panel* p = active();
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, tr("New file"), tr("File name:"), QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || name.isEmpty())
        return;
    if (name.contains(QLatin1Char('/'))) {
        p->showMessage(tr("A name cannot contain '/'"), true);
        return;
    }
    const QString full = joinPath(p->path(), name);
    QPointer<Panel> panel(p);
    ioPool()->start([panel, full, name] {
        const int fd = ::open(toNative(full).constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        const int err = fd < 0 ? errno : 0;
        if (fd >= 0)
            ::close(fd);
        postToUi([panel, err, full, name] {
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
    if (!m_splitter->restoreState(s.value(QStringLiteral("window/splitter")).toByteArray()))
        m_splitter->setSizes({10000, 10000});
    menuBar()->setVisible(s.value(QStringLiteral("window/menuBar"), true).toBool());
    const QString iconTheme = s.value(QStringLiteral("view/iconTheme")).toString();
    if (!iconTheme.isEmpty())
        IconCache::setTheme(iconTheme);
    const bool hidden = s.value(QStringLiteral("view/showHidden"), false).toBool();
    m_hiddenAction->setChecked(hidden);

    const struct {
        Panel* panel;
        const char* key;
        int argIndex;
    } panels[] = {{m_left, "left", 0}, {m_right, "right", 1}};
    for (const auto& p : panels) {
        s.beginGroup(QLatin1String(p.key));
        SortSpec spec;
        spec.column = SortColumn(s.value(QStringLiteral("sortColumn"), 0).toInt());
        spec.descending = s.value(QStringLiteral("sortDescending"), false).toBool();
        p.panel->setSortSpec(spec);
        p.panel->setShowHidden(hidden);
        p.panel->restoreHeaderState(s.value(QStringLiteral("header")).toByteArray());
        QStringList tabs = s.value(QStringLiteral("tabs")).toStringList();
        int current = s.value(QStringLiteral("currentTab"), 0).toInt();
        if (p.argIndex < startPaths.size()) {
            tabs = {expandUserPath(QDir::current().absoluteFilePath(startPaths[p.argIndex]))};
            current = 0;
        }
        p.panel->restoreTabs(tabs, current);
        s.endGroup();
    }
    Panel* act = s.value(QStringLiteral("window/active")).toString() == QLatin1String("right") ? m_right : m_left;
    setActive(act);
    act->view()->setFocus();
}

void MainWindow::saveSettings()
{
    QSettings s;
    s.setValue(QStringLiteral("window/geometry"), saveGeometry());
    s.setValue(QStringLiteral("window/splitter"), m_splitter->saveState());
    s.setValue(QStringLiteral("window/menuBar"), menuBar()->isVisible());
    s.setValue(QStringLiteral("window/active"), m_active == m_right ? "right" : "left");
    s.setValue(QStringLiteral("view/showHidden"), m_hiddenAction->isChecked());
    for (auto [panel, key] : {std::pair{m_left, "left"}, std::pair{m_right, "right"}}) {
        s.beginGroup(QLatin1String(key));
        s.setValue(QStringLiteral("tabs"), panel->tabPaths());
        s.setValue(QStringLiteral("currentTab"), panel->currentTabIndex());
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
            tr("%n operation(s) still running. Cancel them and quit?", nullptr, m_jobs->runningCount()));
        if (answer != QMessageBox::Yes) {
            e->ignore();
            return;
        }
    }
    saveSettings();
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
