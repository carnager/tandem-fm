#pragma once

#include "core/MultiRename.h"

#include <QAbstractTableModel>
#include <QDialog>
#include <QStyledItemDelegate>
#include <QTimer>

class QButtonGroup;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTableView;
class QToolButton;

namespace tandem {

// Before / after, one row per item.
class RenamePreviewModel : public QAbstractTableModel {
    Q_OBJECT
public:
    using QAbstractTableModel::QAbstractTableModel;
    void setPlan(const RenamePlan& plan);
    const RenamePlan& plan() const { return m_plan; }
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;

private:
    RenamePlan m_plan;
};

// Draws the new name with the part that changes highlighted, and problems
// in red after it.
class RenameDiffDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};

QTableView* makeRenamePreview(RenamePreviewModel* model, QWidget* parent);

// Shows a finished plan (from the text editor) and asks to go ahead.
// Result: Accepted = rename, EditAgain = back to the editor.
class RenameConfirmDialog : public QDialog {
    Q_OBJECT
public:
    enum { EditAgain = 2 };
    RenameConfirmDialog(const RenamePlan& plan, QWidget* parent = nullptr);
    QPushButton* renameButton() const { return m_rename; }

private:
    QPushButton* m_rename;
};

// Ctrl+M: rename many items with a template ("{taken} {n:001}{ext}"), an
// optional find/replace and a case style, all previewed live.
class MultiRenameDialog : public QDialog {
    Q_OBJECT
public:
    enum { EditInEditor = 2 };  // dialog result: hand the names to a text editor
    MultiRenameDialog(QList<RenameItem> items, QString folder, QSet<QString> existing, QWidget* parent = nullptr);

    RenamePlan plan() const { return m_plan; }
    RenameOptions options() const;

    // For tests.
    QLineEdit* templateEdit() const { return m_template; }
    QLineEdit* findEdit() const { return m_find; }
    QPushButton* renameButton() const { return m_rename; }
    QListWidget* completionPopup() const { return m_completions; }

protected:
    void accept() override;
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void recompute();
    void loadSettings();
    void saveSettings() const;
    void insertField(const QString& field);
    void updateCompletions();
    void acceptCompletion();
    void readPhotoDates();
    QToolButton* chip(const QString& text, const QString& field, const QString& tip);

    QList<RenameItem> m_items;
    QString m_folder;
    QSet<QString> m_existing;
    RenamePlan m_plan;
    QTimer m_debounce;
    bool m_datesRead = false;
    bool m_readingDates = false;

    QLineEdit* m_template;
    QToolButton* m_recent;
    QListWidget* m_completions;
    QLineEdit* m_find;
    QLineEdit* m_replace;
    QToolButton* m_regex;
    QToolButton* m_matchCase;
    QButtonGroup* m_case;
    QTableView* m_table;
    RenamePreviewModel* m_model;
    QLabel* m_summary;
    QPushButton* m_rename;
};

}  // namespace tandem
