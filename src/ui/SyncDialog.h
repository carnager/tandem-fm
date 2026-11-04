#pragma once

#include "core/FolderCompare.h"

#include <QAbstractTableModel>
#include <QDialog>
#include <QPointer>
#include <QStyledItemDelegate>

#include <functional>
#include <memory>

class QAction;
class QButtonGroup;
class QLabel;
class QPushButton;
class QToolButton;
class QTreeView;

namespace tandem {

class Job;

class SyncModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Action, Item, Why, Size, ColCount };
    // Row groups for the "show" toggles.
    enum Group { GroupToRight, GroupToLeft, GroupDiffers, GroupEqual, GroupDelete, GroupCount };

    using QAbstractTableModel::QAbstractTableModel;
    void setItems(std::vector<CompareItem> items, SyncMode mode);
    void applyMode(SyncMode mode);  // back to the default action for every row
    void setShown(int group, bool on);
    bool isShown(int group) const { return m_shown[group]; }

    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : int(m_visible.size()); }
    int columnCount(const QModelIndex& = {}) const override { return ColCount; }
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;

    const CompareItem& item(int row) const { return m_items[m_visible[size_t(row)]]; }
    SyncAction action(int row) const { return m_actions[m_visible[size_t(row)]]; }
    bool setAction(int row, SyncAction a);  // false if not possible for that row
    QList<SyncAction> possibleActions(int row) const;
    int rowOf(const QString& rel) const;

    const std::vector<CompareItem>& items() const { return m_items; }
    const std::vector<SyncAction>& actions() const { return m_actions; }
    int groupCount(int group) const;
    int groupOf(size_t i) const;

private:
    void rebuildVisible();

    std::vector<CompareItem> m_items;
    std::vector<SyncAction> m_actions;
    std::vector<size_t> m_visible;
    bool m_shown[GroupCount] = {true, true, true, false, true};
};

// Pill for the action, the path with its folder dimmed.
class SyncDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};

// Compare two folder trees and copy (or delete) what differs. The list is
// the plan: one line per difference, what will happen and why. Comparing
// runs off the UI thread and again whenever an option changes.
class SyncDialog : public QDialog {
    Q_OBJECT
public:
    using Runner = std::function<QList<Job*>(const SyncPlan&)>;
    SyncDialog(const QString& left, const QString& right, Runner runner, QWidget* parent = nullptr);
    ~SyncDialog() override;

    void compare();
    bool isComparing() const { return m_comparing; }
    SyncModel* model() const { return m_model; }
    QTreeView* view() const { return m_view; }
    void synchronize();
    void setMode(SyncMode mode);  // the direction toggle

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;
    void done(int r) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void setActionOnSelection(SyncAction a);
    void cycleAction(int visibleRow);
    void showContextMenu(const QPoint& pos);
    void compareFiles(int visibleRow);
    void showDiff(const QString& title, const QString& text);
    void updateSummary();
    void updateFilters();
    void saveOptions();
    SyncMode mode() const;

    QString m_left, m_right;
    Runner m_runner;
    SyncModel* m_model;
    QTreeView* m_view;
    QAction* m_recursive;
    QAction* m_contents;
    QAction* m_hidden;
    QButtonGroup* m_mode;
    QToolButton* m_show[5];
    QLabel* m_status;
    QLabel* m_paths;
    QPushButton* m_syncButton;
    bool m_comparing = false;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    quint64 m_gen = 0;
    QList<QPointer<Job>> m_running;
};

}  // namespace tandem
