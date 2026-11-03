#include "ui/Panel.h"

#include "core/Archive.h"
#include "core/Backend.h"
#include "core/FileOps.h"
#include "core/Mounts.h"
#include "core/Remote.h"
#include "core/Util.h"
#include "ui/DirTab.h"
#include "ui/FileModel.h"
#include "ui/FileView.h"
#include "ui/IconCache.h"
#include "ui/Network.h"
#include "ui/PathBar.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QScrollBar>
#include <QTabBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <chrono>
#include <thread>

namespace tandem {

Panel::Panel(QWidget* parent) : QFrame(parent)
{
    const Theme& t = theme();
    setContentsMargins(1, 1, 1, 1);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);

    m_tabBar = new QTabBar(this);
    m_tabBar->setDocumentMode(true);
    m_tabBar->setExpanding(false);
    m_tabBar->setMovable(false);  // order is the tab numbers
    m_tabBar->setFocusPolicy(Qt::NoFocus);
    m_tabBar->setElideMode(Qt::ElideMiddle);
    m_tabBar->setStyleSheet(t.css(QStringLiteral(
        "QTabBar { background: transparent; }"
        "QTabBar::tab { background: transparent; color: @dimText; padding: 5px 14px; border: none;"
        "  border-bottom: 2px solid transparent; max-width: 220px; }"
        "QTabBar::tab:selected { color: @text; border-bottom: 2px solid @accent; }"
        "QTabBar::tab:hover { color: @text; }")));
    m_tabBar->hide();
    layout->addWidget(m_tabBar);

    m_header = new QWidget(this);
    m_header->setObjectName(QStringLiteral("panelHeader"));
    m_header->setAttribute(Qt::WA_StyledBackground);
    auto* hl = new QHBoxLayout(m_header);
    hl->setContentsMargins(6, 4, 10, 4);
    hl->setSpacing(6);
    m_mountButton = new QToolButton(m_header);
    m_mountButton->setIcon(IconCache::themed("drive-harddisk", "folder"));
    m_mountButton->setAutoRaise(true);
    m_mountButton->setFocusPolicy(Qt::NoFocus);
    m_mountButton->setToolTip(tr("Locations (Alt+F1 / Alt+F2)"));
    m_pathBar = new PathBar(m_header);
    if (auto* edit = m_pathBar->findChild<QLineEdit*>())
        edit->installEventFilter(this);
    m_freeLabel = new QLabel(m_header);
    m_branchLabel = new QLabel(m_header);
    m_branchLabel->setObjectName(QStringLiteral("branch"));
    m_branchLabel->hide();
    m_searchChip = new QToolButton(m_header);
    m_searchChip->setObjectName(QStringLiteral("searchChip"));
    m_searchChip->setFocusPolicy(Qt::NoFocus);
    m_searchChip->setCursor(Qt::PointingHandCursor);
    m_searchChip->setToolTip(tr("Search results — click (or Esc) to leave"));
    m_searchChip->hide();
    connect(m_searchChip, &QToolButton::clicked, this, [this] {
        if (DirTab* t = tab(); t && t->isSearch())
            t->goUp();
        m_view->setFocus();
    });
    hl->addWidget(m_mountButton);
    hl->addWidget(m_pathBar, 1);
    hl->addWidget(m_searchChip);
    hl->addWidget(m_branchLabel);
    hl->addWidget(m_freeLabel);
    layout->addWidget(m_header);

    m_view = new FileView(this);
    layout->addWidget(m_view, 1);

    m_filterBar = new QWidget(this);
    m_filterBar->setObjectName(QStringLiteral("filterBar"));
    m_filterBar->setAttribute(Qt::WA_StyledBackground);
    m_filterBar->setStyleSheet(t.css(QStringLiteral(
        "#filterBar { background: @header; border-top: 1px solid @border; }"
        "QLineEdit { background: @base; color: @text; border: 1px solid @accent; border-radius: 6px; padding: 2px 8px; }"
        "QLabel { color: @dimText; }")));
    auto* fl = new QHBoxLayout(m_filterBar);
    fl->setContentsMargins(8, 4, 8, 4);
    auto* filterLabel = new QLabel(tr("Filter"), m_filterBar);
    m_filterEdit = new QLineEdit(m_filterBar);
    m_filterEdit->setPlaceholderText(tr("words or *.ext — Esc clears, Enter/↓ back to the list"));
    m_filterEdit->installEventFilter(this);
    m_filterInfo = new QLabel(m_filterBar);
    fl->addWidget(filterLabel);
    fl->addWidget(m_filterEdit, 1);
    fl->addWidget(m_filterInfo);
    m_filterBar->hide();
    layout->addWidget(m_filterBar);
    connect(m_filterEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        DirTab* t = tab();
        if (!t)
            return;
        const QString keep = currentName();
        t->model()->setFilter(text);
        int row = t->model()->rowOf(keep);
        // While filtering, land on the first match rather than "..".
        if (row < 0 || (!text.isEmpty() && t->model()->entry(row)->isParent))
            row = t->model()->count() > 1 && t->model()->entry(0)->isParent ? 1 : 0;
        m_view->setCurrentRow(row);
        updateStatus();
    });

    m_searchBar = new QWidget(this);
    m_searchBar->setObjectName(QStringLiteral("searchBarPanel"));
    m_searchBar->setAttribute(Qt::WA_StyledBackground);
    m_searchBar->setStyleSheet(t.css(QStringLiteral(
        "#searchBarPanel { background: @header; border-top: 1px solid @border; }"
        "QLineEdit { background: @base; color: @text; border: 1px solid @accent; border-radius: 6px; padding: 2px 8px; }"
        "QToolButton { color: @dimText; border: none; padding: 2px 8px; border-radius: 9px; }"
        "QToolButton:checked { color: @text; background: @border; }"
        "QLabel, QCheckBox { color: @dimText; }")));
    auto* sbl = new QHBoxLayout(m_searchBar);
    sbl->setContentsMargins(8, 4, 8, 4);
    sbl->setSpacing(6);
    m_searchNames = new QToolButton(m_searchBar);
    m_searchNames->setText(tr("Names"));
    m_searchContents = new QToolButton(m_searchBar);
    m_searchContents->setText(tr("Contents"));
    for (QToolButton* b : {m_searchNames, m_searchContents}) {
        b->setCheckable(true);
        b->setAutoExclusive(true);
        b->setFocusPolicy(Qt::NoFocus);
    }
    m_searchNames->setChecked(true);
    m_searchEdit = new QLineEdit(m_searchBar);
    m_searchEdit->setPlaceholderText(tr("search here and below — Enter starts, Esc closes"));
    m_searchEdit->installEventFilter(this);
    m_searchAll = new QCheckBox(tr("hidden && ignored"), m_searchBar);
    m_searchAll->setFocusPolicy(Qt::NoFocus);
    m_searchAll->setToolTip(tr("Also search hidden files and files excluded by .gitignore"));
    m_searchInfo = new QLabel(m_searchBar);
    sbl->addWidget(m_searchNames);
    sbl->addWidget(m_searchContents);
    sbl->addWidget(m_searchEdit, 1);
    sbl->addWidget(m_searchAll);
    sbl->addWidget(m_searchInfo);
    m_searchBar->hide();
    layout->addWidget(m_searchBar);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &Panel::runSearch);

    m_status = new QLabel(this);
    m_status->setContentsMargins(10, 4, 10, 5);
    m_status->setTextFormat(Qt::PlainText);
    layout->addWidget(m_status);

    m_search = new QLineEdit(m_view);
    m_search->setPlaceholderText(tr("Search"));
    m_search->hide();
    m_search->installEventFilter(this);
    m_search->setStyleSheet(t.css(QStringLiteral(
        "QLineEdit { background: @header; color: @text; border: 1px solid @accent; border-radius: 12px;"
        "  padding: 3px 10px; }")));
    connect(m_search, &QLineEdit::textEdited, this, [this] { quickSearchFind(1, true); });

    m_loadingTimer.setSingleShot(true);
    m_loadingTimer.setInterval(150);
    connect(&m_loadingTimer, &QTimer::timeout, this, &Panel::updateStatus);
    m_messageTimer.setSingleShot(true);
    connect(&m_messageTimer, &QTimer::timeout, this, [this] {
        m_message.clear();
        updateStatus();
    });

    connect(m_tabBar, &QTabBar::currentChanged, this, [this](int i) {
        if (i >= 0 && i != m_current)
            showTab(i);
    });
    connect(m_tabBar, &QTabBar::tabBarDoubleClicked, this, [this](int i) {
        if (i >= 0) {
            showTab(i);
            closeCurrentTab();
        }
    });

    connect(m_pathBar, &PathBar::pathEntered, this, [this](QString text) {
        text = takePassword(text.trimmed());
        if (isRemoteLocation(text) && hasBackend(text.section(QLatin1Char(':'), 0, 0))) {
            navigate(normalizeLocation(text));  // native: a location like any other
            m_view->setFocus();
            return;
        }
        if (isRemoteLocation(path()) && !text.startsWith(QLatin1Char('/')) && !text.startsWith(QLatin1Char('~')) &&
            !isRemoteLocation(text)) {
            navigate(normalizeLocation(joinPath(path(), text)));  // relative, on the server
            m_view->setFocus();
            return;
        }
        if (const QString uri = normalizeServerUri(text); !uri.isEmpty()) {
            connectTo(uri);
            m_view->setFocus();
            return;
        }
        if (!text.startsWith(QLatin1Char('/')) && !text.startsWith(QLatin1Char('~')) &&
            !text.startsWith(QLatin1String("file:")))
            text = joinPath(path(), text);
        navigate(expandUserPath(text));
        m_view->setFocus();
    });
    connect(m_pathBar, &PathBar::segmentClicked, this, [this](const QString& p) {
        // Going up lands on the folder you came from.
        const QString cur = path();
        const QString focus = cur.startsWith(p + QLatin1Char('/'))
                                  ? cur.mid(p == QLatin1String("/") ? 1 : p.size() + 1).section(QLatin1Char('/'), 0, 0)
                                  : QString();
        navigate(p, focus);
        m_view->setFocus();
    });
    connect(m_pathBar, &PathBar::segmentMiddleClicked, this, [this](const QString& p) { addTab(p, false); });
    connect(m_pathBar, &PathBar::focused, this, [this] { emit activated(this); });

    auto* menu = new QMenu(this);
    m_mountButton->setMenu(menu);
    m_mountButton->setPopupMode(QToolButton::InstantPopup);
    connect(menu, &QMenu::aboutToShow, this, [this, menu] {
        menu->clear();
        menu->addAction(IconCache::themed("user-home", "folder"), tr("Home"),
                        this, [this] { navigate(QDir::homePath()); });
        menu->addAction(IconCache::themed("folder-root", "folder"), QStringLiteral("/"),
                        this, [this] { navigate(QStringLiteral("/")); });
        menu->addSeparator();
        QStringList unmountable;
        for (const MountPoint& m : listMounts()) {
            if (m.path == QLatin1String("/"))
                continue;
            const char* icon = m.isNetwork ? "network-server" : (m.isRemovable ? "drive-removable-media" : "drive-harddisk");
            QAction* a = menu->addAction(IconCache::themed(icon, "drive-harddisk"),
                                         QStringLiteral("%1   (%2)").arg(m.path, m.fsType),
                                         this, [this, p = m.path] { navigate(p); });
            a->setToolTip(m.device);
            if (m.isRemovable)
                unmountable << m.path;
        }
        const QList<Place> places = Network::instance().places();
        bool header = false;
        for (const Place& pl : places) {
            if (!header) {
                menu->addSection(tr("Network && drives"));
                header = true;
            }
            const QIcon icon = IconCache::themed(pl.iconName.toUtf8().constData(),
                                                 pl.network ? "network-server" : "drive-removable-media");
            if (!pl.path.isEmpty()) {
                const auto r = remoteRootOf(pl.path);
                menu->addAction(icon, r ? QStringLiteral("%1   (%2)").arg(pl.name, r->label) : pl.name, this,
                                [this, p = pl.path] { navigate(p); });
                unmountable << pl.path;
            } else {
                menu->addAction(icon, tr("%1   (mount)").arg(pl.name), this, [this, id = pl.volumeId] {
                    showMessage(tr("Mounting…"));
                    QPointer<Panel> self(this);
                    Network::instance().mountVolume(id, this, [self](const QString& path, const QString& error) {
                        if (!self)
                            return;
                        if (!error.isEmpty())
                            self->showMessage(error, true);
                        else
                            self->navigate(path);
                    });
                });
            }
        }
        const QStringList recent = Network::instance().recentServers();
        if (!recent.isEmpty()) {
            menu->addSection(tr("Recent servers"));
            for (const QString& uri : recent)
                menu->addAction(IconCache::themed("network-server", "folder-remote"), uri, this,
                                [this, uri] { connectTo(uri); });
            menu->addAction(tr("Clear recent servers"), this, [] {
                for (const QString& uri : Network::instance().recentServers())
                    Network::instance().forgetServer(uri);
            });
        }
        menu->addSeparator();
        if (Network::available())
            menu->addAction(IconCache::themed("network-connect", "network-server"), tr("Connect to server…\tCtrl+K"),
                            this, &Panel::connectDialog);
        if (!unmountable.isEmpty()) {
            QMenu* sub = menu->addMenu(IconCache::themed("media-eject", "media-eject"), tr("Disconnect / eject"));
            for (const QString& p : unmountable) {
                const auto r = remoteRootOf(p);
                sub->addAction(r ? r->label : p, this, [this, p] { disconnectMount(p); });
            }
        }
    });

    connect(m_view, &FileView::focused, this, [this] { emit activated(this); });
    connect(m_view, &FileView::cursorMoved, this, [this] { emit cursorChanged(this); });
    connect(m_view, &FileView::paletteRequested, this, &Panel::paletteRequested);
    connect(m_view, &FileView::bookmarkJumpRequested, this, &Panel::bookmarkJumpRequested);
    connect(m_view, &FileView::bookmarkAssignRequested, this, &Panel::bookmarkAssignRequested);
    connect(m_view, &FileView::commandRequested, this, &Panel::commandRequested);
    connect(m_view, &FileView::searchNextRequested, this, [this](int dir) {
        if (m_lastSearch.isEmpty())
            return;
        m_search->setText(m_lastSearch);
        quickSearchFind(dir, false);
    });
    connect(m_view, &FileView::openRequested, this, &Panel::openCurrent);
    connect(m_view, &FileView::parentRequested, this, &Panel::goUp);
    connect(m_view, &FileView::switchPanelRequested, this, &Panel::switchRequested);
    connect(m_view, &FileView::escapePressed, this, &Panel::cancelLoading);
    connect(m_view, &FileView::quickSearchRequested, this, &Panel::startQuickSearch);
    connect(m_view, &FileView::markGroupRequested, this, &Panel::markGroup);
    connect(m_view, &FileView::historyRequested, this, [this](int step) { step < 0 ? back() : forward(); });
    connect(m_view, &FileView::dropRequested, this, &Panel::dropRequested);
    connect(m_view, &FileView::dragPathsRequested, this, [this](QStringList& out) {
        if (inArchive())
            showMessage(tr("Dragging out of archives isn't supported yet. Use F5 to extract."), true);
        else if (isRemote())
            showMessage(tr("Dragging files off a server isn't supported yet. Use F5 to copy."), true);
        else
            out = selectedPaths();
    });
    connect(m_view, &FileView::spaceMarkRequested, this, [this](int row) {
        DirTab* t = tab();
        const FileEntry* e = t ? t->model()->entry(row) : nullptr;
        if (!e || e->isParent)
            return;
        const bool willMark = !e->marked;
        if (e->isDir && willMark && e->size < 0)
            t->computeSize(e->name);
        t->model()->setMarked(row, willMark);
    });
    connect(m_view, &FileView::contextMenuRequested, this, [this](const QPoint& pos, int row) {
        DirTab* t = tab();
        emit activated(this);
        const FileEntry* e = t ? t->model()->entry(row) : nullptr;
        if (!e || e->isParent) {
            emit contextMenuRequested(this, pos, false);
            return;
        }
        m_view->setCurrentRow(row);
        // Marked item: act on all marks. Unmarked item: act on it alone and
        // leave the marks alone (they may have taken a while to make).
        if (!e->marked)
            m_override = {joinPath(t->path(), e->name)};
        emit contextMenuRequested(this, pos, true);
        m_override.clear();
    });
    connect(m_view->header(), &QHeaderView::sectionClicked, this, [this](int section) {
        sortBy(SortColumn(section));
    });

    updateActiveStyle();
}

// ------------------------------------------------------------------ tabs

DirTab* Panel::tab() const
{
    return m_current >= 0 ? m_tabs[size_t(m_current)] : nullptr;
}

int Panel::tabIndexOf(DirTab* t) const
{
    for (size_t i = 0; i < m_tabs.size(); ++i)
        if (m_tabs[i] == t)
            return int(i);
    return -1;
}

void Panel::bindTab(DirTab* t)
{
    connect(t, &DirTab::listingReady, this, [this, t](const QString& focus) { onListingReady(t, focus); });
    connect(t, &DirTab::listingFailed, this, [this, t](const QString& p, const QString& msg) {
        if (t != tab())
            return;
        showMessage(tr("Cannot open %1: %2").arg(p, msg), true);
        updateHeader();
    });
    connect(t, &DirTab::loadingChanged, this, [this, t](bool loading) {
        if (t != tab())
            return;
        if (loading)
            m_loadingTimer.start();
        else
            m_loadingTimer.stop();
        updateStatus();
    });
    connect(t, &DirTab::focusRequested, this, [this, t](const QString& name) {
        if (t != tab())
            return;
        const int row = t->model()->rowOf(name);
        if (row >= 0)
            m_view->setCurrentRow(row);
    });
    connect(t, &DirTab::infoChanged, this, [this, t] {
        if (t == tab())
            updateHeader();
    });
    connect(t, &DirTab::searchChanged, this, [this, t] {
        const int ti = tabIndexOf(t);
        if (ti >= 0)
            updateTabTitles();
        if (t == tab()) {
            updateSearchUi();
            updateStatus();
        }
    });
    FileModel* m = t->model();
    connect(m, &FileModel::statsChanged, this, [this, t] {
        if (t == tab())
            updateStatus();
    });
    connect(m, &FileModel::renameRequested, this, &Panel::doRename);
    connect(m, &FileModel::modelAboutToBeReset, this, [this, t] {
        if (t != tab())
            return;
        m_pathBeforeReset = t->model()->path();
        m_scrollBeforeReset = m_view->verticalScrollBar()->value();
        m_rowBeforeReset = m_view->currentRow();
    });
}

int Panel::lowestFreeSlot() const
{
    for (int n = 1;; ++n) {
        bool used = false;
        for (DirTab* t : m_tabs)
            used |= t->slot == n;
        if (!used)
            return n;
    }
}

void Panel::addTab(const QString& path, bool activate, int slot)
{
    if (slot <= 0)
        slot = lowestFreeSlot();
    for (int i = 0; i < int(m_tabs.size()); ++i) {
        if (m_tabs[size_t(i)]->slot == slot) {
            if (activate)
                showTab(i);
            return;
        }
    }
    auto* t = new DirTab(this);
    t->slot = slot;
    t->setShowHidden(m_showHidden);
    t->model()->setSortSpec(m_sort);
    bindTab(t);
    int index = 0;
    while (index < int(m_tabs.size()) && m_tabs[size_t(index)]->slot < slot)
        ++index;  // always in number order
    m_tabs.insert(m_tabs.begin() + index, t);
    {
        QSignalBlocker block(m_tabBar);
        m_tabBar->insertTab(index, baseName(path));
        if (m_current >= index)
            m_current++;
    }
    m_tabBar->setVisible(m_tabs.size() > 1);
    t->navigate(path);
    updateTabTitles();  // after navigate: the title comes from the path being loaded
    if (activate || m_current < 0)
        showTab(index);
    emit stateChanged();
}

void Panel::closeCurrentTab()
{
    if (m_tabs.size() <= 1 || m_current < 0)
        return;
    const int index = m_current;
    DirTab* t = m_tabs[size_t(index)];
    t->setCursorProvider(nullptr);
    m_tabs.erase(m_tabs.begin() + index);
    m_current = -1;
    {
        QSignalBlocker block(m_tabBar);
        m_tabBar->removeTab(index);
    }
    m_tabBar->setVisible(m_tabs.size() > 1);
    updateTabTitles();
    showTab(qMin(index, int(m_tabs.size()) - 1));
    t->deleteLater();
    emit stateChanged();
}

void Panel::selectSlot(int slot)
{
    addTab(path(), true, slot);  // existing: switch; missing: create here
}

QList<int> Panel::tabSlots() const
{
    QList<int> out;
    for (DirTab* t : m_tabs)
        out.append(t->slot);
    return out;
}

// "1  src", "2  Downloads": the number is the Alt+number shortcut (like yazi).
void Panel::updateTabTitles()
{
    for (int i = 0; i < int(m_tabs.size()); ++i) {
        const DirTab* t = m_tabs[size_t(i)];
        m_tabBar->setTabText(i, QStringLiteral("%1  %2").arg(t->slot).arg(t->title()));
        m_tabBar->setTabToolTip(i, m_tabs[size_t(i)]->path());
    }
}

void Panel::cycleTab(int step)
{
    if (m_tabs.size() < 2)
        return;
    const int n = int(m_tabs.size());
    showTab(((m_current + step) % n + n) % n);
}

QStringList Panel::tabPaths() const
{
    QStringList out;
    for (DirTab* t : m_tabs)
        out.append(t->path());
    return out;
}

void Panel::restoreTabs(const QStringList& paths, int current, const QList<int>& numbers)
{
    for (int i = 0; i < paths.size(); ++i)
        addTab(paths[i], false, numbers.value(i, 0));
    if (m_tabs.empty())
        addTab(QDir::homePath(), true);
    showTab(std::clamp(current, 0, int(m_tabs.size()) - 1));
}

void Panel::showTab(int index)
{
    if (index < 0 || index >= int(m_tabs.size()))
        return;
    endQuickSearch();
    if (DirTab* old = tab()) {
        if (index == m_current && m_view->model() == old->model())
            return;
        old->savedCursor = currentName();
        old->savedScroll = m_view->verticalScrollBar()->value();
        old->setCursorProvider(nullptr);
        m_headerState = m_view->userHeaderState();
    }
    m_current = index;
    DirTab* t = m_tabs[size_t(index)];

    QItemSelectionModel* oldSel = m_view->selectionModel();
    m_view->setModel(t->model());
    delete oldSel;
    if (m_headerState.isEmpty())
        applyDefaultColumns();
    else
        m_view->header()->restoreState(m_headerState);
    m_view->fitColumns();
    {
        QSignalBlocker block(m_view->header());
        const SortSpec s = t->model()->sortSpec();
        m_view->header()->setSortIndicator(int(s.column), s.descending ? Qt::DescendingOrder : Qt::AscendingOrder);
    }
    t->setCursorProvider([this] { return currentName(); });
    {
        QSignalBlocker block(m_filterEdit);
        m_filterEdit->setText(t->model()->filter());
    }
    m_filterBar->setVisible(!t->model()->filter().isEmpty());

    const int row = t->model()->rowOf(t->savedCursor);
    m_view->setCurrentRow(row < 0 ? 0 : row);
    m_view->verticalScrollBar()->setValue(t->savedScroll);

    {
        QSignalBlocker block(m_tabBar);
        m_tabBar->setCurrentIndex(index);
    }
    updateHeader();
    updateStatus();
    emit stateChanged();
}

void Panel::onListingReady(DirTab* t, const QString& focus)
{
    updateTabTitles();
    if (t != tab()) {
        t->savedCursor = focus;
        t->savedScroll = 0;
        return;
    }
    m_filterBar->setVisible(!t->model()->filter().isEmpty());
    const bool sameFolder = t->model()->path() == m_pathBeforeReset;
    int row = focus.isEmpty() ? 0 : t->model()->rowOf(focus);
    if (row < 0)  // the focused item is gone (deleted, moved): stay where it was, not at the top
        row = sameFolder ? qBound(0, m_rowBeforeReset, qMax(0, t->model()->count() - 1)) : 0;
    if (t->isRemote())  // it answered: offer it again under recent servers
        Network::instance().rememberServer(locationAuthority(t->model()->path()));
    if (t->model()->path() == m_pathBeforeReset)
        m_view->verticalScrollBar()->setValue(m_scrollBeforeReset);  // refresh: keep the view still
    m_view->setCurrentRow(row);
    m_pathBeforeReset.clear();
    updateHeader();
    updateStatus();
    emit stateChanged();
}

void Panel::applyDefaultColumns()
{
    QHeaderView* h = m_view->header();
    const QFontMetrics fm(theme().numberFont);
    h->setSectionResizeMode(FileModel::ColName, QHeaderView::Stretch);
    for (int c = FileModel::ColExt; c < FileModel::ColCount; ++c)
        h->setSectionResizeMode(c, QHeaderView::Interactive);
    m_view->setColumnWidth(FileModel::ColExt, fm.horizontalAdvance(QStringLiteral("desktop")) + 24);
    m_view->setColumnWidth(FileModel::ColSize, fm.horizontalAdvance(QStringLiteral("999.9 MiB")) + 24);
    m_view->setColumnWidth(FileModel::ColTime, fm.horizontalAdvance(QStringLiteral("2026-12-31 23:59")) + 24);
    m_view->setColumnWidth(FileModel::ColAttr, fm.horizontalAdvance(QStringLiteral("drwxr-xr-x")) + 24);
}

QByteArray Panel::headerState() const
{
    return m_view->userHeaderState();
}

void Panel::restoreHeaderState(const QByteArray& state)
{
    if (state.isEmpty())
        return;
    m_headerState = state;
    m_view->header()->restoreState(state);
}

// ------------------------------------------------------------ navigation

QString Panel::path() const
{
    DirTab* t = tab();
    return t ? t->path() : QString();
}

const FileEntry* Panel::currentEntry() const
{
    DirTab* t = tab();
    return t ? t->model()->entry(m_view->currentRow()) : nullptr;
}

QString Panel::currentName() const
{
    const FileEntry* e = currentEntry();
    return e ? e->name : QString();
}

QString Panel::currentPath() const
{
    const FileEntry* e = currentEntry();
    if (!e || e->isParent)
        return {};
    return joinPath(path(), e->name);
}

QStringList Panel::selectedPaths() const
{
    DirTab* t = tab();
    if (!t)
        return {};
    if (!m_override.isEmpty())
        return m_override;
    QStringList out;
    for (const QString& n : t->model()->markedNames())
        out.append(joinPath(t->path(), n));
    if (out.isEmpty() && !currentPath().isEmpty())
        out.append(currentPath());
    return out;
}

void Panel::navigate(const QString& p, const QString& focus)
{
    if (DirTab* t = tab())
        t->navigate(p, focus);
}

void Panel::refresh()
{
    if (DirTab* t = tab())
        t->refresh();
}

void Panel::openCurrent()
{
    const FileEntry* e = currentEntry();
    if (!e)
        return;
    if (e->isParent)
        goUp();
    else if (e->isDir || (!inArchive() && !isRemote() && isArchiveName(e->name)))
        navigate(joinPath(path(), e->name));
    else
        emit openFileRequested(joinPath(path(), e->name));
}

void Panel::enterCurrent()
{
    const FileEntry* e = currentEntry();
    if (!e || e->isParent)
        return;
    navigate(joinPath(path(), e->name));
}

bool Panel::isRemote() const
{
    DirTab* t = tab();
    return t && t->isRemote();
}

bool Panel::inArchive() const
{
    DirTab* t = tab();
    return t && t->inArchive();
}

QString Panel::archivePath() const
{
    DirTab* t = tab();
    return t ? t->archivePath() : QString();
}

QString Panel::innerPath(const QString& p) const
{
    const QString a = archivePath();
    if (a.isEmpty() || !p.startsWith(a))
        return {};
    return p.mid(a.size() + 1);
}

void Panel::goUp()
{
    if (DirTab* t = tab())
        t->goUp();
}

void Panel::back()
{
    if (DirTab* t = tab())
        t->back();
}

void Panel::forward()
{
    if (DirTab* t = tab())
        t->forward();
}

void Panel::cancelLoading()
{
    DirTab* t = tab();
    if (t && t->isSearch()) {
        if (t->isSearching()) {
            t->cancelSearch();
            showMessage(tr("Search stopped"));
        } else {
            t->goUp();  // leave the results
        }
        return;
    }
    if (t && t->isLoading()) {
        t->cancelLoading();
        showMessage(tr("Cancelled"));
        updateHeader();
    }
}

// ---------------------------------------------------------- sort & marks

SortSpec Panel::sortSpec() const
{
    return m_sort;
}

void Panel::setSortSpec(const SortSpec& spec)
{
    m_sort = spec;
    for (DirTab* t : m_tabs)
        if (t->model()->sortSpec() != spec)
            t->model()->setSortSpec(spec);
    QSignalBlocker block(m_view->header());
    m_view->header()->setSortIndicator(int(spec.column), spec.descending ? Qt::DescendingOrder : Qt::AscendingOrder);
    m_view->scrollTo(m_view->currentIndex());
}

void Panel::sortBy(SortColumn column)
{
    SortSpec s = sortSpec();
    if (s.column == column) {
        s.descending = !s.descending;
    } else {
        s.column = column;
        // Newest / biggest first is what you want for those columns.
        s.descending = column == SortColumn::Time || column == SortColumn::Size;
    }
    setSortSpec(s);
    emit stateChanged();
}

void Panel::setShowHidden(bool on)
{
    m_showHidden = on;
    for (DirTab* t : m_tabs)
        t->setShowHidden(on);
}

void Panel::markGroup(int mode)
{
    DirTab* t = tab();
    if (!t)
        return;
    if (mode == 0) {
        t->model()->invertMarks();
        return;
    }
    static QString lastPattern = QStringLiteral("*.*");
    bool ok = false;
    const QString pattern = QInputDialog::getText(
        this, mode > 0 ? tr("Mark files") : tr("Unmark files"),
        tr("Patterns, separated by spaces or ';'. Add a trailing / for folders (e.g. build/):"), QLineEdit::Normal,
        lastPattern, &ok);
    if (!ok || pattern.trimmed().isEmpty())
        return;
    lastPattern = pattern;
    t->model()->markByPattern(pattern, mode > 0);
}

void Panel::markAll(bool on)
{
    if (DirTab* t = tab())
        t->model()->markAll(on);
}

void Panel::computeCurrentSize()
{
    const FileEntry* e = currentEntry();
    if (e && e->isDir && !e->isParent)
        tab()->computeSize(e->name);
}

void Panel::computeAllSizes()
{
    if (DirTab* t = tab())
        t->computeAllSizes();
}

void Panel::startRename()
{
    QString why;
    if (inArchive() && !canUpdateArchive(archivePath(), &why)) {
        showMessage(why, true);
        return;
    }
    const FileEntry* e = currentEntry();
    if (!e || e->isParent)
        return;
    m_view->edit(m_view->model()->index(m_view->currentRow(), FileModel::ColName));
}

void Panel::expectName(const QString& name)
{
    if (DirTab* t = tab())
        t->expectName(name);
}

void Panel::doRename(const QString& from, const QString& to)
{
    if (to.contains(QLatin1Char('/')) && !(tab() && tab()->isSearch())) {
        showMessage(tr("A name cannot contain '/'"), true);
        return;
    }
    if (from.contains(QLatin1Char('/')) && !to.contains(QLatin1Char('/'))) {
        // In search results names are relative paths: keep the folder part.
        doRename(from, from.left(from.lastIndexOf(QLatin1Char('/')) + 1) + to);
        return;
    }
    const QString dir = path();
    if (inArchive()) {
        emit archiveRenameRequested(joinPath(dir, from), joinPath(dir, to));
        if (DirTab* t = tab())
            t->expectName(to);
        return;
    }
    QPointer<Panel> self(this);
    QPointer<DirTab> t(tab());
    ioPool()->start([self, t, dir, from, to] {
        const int err = renameNoReplaceAt(joinPath(dir, from), joinPath(dir, to));
        postToUi([self, t, err, dir, from, to] {
            if (!self)
                return;
            if (err) {
                self->showMessage(tr("Cannot rename %1: %2").arg(from, errnoString(err)), true);
                return;
            }
            emit self->renamed(dir, from, to);
            if (t)
                t->expectName(to);
        });
    });
}

// ---------------------------------------------------------- quick search

void Panel::setVimKeys(bool on)
{
    m_view->setVimKeys(on);
}

void Panel::startQuickSearch(const QString& text)
{
    m_search->setText(text);
    positionSearch();
    m_search->show();
    m_search->raise();
    m_search->setFocus();
    quickSearchFind(1, true);
}

void Panel::positionSearch()
{
    const int w = qMin(260, m_view->width() - 20);
    m_search->setFixedWidth(w);
    m_search->move(10, m_view->height() - m_search->sizeHint().height() - 10);
}

void Panel::quickSearchFind(int direction, bool includeCurrent)
{
    DirTab* t = tab();
    if (!t)
        return;
    const FileModel* m = t->model();
    const QString needle = m_search->text();
    const int n = m->count();
    if (needle.isEmpty() || n == 0)
        return;
    const int start = m_view->currentRow();
    // Prefix matches first; fall back to substring matches.
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = includeCurrent ? 0 : 1; i <= n; ++i) {
            const int row = ((start + direction * i) % n + n) % n;
            const FileEntry* e = m->entry(row);
            const bool hit = pass == 0 ? e->name.startsWith(needle, Qt::CaseInsensitive)
                                       : e->name.contains(needle, Qt::CaseInsensitive);
            if (hit && !e->isParent) {
                m_view->setCurrentRow(row);
                m_search->setProperty("miss", false);
                m_search->setStyleSheet(theme().css(QStringLiteral(
                    "QLineEdit { background: @header; color: @text; border: 1px solid @accent;"
                    "  border-radius: 12px; padding: 3px 10px; }")));
                return;
            }
        }
    }
    m_search->setStyleSheet(theme().css(QStringLiteral(
        "QLineEdit { background: @header; color: @error; border: 1px solid @error;"
        "  border-radius: 12px; padding: 3px 10px; }")));
}

void Panel::endQuickSearch()
{
    if (!m_search->isVisible())
        return;
    if (!m_search->text().isEmpty())
        m_lastSearch = m_search->text();  // for vim's n / N
    m_search->hide();
    m_view->setFocus();
}

// ---------------------------------------------------------- presentation

void Panel::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    m_view->setActive(active);
    updateActiveStyle();
    update();
}

void Panel::updateActiveStyle()
{
    const Theme& t = theme();
    m_header->setStyleSheet(t.css(m_active ? QStringLiteral(
        "#panelHeader { background: @headerActive; border-top-left-radius: 7px; border-top-right-radius: 7px; }"
        "QLineEdit { background: transparent; color: @text; font-weight: 600; }"
        "QToolButton#crumb { color: @text; border: none; padding: 1px 4px; border-radius: 4px; }"
        "QToolButton#crumb:hover { background: @border; }"
        "QLabel { color: @dimText; }"
        "QLabel#branch { color: @accent; padding-right: 8px; }"
        "QToolButton#searchChip { color: @accentText; background: @accent; border: none; border-radius: 9px; padding: 1px 8px; }")
                                           : QStringLiteral(
        "#panelHeader { background: @header; border-top-left-radius: 7px; border-top-right-radius: 7px; }"
        "QLineEdit { background: transparent; color: @dimText; font-weight: 600; }"
        "QToolButton#crumb { color: @dimText; border: none; padding: 1px 4px; border-radius: 4px; }"
        "QToolButton#crumb:hover { background: @border; color: @text; }"
        "QLabel { color: @dimText; }"
        "QLabel#branch { color: @dimText; padding-right: 8px; }"
        "QToolButton#searchChip { color: @text; background: @border; border: none; border-radius: 9px; padding: 1px 8px; }")));
}

void Panel::paintEvent(QPaintEvent*)
{
    const Theme& t = theme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(m_active ? t.accent : t.border, 1));
    p.setBrush(t.base);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
}

void Panel::updateHeader()
{
    DirTab* t = tab();
    if (!t)
        return;
    m_pathBar->setPath(t->path());
    updateSearchUi();
    m_branchLabel->setVisible(!t->gitBranch().isEmpty());
    m_branchLabel->setText(QStringLiteral("⎇ %1").arg(t->gitBranch()));
    m_branchLabel->setToolTip(tr("Git branch"));
    m_mountButton->setIcon(t->inArchive()  ? IconCache::themed("package-x-generic", "application-x-archive")
                           : t->isRemote() ? IconCache::themed("network-server", "folder-remote")
                                           : IconCache::themed("drive-harddisk", "folder"));
    m_mountButton->setToolTip(t->inArchive() ? tr("Inside archive %1").arg(t->archivePath())
                                             : tr("Locations (Alt+F1 / Alt+F2)"));
    if (t->freeBytes() >= 0 && t->totalBytes() > 0) {  // GVfs/FUSE often report 0 of 0
        m_freeLabel->setText(tr("%1 free").arg(formatSize(t->freeBytes())));
        m_freeLabel->setToolTip(tr("%1 free of %2").arg(formatSize(t->freeBytes()), formatSize(t->totalBytes())));
    } else {
        m_freeLabel->clear();
    }
    window()->setWindowTitle(QStringLiteral("Tandem — %1").arg(path()));
}

void Panel::updateStatus()
{
    const Theme& t = theme();
    DirTab* tb = tab();
    QString text;
    QColor color = t.dimText;
    if (!m_message.isEmpty()) {
        text = m_message;
        color = m_messageError ? t.error : t.text;
    } else if (tb && tb->isSearch() && tb->isSearching()) {
        text = tr("Searching %1 for “%2” …  %3 found   (Esc stops)")
                   .arg(tb->searchSpec().root, tb->searchSpec().query)
                   .arg(tb->searchFound());
        color = t.accent;
    } else if (tb && tb->isLoading() && !m_loadingTimer.isActive()) {
        text = tr("Loading %1 …   (Esc to cancel)").arg(tb->loadingPath());
        color = t.accent;
    } else if (tb) {
        const FileModel::Stats s = tb->model()->stats();
        if (!tb->model()->filter().isEmpty())
            m_filterInfo->setText(tr("%1 shown, %2 hidden").arg(s.files + s.dirs).arg(tb->model()->hiddenByFilter()));
        if (s.markedFiles + s.markedDirs > 0) {
            text = tr("%1 of %2 files, %3 of %4 folders marked   ·   %5 of %6")
                       .arg(s.markedFiles).arg(s.files).arg(s.markedDirs).arg(s.dirs)
                       .arg(formatSize(s.markedBytes), formatSize(s.bytes));
            color = t.mark;
        } else {
            text = tr("%1 files, %2 folders   ·   %3").arg(s.files).arg(s.dirs).arg(formatSize(s.bytes));
        }
    }
    m_status->setText(text);
    QPalette pal = m_status->palette();
    pal.setColor(QPalette::WindowText, color);
    m_status->setPalette(pal);
}

void Panel::showMessage(const QString& text, bool error)
{
    m_message = text;
    m_messageError = error;
    m_messageTimer.start(error ? 6000 : 2500);
    updateStatus();
}

void Panel::startSearchBar()
{
    if (isRemote()) {
        showMessage(tr("Searching on servers isn't supported yet"), true);
        return;
    }
    if (inArchive()) {
        showMessage(tr("Searching inside archives isn't supported yet"), true);
        return;
    }
    DirTab* t = tab();
    if (t && t->isSearch()) {
        m_searchEdit->setText(t->searchSpec().query);
        m_searchContents->setChecked(t->searchSpec().contents);
        m_searchNames->setChecked(!t->searchSpec().contents);
        m_searchAll->setChecked(t->searchSpec().all);
    }
    m_searchBar->show();
    m_searchEdit->setFocus();
    m_searchEdit->selectAll();
    updateSearchUi();
}

void Panel::runSearch()
{
    DirTab* t = tab();
    const QString q = m_searchEdit->text().trimmed();
    if (!t || q.isEmpty())
        return;
    SearchSpec spec;
    spec.root = t->isSearch() ? t->searchSpec().root : path();
    spec.query = q;
    spec.contents = m_searchContents->isChecked();
    spec.all = m_searchAll->isChecked();
    t->startSearch(spec);
    m_view->setFocus();
}

void Panel::updateSearchUi()
{
    DirTab* t = tab();
    const bool search = t && t->isSearch();
    m_searchChip->setVisible(search);
    if (!search) {
        m_searchInfo->clear();
        return;
    }
    const SearchSpec& s = t->searchSpec();
    const QString count = t->isSearching() ? tr("%1 found…").arg(t->searchFound()) : tr("%1 found").arg(t->searchFound());
    m_searchChip->setText(QStringLiteral("⌕ “%1”  ·  %2  ✕").arg(s.query, count));
    m_searchInfo->setText(t->searchError().isEmpty() ? count : t->searchError());
}

void Panel::startFilter()
{
    m_filterBar->show();
    m_filterEdit->setFocus();
    m_filterEdit->selectAll();
}

void Panel::clearFilter()
{
    m_filterEdit->clear();
    m_filterBar->hide();
    if (DirTab* t = tab())
        t->model()->setFilter({});
    m_view->setFocus();
}

void Panel::focusPathEdit()
{
    m_pathBar->startEditing();
}

void Panel::showMountMenu()
{
    m_mountButton->showMenu();
}

// "smb://bob:secret@nas": the backend gets the password for this session,
// the location (kept in tabs, history, settings) doesn't.
QString Panel::takePassword(const QString& text)
{
    if (!isRemoteLocation(text))
        return text;
    QUrl u(text);
    if (u.password().isEmpty())
        return text;
    const QString password = u.password();
    u.setPassword(QString());
    const QString clean = u.toString();
    Backend* b = backendFor(clean);
    if (!b)
        return text;  // GVfs takes it from the address (and may keep it in the keyring)
    b->offerPassword(clean, u.userName(), password);
    return clean;
}

void Panel::connectTo(const QString& uriIn)
{
    const QString uri = takePassword(uriIn);
    if (hasBackend(uri.section(QLatin1Char(':'), 0, 0))) {  // built in: just go there
        if (!QUrl(uri).path().isEmpty()) {
            navigate(normalizeLocation(uri));
            return;
        }
        // No path: start in the login's home folder, which only the server
        // knows (asking it means connecting, so off the UI thread).
        showMessage(tr("Connecting to %1 …").arg(uri));
        QPointer<Panel> self(this);
        const QString root = normalizeLocation(uri);
        ioPool()->start([self, root] {
            Backend* b = backendFor(root);
            const QString home = b ? b->homeLocation(root) : root;
            postToUi([self, home] {
                if (self)
                    self->navigate(home);
            });
        });
        return;
    }
    if (!Network::available()) {
        showMessage(tr("Tandem was built without GIO: mount with sshfs/rclone and browse the folder"), true);
        return;
    }
    showMessage(tr("Connecting to %1 …").arg(uri));
    QPointer<Panel> self(this);
    const QString user = QUrl(uri).userName().isEmpty() ? QString::fromLocal8Bit(qgetenv("USER")) : QUrl(uri).userName();
    const bool wantsHome = uri.startsWith(QLatin1String("sftp://")) && (QUrl(uri).path().isEmpty() || QUrl(uri).path() == QLatin1String("/"));
    Network::instance().mount(uri, this, [self, uri, user, wantsHome](const QString& path, const QString& error) {
        if (!self)
            return;
        if (!error.isEmpty()) {
            self->showMessage(tr("%1: %2").arg(uri, error), true);
            return;
        }
        QUrl server(uri);  // remembered once it worked; the server, not the folder
        if (!server.host().isEmpty())
            server.setPath(QString());
        Network::instance().rememberServer(server.toString());
        // Off the UI thread (network round trips): right after a reconnect
        // the kernel can briefly still report the folder as missing, so wait
        // a moment for it. A bare sftp://host starts in the home folder.
        ioPool()->start([self, path, user, wantsHome] {
            for (int i = 0; i < 30 && !QFileInfo::exists(path); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const QString home = path + QStringLiteral("/home/") + user;
            const QString target = wantsHome && QFileInfo(home).isDir() ? home : path;
            postToUi([self, target] {
                if (self)
                    self->navigate(target);
            });
        });
    });
}

void Panel::connectDialog()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Connect to server"));
    auto* layout = new QVBoxLayout(&dlg);
    auto* edit = new QComboBox(&dlg);
    edit->setEditable(true);
    edit->addItems(Network::instance().recentServers());
    edit->setCurrentText(QString());
    edit->lineEdit()->setPlaceholderText(QStringLiteral("sftp://user@host/path"));
    edit->setMinimumWidth(460);
    auto* hint = new QLabel(tr("Examples:  sftp://user@host/path  ·  user@host:folder  ·  smb://nas/share  ·  "
                               "davs://cloud.example.com/remote.php/dav/files/me  ·  ftp://host\n"
                               "sftp and smb are built in; the others are mounted with GVfs (other apps see those too)."),
                            &dlg);
    hint->setStyleSheet(theme().css(QStringLiteral("color: @dimText;")));
    hint->setWordWrap(true);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Connect"));
    layout->addWidget(edit);
    layout->addWidget(hint);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    edit->setFocus();
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QString text = edit->currentText().trimmed();
    QString uri = normalizeServerUri(text);
    if (uri.isEmpty() && !text.isEmpty() && !text.contains(QLatin1Char('/')))
        uri = QStringLiteral("sftp://") + text;  // a bare host name
    if (uri.isEmpty()) {
        showMessage(tr("Not a server address: %1").arg(text), true);
        return;
    }
    connectTo(uri);
}

void Panel::disconnectMount(const QString& mountPath)
{
    QPointer<Panel> self(this);
    auto leave = [mountPath](Panel* p) {
        const QString cur = p->path();
        if (cur == mountPath || cur.startsWith(mountPath + QLatin1Char('/')))
            p->navigate(QDir::homePath());
    };
    // Step out first, so nothing in Tandem keeps the mount busy.
    leave(this);
    if (auto* w = window())
        for (Panel* p : w->findChildren<Panel*>())
            leave(p);
    showMessage(tr("Disconnecting…"));
    auto finish = [self, mountPath](const QString& error) {
        if (!self)
            return;
        if (error.isEmpty())
            self->showMessage(tr("Disconnected %1").arg(mountPath));
        else
            self->showMessage(error, true);
    };
    if (Network::instance().canUnmount(mountPath)) {
        Network::instance().unmount(mountPath, this, finish);
        return;
    }
    // A kernel/FUSE mount GIO doesn't manage (sshfs, rclone, …).
    auto* proc = new QProcess(this);
    connect(proc, &QProcess::finished, this, [proc, finish](int code) {
        const QString err = QString::fromLocal8Bit(proc->readAllStandardError()).trimmed();
        proc->deleteLater();
        finish(code == 0 ? QString() : (err.isEmpty() ? tr("Could not unmount") : err));
    });
    proc->start(QStringLiteral("umount"), {mountPath});
}

bool Panel::eventFilter(QObject* obj, QEvent* e)
{
    if (e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape &&
        qobject_cast<QLineEdit*>(obj) && obj->parent() == m_pathBar) {
        m_view->setFocus();
        return false;
    }
    if (obj == m_searchEdit && e->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(e);
        if (ke->key() == Qt::Key_Escape) {
            m_searchBar->hide();
            m_view->setFocus();
            return true;
        }
        if (ke->key() == Qt::Key_Tab) {  // Tab flips names / contents
            (m_searchNames->isChecked() ? m_searchContents : m_searchNames)->setChecked(true);
            return true;
        }
        if (ke->key() == Qt::Key_Down) {
            m_view->setFocus();
            return true;
        }
        return false;
    }
    if (obj == m_filterEdit && e->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(e);
        if (ke->key() == Qt::Key_Escape) {
            clearFilter();
            return true;
        }
        if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter || ke->key() == Qt::Key_Down) {
            m_view->setFocus();
            return true;
        }
        return false;
    }
    if (obj == m_search) {
        if (e->type() == QEvent::FocusOut) {
            m_search->hide();
            return false;
        }
        if (e->type() != QEvent::KeyPress)
            return false;
        auto* ke = static_cast<QKeyEvent*>(e);
        switch (ke->key()) {
        case Qt::Key_Escape:
            endQuickSearch();
            return true;
        case Qt::Key_Down:
            quickSearchFind(1, false);
            return true;
        case Qt::Key_Up:
            quickSearchFind(-1, false);
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            endQuickSearch();
            openCurrent();
            return true;
        case Qt::Key_Tab:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
        case Qt::Key_Home:
        case Qt::Key_End:
        case Qt::Key_Insert:
            endQuickSearch();
            QCoreApplication::sendEvent(m_view, e);
            return true;
        default:
            break;
        }
        // Function keys and shortcuts end the search and go to the window.
        if (ke->key() >= Qt::Key_F1 && ke->key() <= Qt::Key_F35)
            endQuickSearch();
        return false;
    }
    return QFrame::eventFilter(obj, e);
}

}  // namespace tandem
