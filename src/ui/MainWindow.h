#pragma once

#include "core/FileOps.h"
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
    void populateIconThemeMenu(QMenu* menu);
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
    QList<IconTheme> m_iconThemes;
    bool m_iconThemesLoaded = false;
};

}  // namespace tandem
