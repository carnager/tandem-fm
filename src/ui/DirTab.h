#pragma once

#include "core/DirLister.h"
#include "core/Search.h"

#include <QObject>
#include <QTimer>
#include <QHash>
#include <QSet>
#include <QStringList>

#include <atomic>
#include <functional>
#include <memory>

namespace tandem {

class FileModel;

// One tab of a panel: a directory, its model, history and watch.
// All filesystem access is dispatched to the I/O pool. A hung mount only
// leaves this tab "loading"; the UI and every other tab keep working.
class DirTab : public QObject {
    Q_OBJECT
public:
    explicit DirTab(QObject* parent = nullptr);
    ~DirTab() override;

    FileModel* model() const { return m_model; }
    QString path() const;
    QString title() const;
    bool isLoading() const { return m_loading; }
    QString loadingPath() const { return m_loadingPath; }
    int64_t freeBytes() const { return m_free; }
    int64_t totalBytes() const { return m_total; }
    bool showHidden() const { return m_showHidden; }
    bool canGoBack() const { return !m_back.isEmpty(); }
    bool canGoForward() const { return !m_forward.isEmpty(); }
    bool isWatched() const { return m_watch >= 0; }
    // Inside an archive: path() is archivePath() + "/" + the folder inside it.
    bool inArchive() const { return m_inArchive; }
    QString gitBranch() const { return m_gitBranch; }
    bool isRemote() const { return m_remote; }  // listed through a Backend (sftp://, …)

    // Search results as a virtual folder: entries are paths relative to the
    // searched folder, so every file operation keeps working on them.
    void startSearch(const SearchSpec& spec);
    bool isSearch() const { return m_inSearch; }
    bool isSearching() const { return m_searchRunning; }
    const SearchSpec& searchSpec() const { return m_search; }
    int searchFound() const { return m_searchFound; }
    QString searchError() const { return m_searchError; }
    void cancelSearch();
    static void setGitEnabled(bool on);
    void refreshGit();
    QString archivePath() const { return m_archivePath; }

    void navigate(const QString& path, const QString& focusName = {});
    void refresh();
    void goUp();
    void back();
    void forward();
    void cancelLoading();
    void setShowHidden(bool on);
    void computeSize(const QString& name);
    void computeAllSizes();
    // Put the cursor on this name as soon as it appears (after mkdir/rename).
    void expectName(const QString& name);

    // The panel provides the cursor while this tab is visible.
    void setCursorProvider(std::function<QString()> f) { m_cursorProvider = std::move(f); }
    QString cursorName() const { return m_cursorProvider ? m_cursorProvider() : savedCursor; }

    QString savedCursor;
    int savedScroll = 0;
    int slot = 0;  // the tab's fixed number (Alt+slot); tabs are ordered by it

signals:
    void loadingChanged(bool loading);
    void listingReady(const QString& focusName);
    void listingFailed(const QString& path, const QString& message);
    void focusRequested(const QString& name);
    void infoChanged();
    void searchChanged();

private:
    enum class History { Push, Back, Forward, None };
    void startListing(const QString& path, const QString& focus, History history, bool keepMarks);
    void onWatchChanged(int wd, const QStringList& names);
    void flushUpdates();
    void onDirectoryGone(int wd);
    void setWatch(int wd);
    void computeSizes(const QStringList& names);
    void remember(const QString& dir);

    FileModel* m_model;
    quint64 m_gen = 0;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    std::shared_ptr<std::atomic<bool>> m_sizeCancel;
    bool m_loading = false;
    QString m_loadingPath;
    int m_watch = -1;
    int64_t m_free = -1, m_total = -1;
    bool m_showHidden = false;
    bool m_inArchive = false;
    QString m_gitBranch;
    bool m_remote = false;
    SearchSpec m_search;
    bool m_inSearch = false;
    bool m_searchRunning = false;
    int m_searchFound = 0;
    QString m_searchError;
    std::shared_ptr<std::atomic<bool>> m_searchCancel;
    quint64 m_gitGen = 0;
    QTimer* m_gitTimer = nullptr;
    QString m_archivePath;
    QStringList m_back, m_forward;
    // Cursor and marks per folder, restored when coming back to it.
    struct FolderMemory {
        QString cursor;
        QSet<QString> marks;
    };
    QHash<QString, FolderMemory> m_memory;
    QStringList m_memoryOrder;  // oldest first, bounded
    QString m_pendingFocus;
    QSet<QString> m_pendingNames;
    bool m_statInFlight = false;
    std::function<QString()> m_cursorProvider;
};

}  // namespace tandem
