#pragma once

#include "core/FileEntry.h"
#include "core/Sorting.h"

#include <QFrame>
#include <QTimer>
#include <QUrl>

#include <vector>

class QLabel;
class QLineEdit;
class QCheckBox;
class QTabBar;
class QToolButton;

namespace tandem {

class DirTab;
class FuzzyPopup;
struct PopupItem;
class PathBar;
class FileView;

class Panel : public QFrame {
    Q_OBJECT
public:
    explicit Panel(QWidget* parent = nullptr);

    DirTab* tab() const;
    FileView* view() const { return m_view; }
    QString path() const;
    const FileEntry* currentEntry() const;
    QString currentName() const;
    QString currentPath() const;      // full path of the cursor entry ("" on "..")
    QStringList selectedPaths() const; // override, else marked entries, else the cursor entry
    // While a context menu runs, commands act on what was right-clicked.
    void setSelectionOverride(const QStringList& paths) { m_override = paths; }
    bool hasSelectionOverride() const { return !m_override.isEmpty(); }
    bool isActive() const { return m_active; }
    void setActive(bool active);

    void navigate(const QString& path, const QString& focus = {});
    void refresh();
    void openCurrent();
    void stepInto();  // like openCurrent, but files are left alone
    void enterCurrent();  // Ctrl+PgDn: also browses into files of any type as archives

    // Archive locations (read-only for now).
    bool inArchive() const;
    bool isRemote() const;  // on a server through a native backend (sftp://, …)
    QString archivePath() const;
    QString innerPath(const QString& path) const;  // path inside the archive
    void goUp();
    void back();
    void forward();
    void cancelLoading();

    // Tabs have fixed numbers (like yazi): slot 0 takes the lowest free one.
    // Asking for a slot that exists just switches to it.
    void addTab(const QString& path, bool activate = true, int slot = 0);
    void closeCurrentTab();
    void cycleTab(int step);
    void selectSlot(int slot);  // Alt+1…9: go to tab N, creating it here if needed
    QStringList tabPaths() const;
    QList<int> tabSlots() const;
    int currentTabIndex() const { return m_current; }
    void restoreTabs(const QStringList& paths, int current, const QList<int>& numbers = {});

    void sortBy(SortColumn column);
    SortSpec sortSpec() const;
    void setSortSpec(const SortSpec& spec);
    void setShowHidden(bool on);
    bool showHidden() const { return m_showHidden; }
    void markGroup(int mode);
    void markAll(bool on);
    void unmarkAll() { markAll(false); }
    void computeCurrentSize();
    void computeAllSizes();
    void startRename();
    void doRename(const QString& from, const QString& to);  // inside the current folder, off the UI thread
    void expectName(const QString& name);
    void focusPathEdit();
    void startFilter();
    void startSearchBar();  // Alt+F7
    void clearFilter();
    void showMountMenu();
    void showLocations();
    void toggleThumbnails();  // remembered for this folder
    void applyViewMode();
    QList<PopupItem> locationItems();
    // sftp://…, smb://…, user@host:path, …: mount through GVfs, then open.
    void connectTo(const QString& uri);
    static QString takePassword(const QString& location);
    void connectDialog();
    void disconnectMount(const QString& path);
    void showMessage(const QString& text, bool error = false);

    void setVimKeys(bool on);
    void setArrowFolders(bool on);
    QByteArray headerState() const;
    void restoreHeaderState(const QByteArray& state);

signals:
    void activated(Panel* panel);
    void switchRequested();
    void openFileRequested(const QString& path);
    void dropRequested(const QList<QUrl>& urls, const QString& targetDir, Qt::DropAction action);
    void stateChanged();
    void contextMenuRequested(Panel* panel, const QPoint& globalPos, bool onItem);
    void renamed(const QString& dir, const QString& from, const QString& to);
    // Inside an archive renames are archive rewrites, which MainWindow queues.
    void archiveRenameRequested(const QString& from, const QString& to);
    void trashRequested();
    void cursorChanged(Panel* panel);
    void paletteRequested();
    void bookmarkJumpRequested(const QString& keys);
    void bookmarkAssignRequested();
    void commandRequested();

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;
    void paintEvent(QPaintEvent* e) override;

private:
    void bindTab(DirTab* t);
    void showTab(int index);
    void onListingReady(DirTab* t, const QString& focus);
    void updateHeader();
    void updateStatus();
    void updateActiveStyle();
    void applyDefaultColumns();
    void startQuickSearch(const QString& text);
    void quickSearchFind(int direction, bool includeCurrent);
    void endQuickSearch();
    void positionSearch();
    int tabIndexOf(DirTab* t) const;
    void updateTabTitles();
    int lowestFreeSlot() const;

    std::vector<DirTab*> m_tabs;
    int m_current = -1;
    bool m_active = false;
    bool m_showHidden = false;
    SortSpec m_sort;

    QTabBar* m_tabBar;
    QWidget* m_header;
    QToolButton* m_mountButton;
    PathBar* m_pathBar;
    QLabel* m_freeLabel;
    QLabel* m_branchLabel;
    QToolButton* m_searchChip;
    QWidget* m_searchBar;
    QLineEdit* m_searchEdit;
    QToolButton* m_searchNames;
    QToolButton* m_searchContents;
    QCheckBox* m_searchAll;
    QLabel* m_searchInfo;
    void runSearch();
    void updateSearchUi();
    FileView* m_view;
    QLabel* m_status;
    QLineEdit* m_search;
    QWidget* m_filterBar;
    QLineEdit* m_filterEdit;
    QLabel* m_filterInfo;

    QTimer m_loadingTimer;
    QTimer m_messageTimer;
    QString m_message;
    bool m_messageError = false;
    QByteArray m_headerState;
    QStringList m_override;
    QString m_lastSearch;
    QString m_pathBeforeReset;
    int m_scrollBeforeReset = 0;
    int m_rowBeforeReset = 0;
    FuzzyPopup* m_locations = nullptr;
};

}  // namespace tandem
