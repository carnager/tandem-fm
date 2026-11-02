#pragma once

#include "core/FileOps.h"
#include "core/MultiRename.h"
#include "ui/FuzzyPopup.h"
#include "ui/IconCache.h"

#include <QHash>
#include <QMainWindow>
#include <QUrl>

#include <functional>

class QMenu;
class QSplitter;
class QVBoxLayout;

namespace tandem {

class JobManager;
class JobsPanel;
class Panel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const QStringList& startPaths);

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    Panel* active() const { return m_active; }
    Panel* other() const;
    void setActive(Panel* p);

    QAction* addCommand(QMenu* menu, const QString& text, const QList<QKeySequence>& keys,
                        std::function<void()> fn);
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
    // Extracts a file from the panel's archive to the cache (off-thread), then calls fn.
    void withExtracted(Panel* p, const QString& path, std::function<void(const QString& local)> fn);
    void openFrom(Panel* p, const QString& path);
    void pack();
    void multiRename();
    void renameInEditor();
    bool collectRenameItems(Panel* p, QList<RenameItem>& items, QSet<QString>& existing);
    void startEditorRename(Panel* p, const QList<RenameItem>& items, const QSet<QString>& existing);
    void runRenamePlan(Panel* p, const RenamePlan& plan);
    void undoMultiRename();
    void unpack();
    void populateIconThemeMenu(QMenu* menu);
    void commandPalette();
    void jumpToFolder();
    QList<PopupItem> folderItems() const;
    void togglePin();
    void setIconTheme(const QString& id);

    void loadSettings(const QStringList& startPaths);
    void saveSettings();

    Panel* m_left;
    Panel* m_right;
    Panel* m_active = nullptr;
    QSplitter* m_splitter;
    JobManager* m_jobs;
    JobsPanel* m_jobsPanel;
    QAction* m_hiddenAction = nullptr;
    QHash<QString, QAction*> m_actions;
    FuzzyPopup* m_popup = nullptr;
    QList<IconTheme> m_iconThemes;
    bool m_iconThemesLoaded = false;
    struct LastRename {
        QString dir;
        QList<QPair<QString, QString>> pairs;  // (current name, previous name)
    } m_lastRename;
};

}  // namespace tandem
