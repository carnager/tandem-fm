#pragma once

#include <QDialog>

class QKeySequenceEdit;
class QLabel;
class QLineEdit;
class QListWidget;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QPushButton;

namespace tandem {

class MainWindow;

// Ctrl+,: appearance, behaviour, external tools and key bindings. Changes
// apply right away; language changes on the next start.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(MainWindow* window);

    void showPage(int index);  // 0 appearance, 1 behaviour, 2 tools, 3 keys
    // For tests.
    QTreeWidget* keyList() const { return m_keys; }
    QKeySequenceEdit* keyEdit() const { return m_keyEdit; }
    QLabel* keyNote() const { return m_keyNote; }

private:
    QWidget* appearancePage();
    QWidget* behaviourPage();
    QWidget* toolsPage();
    QWidget* keysPage();
    void fillKeys();
    void keySelected();
    void assignKeys(const QKeySequence& seq);

    MainWindow* m_window;
    QListWidget* m_nav;
    QStackedWidget* m_pages;
    QTreeWidget* m_keys = nullptr;
    QLineEdit* m_keyFilter = nullptr;
    QKeySequenceEdit* m_keyEdit = nullptr;
    QLabel* m_keyNote = nullptr;
    QPushButton* m_keyClear = nullptr;
    QPushButton* m_keyReset = nullptr;
    bool m_loadingKey = false;
};

// Keys the user changed, stored per command id ("keys/<id>"); applied at start.
void applyKeyBindings(const QList<class QAction*>& actions);
void saveKeyBinding(QAction* action);  // drops the setting when it's the default again

}  // namespace tandem
