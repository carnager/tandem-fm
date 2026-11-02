#pragma once

#include "core/FileEntry.h"
#include "core/Sorting.h"

#include <QFrame>
#include <QTimer>
#include <QUrl>

#include <vector>

class QLabel;
class QLineEdit;
class QTabBar;
class QToolButton;

namespace tandem {

class DirTab;
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
    QStringList selectedPaths() const; // marked entries, or the cursor entry
    bool isActive() const { return m_active; }
    void setActive(bool active);

    void navigate(const QString& path, const QString& focus = {});
    void refresh();
    void openCurrent();
    void goUp();
    void back();
    void forward();
    void cancelLoading();

    // position -1: right after the current tab
    void addTab(const QString& path, bool activate = true, int position = -1);
    void closeCurrentTab();
    void cycleTab(int step);
    QStringList tabPaths() const;
    int currentTabIndex() const { return m_current; }
    void restoreTabs(const QStringList& paths, int current);

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
    void expectName(const QString& name);
    void focusPathEdit();
    void showMountMenu();
    void showMessage(const QString& text, bool error = false);

    QByteArray headerState() const;
    void restoreHeaderState(const QByteArray& state);

signals:
    void activated(Panel* panel);
    void switchRequested();
    void openFileRequested(const QString& path);
    void dropRequested(const QList<QUrl>& urls, const QString& targetDir, Qt::DropAction action);
    void stateChanged();

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
    void doRename(const QString& from, const QString& to);
    int tabIndexOf(DirTab* t) const;

    std::vector<DirTab*> m_tabs;
    int m_current = -1;
    bool m_active = false;
    bool m_showHidden = false;
    SortSpec m_sort;

    QTabBar* m_tabBar;
    QWidget* m_header;
    QToolButton* m_mountButton;
    QLineEdit* m_pathEdit;
    QLabel* m_freeLabel;
    FileView* m_view;
    QLabel* m_status;
    QLineEdit* m_search;

    QTimer m_loadingTimer;
    QTimer m_messageTimer;
    QString m_message;
    bool m_messageError = false;
    QByteArray m_headerState;
    QString m_pathBeforeReset;
    int m_scrollBeforeReset = 0;
};

}  // namespace tandem
