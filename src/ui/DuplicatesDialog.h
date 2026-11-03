#pragma once

#include "core/Duplicates.h"

#include <QDialog>

#include <atomic>
#include <memory>

class QButtonGroup;
class QCheckBox;
class QLabel;
class QPushButton;
class QTreeWidget;

namespace tandem {

// Files with the same contents, grouped; tick the ones to get rid of
// ("keep the newest" ticks all but one per group) and move them to the trash.
class DuplicatesDialog : public QDialog {
    Q_OBJECT
public:
    DuplicatesDialog(QString here, QString other, QWidget* parent = nullptr);
    ~DuplicatesDialog() override;

    void search();
    bool isSearching() const { return m_searching; }
    QTreeWidget* results() const { return m_tree; }
    void keep(bool newest);  // ticks all but the newest (oldest) in each group
    QStringList ticked() const;

signals:
    void showRequested(const QString& path);  // put the panel's cursor on it
    void trashRequested(const QStringList& paths);

private:
    void fill();
    void updateSummary();

    QString m_here, m_other;
    QCheckBox* m_both;
    QCheckBox* m_hidden;
    QButtonGroup* m_min;
    QPushButton* m_find;
    QTreeWidget* m_tree;
    QLabel* m_summary;
    QPushButton* m_trash;
    QList<DuplicateGroup> m_groups;
    bool m_searching = false;
    std::shared_ptr<std::atomic<bool>> m_cancel;
};

}  // namespace tandem
