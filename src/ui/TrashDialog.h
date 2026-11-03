#pragma once

#include "core/Trash.h"

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;

namespace tandem {

// What's in the trash (all of them, other apps' deletions too): restore,
// delete for good, or empty it.
class TrashDialog : public QDialog {
    Q_OBJECT
public:
    explicit TrashDialog(QWidget* parent = nullptr);
    void reload();
    // For tests.
    QTreeWidget* list() const { return m_list; }
    bool isLoading() const { return m_loading; }
    void restoreSelected();
    void deleteSelected(bool confirm = true);

signals:
    void changed();  // something was restored or deleted (panels refresh)

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    QList<TrashItem> selectedItems() const;
    void fill();
    void updateButtons();
    void run(std::function<QStringList()> work, const QString& done);

    QList<TrashItem> m_items;
    QLineEdit* m_filter;
    QTreeWidget* m_list;
    QLabel* m_summary;
    QPushButton* m_restore;
    QPushButton* m_delete;
    QPushButton* m_empty;
    bool m_loading = false;
};

}  // namespace tandem
