#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace tandem {

// Locations › Bookmarks, laid out like the servers: the bookmarks as a list
// (dragged into order), the selected one's key and folder in a form.
// Nothing changes until save().
class BookmarksEditor : public QWidget {
    Q_OBJECT
public:
    explicit BookmarksEditor(QWidget* parent);

    void setCurrentFolder(const QString& path) { m_currentFolder = path; }
    QListWidget* list() const { return m_list; }
    QLineEdit* keyEdit() const { return m_key; }
    QLineEdit* folderEdit() const { return m_folder; }
    // Empty when fine, else what's wrong (and that bookmark is selected).
    QString check();
    void save();

signals:
    void edited();

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void addRow(const QString& key, const QString& path);
    void removeCurrent();
    void showCurrent();
    void formChanged();
    void updateRow(QListWidgetItem* item);

    QListWidget* m_list;
    QWidget* m_form;
    QLabel* m_heading;
    QLabel* m_address;
    QLabel* m_empty;
    QLineEdit* m_key;
    QLineEdit* m_folder;
    QPushButton* m_remove;
    QString m_currentFolder;
    bool m_loading = false;
};

}  // namespace tandem
