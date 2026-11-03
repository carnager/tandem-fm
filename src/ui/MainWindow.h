#pragma once

#include "core/Archive.h"

#include "core/FileOps.h"
#include "core/MultiRename.h"
#include "core/Undo.h"
#include "ui/FuzzyPopup.h"
#include "ui/IconCache.h"

#include <QHash>
#include <QMainWindow>
#include <QUrl>

#include <functional>

class QMenu;
class QSplitter;
class QVBoxLayout;

class QFileSystemWatcher;
class QTimer;

namespace tandem {

class JobManager;
class ViewerWindow;
class JobsPanel;
class Panel;

class MainWindow : public QMainWindow {
    Q_OBJECT
    friend class SettingsDialog;
public:
    explicit MainWindow(const QStringList& startPaths);

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;
    void closeEvent(QCloseEvent* e) override;

private:
    Panel* active() const { return m_active; }
    Panel* other() const;
    void setActive(Panel* p);

    QAction* addCommand(QMenu* menu, const char* source, const QList<QKeySequence>& keys, std::function<void()> fn);
    QAction* addCommand(QMenu* menu, const QString& text, const QList<QKeySequence>& keys, std::function<void()> fn,
                        const QString& id);
    void createMenus();
    QWidget* createFKeyBar();

    void copyOrMove(CopyJob::Mode mode);
    void deleteSelected(bool permanent);
    void makeFolder();
    void viewCurrent();
    void editCurrent();
    void openFile(const QString& path);
    void handleDrop(const QList<QUrl>& urls, const QString& targetDir, Qt::DropAction action);
    void copyToClipboard(bool cut);
    void copyPathsAsText();
    void paste();
    void swapPanels();
    void openInPanel(Panel* target);
    void showShortcuts();
    void showContextMenu(Panel* panel, const QPoint& pos, bool onItem);
    void addOpenWithMenu(QMenu* menu, const QStringList& paths);
    void makeFile();
    void openTerminal(const QString& dir = {});
    void pasteInto(const QString& dir);
    bool refuseInArchive(Panel* p);
    Panel* previewOwner(QWidget* w) const;
    void returnFocusFromPreview(QWidget* w);
    // Archive changes run as one rewrite job. false (with a message) when the
    // archive's format can't be written.
    bool changeArchive(Panel* p, ArchiveChanges changes, const QString& title);
    bool addToArchive(Panel* p, const QStringList& localPaths, const QString& innerDir, bool move);
    Panel* archivePanelFor(const QString& path) const;
    void editInArchive(Panel* p, const QString& path);
    void watchEdit(const QString& local);
    bool refuseOnServer(Panel* p, const QString& what);
    // Archives: extract; servers: download (cached). Then fn(local path).
    // `local` overrides where a server file is put (edit copies live apart
    // from preview copies, so a preview never overwrites unsaved edits).
    void withLocalCopy(Panel* p, const QString& path, std::function<void(const QString& local)> fn,
                       const QString& local = {});
    void editRemote(Panel* p, const QString& path);
    void editedFileChanged(const QString& local);
    void uploadEdited();
    QFileSystemWatcher* m_editWatch = nullptr;
    QTimer* m_uploadTimer = nullptr;
    QSet<QString> m_changedEdits;
    QHash<QString, QString> m_editedRemote;  // local copy -> server location
    QHash<QString, QPair<QString, QString>> m_editedArchive;  // local copy -> (archive, member)
    // Extracts a file from the panel's archive to the cache (off-thread), then calls fn.
    void withExtracted(Panel* p, const QString& path, std::function<void(const QString& local)> fn);
    void openFrom(Panel* p, const QString& path);
    void pack();
    void multiRename();
    void editPermissions();
    void createLink();
    void renameInEditor();
    bool collectRenameItems(Panel* p, QList<RenameItem>& items, QSet<QString>& existing);
    void startEditorRename(Panel* p, const QList<RenameItem>& items, const QSet<QString>& existing);
    void runRenamePlan(Panel* p, const RenamePlan& plan);
    void undo();
    void pushUndo(UndoEntry entry);
    void updateUndoAction();
    void recordJobUndo(Job* job);
    void notifyFinished(Job* job);
    void unpack();
    void populateIconThemeMenu(QMenu* menu);
    void commandPalette();
    void jumpToFolder();
    QList<PopupItem> folderItems() const;
    void toggleQuickView();
    void updateQuickView();
    void setIconTheme(const QString& id);

    void loadSettings(const QStringList& startPaths);
    void saveSettings();

    Panel* m_left;
    Panel* m_right;
    Panel* m_active = nullptr;
    QSplitter* m_splitter;       // left | right (| preview when on the right)
    QSplitter* m_vsplit;         // panels / preview when below both
    QSplitter* m_leftCol;        // left panel / its own preview
    QSplitter* m_rightCol;       // right panel / its own preview
    QString previewMode() const;  // "each" (default), "bottom", "right"
    void setPreviewPosition(const QString& code);
    void openSettings(int page = 0);
    void showTrash();
    void findDuplicates();
    void showDiskUsage();
    void closePreview();
    void updatePreviewFor(ViewerWindow* preview, Panel* panel);
    void openCommandBar();
    void compareFolders();  // Shift+F2: mark what differs
    void synchronizeFolders();
    void runCommand(const QString& command, bool inTerminal);
    class CommandBar* m_commandBar = nullptr;
    JobManager* m_jobs;
    JobsPanel* m_jobsPanel;
    QAction* m_hiddenAction = nullptr;
    QHash<QString, QAction*> m_actions;
    FuzzyPopup* m_popup = nullptr;
    class KeyChordPopup* m_chords = nullptr;
    ViewerWindow* m_quickView = nullptr;   // the preview (left one in "each" mode)
    ViewerWindow* m_quickView2 = nullptr;  // the right panel's own preview in "each" mode
    QTimer* m_quickTimer = nullptr;
    double m_panelRatio = 0.5;  // left panel's share of the two panels
    QList<IconTheme> m_iconThemes;
    bool m_iconThemesLoaded = false;
    QList<UndoEntry> m_undo;
};

}  // namespace tandem
