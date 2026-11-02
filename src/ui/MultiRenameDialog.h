#pragma once

#include "core/MultiRename.h"

#include <QAbstractTableModel>
#include <QDialog>
#include <QTimer>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableView;

namespace tandem {

class RenamePreviewModel : public QAbstractTableModel {
    Q_OBJECT
public:
    using QAbstractTableModel::QAbstractTableModel;
    void setPlan(const RenamePlan& plan);
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;

private:
    RenamePlan m_plan;
};

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

// Ctrl+M: Total Commander style multi-rename with a live preview.
class MultiRenameDialog : public QDialog {
    Q_OBJECT
public:
    enum { EditInEditor = 2 };  // dialog result: hand the names to a text editor
    MultiRenameDialog(QList<RenameItem> items, QString parentFolderName, QSet<QString> existing,
                      QWidget* parent = nullptr);

    RenamePlan plan() const { return m_plan; }
    RenameOptions options() const;

    // For tests: set fields directly.
    QLineEdit* nameMaskEdit() const { return m_nameMask; }
    QLineEdit* searchEdit() const { return m_search; }
    QPushButton* renameButton() const { return m_rename; }

protected:
    void accept() override;

private:
    void recompute();
    void resetFields();
    void loadSettings();
    void saveSettings() const;
    QWidget* insertButton(QLineEdit* target, bool forExtension);

    QList<RenameItem> m_items;
    QString m_parentName;
    QSet<QString> m_existing;
    RenamePlan m_plan;
    QTimer m_debounce;

    QLineEdit* m_nameMask;
    QLineEdit* m_extMask;
    QLineEdit* m_search;
    QLineEdit* m_replace;
    QCheckBox* m_regex;
    QCheckBox* m_caseSensitive;
    QComboBox* m_case;
    QSpinBox* m_start;
    QSpinBox* m_step;
    QSpinBox* m_digits;
    QTableView* m_table;
    RenamePreviewModel* m_model;
    QLabel* m_summary;
    QPushButton* m_rename;
};

}  // namespace tandem
