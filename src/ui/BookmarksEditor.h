#pragma once

#include <QWidget>

class QLabel;
class QPushButton;
class QTreeWidget;

namespace tandem {

// All bookmarks as a table: keys and folders edited in place, added,
// removed and reordered. Nothing changes until save().
class BookmarksEditor : public QWidget {
    Q_OBJECT
public:
    explicit BookmarksEditor(QWidget* parent);

    void setCurrentFolder(const QString& path) { m_currentFolder = path; }
    QTreeWidget* table() const { return m_table; }
    // Empty when fine, else what's wrong (and that row is selected).
    QString check();
    void save();

private:
    void addRow(const QString& key, const QString& path);

    QTreeWidget* m_table;
    QPushButton* m_remove;
    QString m_currentFolder;
};

}  // namespace tandem
