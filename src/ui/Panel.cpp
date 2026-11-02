#include "ui/Panel.h"

#include "core/FileOps.h"
#include "core/Mounts.h"
#include "core/Util.h"
#include "ui/DirTab.h"
#include "ui/FileModel.h"
#include "ui/FileView.h"
#include "ui/IconCache.h"
#include "ui/Theme.h"

#include <QDir>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QScrollBar>
#include <QTabBar>
#include <QToolButton>
#include <QVBoxLayout>

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
    m_tabBar->setMovable(true);
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
    m_pathEdit = new QLineEdit(m_header);
    m_pathEdit->setFrame(false);
    m_pathEdit->setFocusPolicy(Qt::ClickFocus);
    m_pathEdit->installEventFilter(this);
    m_freeLabel = new QLabel(m_header);
    hl->addWidget(m_mountButton);
    hl->addWidget(m_pathEdit, 1);
    hl->addWidget(m_freeLabel);
    layout->addWidget(m_header);

    m_view = new FileView(this);
    layout->addWidget(m_view, 1);

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
    connect(m_tabBar, &QTabBar::tabMoved, this, [this](int from, int to) {
        DirTab* moved = m_tabs[size_t(from)];
        m_tabs.erase(m_tabs.begin() + from);
        m_tabs.insert(m_tabs.begin() + to, moved);
        m_current = m_tabBar->currentIndex();
        emit stateChanged();
    });
    connect(m_tabBar, &QTabBar::tabBarDoubleClicked, this, [this](int i) {
        if (i >= 0) {
            showTab(i);
            closeCurrentTab();
        }
    });

    connect(m_pathEdit, &QLineEdit::returnPressed, this, [this] {
        QString text = m_pathEdit->text().trimmed();
        if (!text.startsWith(QLatin1Char('/')) && !text.startsWith(QLatin1Char('~')) &&
            !text.startsWith(QLatin1String("file:")))
            text = joinPath(path(), text);
        navigate(expandUserPath(text));
        m_view->setFocus();
    });

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
        for (const MountPoint& m : listMounts()) {
            if (m.path == QLatin1String("/"))
                continue;
            const char* icon = m.isNetwork ? "network-server" : (m.isRemovable ? "drive-removable-media" : "drive-harddisk");
            QAction* a = menu->addAction(IconCache::themed(icon, "drive-harddisk"),
                                         QStringLiteral("%1   (%2)").arg(m.path, m.fsType),
                                         this, [this, p = m.path] { navigate(p); });
            a->setToolTip(m.device);
        }
    });

    connect(m_view, &FileView::focused, this, [this] { emit activated(this); });
    connect(m_view, &FileView::openRequested, this, &Panel::openCurrent);
    connect(m_view, &FileView::parentRequested, this, &Panel::goUp);
    connect(m_view, &FileView::switchPanelRequested, this, &Panel::switchRequested);
    connect(m_view, &FileView::escapePressed, this, &Panel::cancelLoading);
    connect(m_view, &FileView::quickSearchRequested, this, &Panel::startQuickSearch);
    connect(m_view, &FileView::markGroupRequested, this, &Panel::markGroup);
    connect(m_view, &FileView::historyRequested, this, [this](int step) { step < 0 ? back() : forward(); });
    connect(m_view, &FileView::dropRequested, this, &Panel::dropRequested);
    connect(m_view, &FileView::dragPathsRequested, this, [this](QStringList& out) { out = selectedPaths(); });
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
    });
}

void Panel::addTab(const QString& path, bool activate, int position)
{
    auto* t = new DirTab(this);
    t->setShowHidden(m_showHidden);
    t->model()->setSortSpec(m_sort);
    bindTab(t);
    const int index = position >= 0 ? qMin(position, int(m_tabs.size())) : (m_current < 0 ? 0 : m_current + 1);
    m_tabs.insert(m_tabs.begin() + index, t);
    {
        QSignalBlocker block(m_tabBar);
        m_tabBar->insertTab(index, baseName(path));
        if (m_current >= index)
            m_current++;
    }
    m_tabBar->setVisible(m_tabs.size() > 1);
    t->navigate(path);
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
    showTab(qMin(index, int(m_tabs.size()) - 1));
    t->deleteLater();
    emit stateChanged();
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

void Panel::restoreTabs(const QStringList& paths, int current)
{
    for (const QString& p : paths)
        addTab(p, false, int(m_tabs.size()));
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
        m_headerState = m_view->header()->saveState();
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
    {
        QSignalBlocker block(m_view->header());
        const SortSpec s = t->model()->sortSpec();
        m_view->header()->setSortIndicator(int(s.column), s.descending ? Qt::DescendingOrder : Qt::AscendingOrder);
    }
    t->setCursorProvider([this] { return currentName(); });

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
    const int ti = tabIndexOf(t);
    if (ti >= 0)
        m_tabBar->setTabText(ti, t->title());
    if (t != tab()) {
        t->savedCursor = focus;
        t->savedScroll = 0;
        return;
    }
    int row = focus.isEmpty() ? 0 : t->model()->rowOf(focus);
    if (row < 0)
        row = 0;
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
    m_view->setColumnWidth(FileModel::ColExt, fm.horizontalAdvance(QStringLiteral("extn")) + 24);
    m_view->setColumnWidth(FileModel::ColSize, fm.horizontalAdvance(QStringLiteral("999.9 MiB")) + 24);
    m_view->setColumnWidth(FileModel::ColTime, fm.horizontalAdvance(QStringLiteral("2026-12-31 23:59")) + 24);
    m_view->setColumnWidth(FileModel::ColAttr, fm.horizontalAdvance(QStringLiteral("drwxr-xr-x")) + 24);
}

QByteArray Panel::headerState() const
{
    return m_view->header()->saveState();
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
    else if (e->isDir)
        navigate(joinPath(path(), e->name));
    else
        emit openFileRequested(joinPath(path(), e->name));
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
    if (to.contains(QLatin1Char('/'))) {
        showMessage(tr("A name cannot contain '/'"), true);
        return;
    }
    const QString dir = path();
    QPointer<Panel> self(this);
    QPointer<DirTab> t(tab());
    ioPool()->start([self, t, dir, from, to] {
        const int err = renameNoReplace(joinPath(dir, from), joinPath(dir, to));
        postToUi([self, t, err, from, to] {
            if (!self)
                return;
            if (err)
                self->showMessage(tr("Cannot rename %1: %2").arg(from, errnoString(err)), true);
            else if (t)
                t->expectName(to);
        });
    });
}

// ---------------------------------------------------------- quick search

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
        "QLabel { color: @dimText; }")
                                           : QStringLiteral(
        "#panelHeader { background: @header; border-top-left-radius: 7px; border-top-right-radius: 7px; }"
        "QLineEdit { background: transparent; color: @dimText; font-weight: 600; }"
        "QLabel { color: @dimText; }")));
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
    if (!m_pathEdit->hasFocus())
        m_pathEdit->setText(t->path());
    if (t->freeBytes() >= 0) {
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
    } else if (tb && tb->isLoading() && !m_loadingTimer.isActive()) {
        text = tr("Loading %1 …   (Esc to cancel)").arg(tb->loadingPath());
        color = t.accent;
    } else if (tb) {
        const FileModel::Stats s = tb->model()->stats();
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

void Panel::focusPathEdit()
{
    m_pathEdit->setFocus();
    m_pathEdit->selectAll();
}

void Panel::showMountMenu()
{
    m_mountButton->showMenu();
}

bool Panel::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == m_pathEdit) {
        if (e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape) {
            m_pathEdit->setText(path());
            m_view->setFocus();
            return true;
        }
        if (e->type() == QEvent::FocusIn)
            emit activated(this);
        if (e->type() == QEvent::FocusOut)
            m_pathEdit->setText(path());
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
