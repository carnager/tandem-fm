#pragma once

#include "core/Permissions.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

namespace tandem {

// Alt+A: who may read, change and run the selected items, the extra users
// and groups (ACLs) and filesystem flags, with a preview of the change.
// Items on servers get the basic bits only.
class PermissionsDialog : public QDialog {
    Q_OBJECT
public:
    PermissionsDialog(QStringList paths, QList<PermInfo> infos, bool remote, QWidget* parent = nullptr);

    PermChange change() const;
    // For tests.
    QCheckBox* bitBox(int who, int perm) const;  // who: 0 owner, 1 group, 2 others; perm: 0 r, 1 w, 2 x
    QLineEdit* octalEdit() const { return m_octal; }
    QLineEdit* addEdit() const { return m_add; }
    QPushButton* applyButton() const { return m_apply; }
    QCheckBox* recursiveBox() const { return m_recursive; }
    QCheckBox* inheritBox() const { return m_inherit; }

private:
    struct Row {
        AclEntry entry;
        QCheckBox* box[3];
        QWidget* widget;
    };
    void addAclRow(const AclEntry& e);
    void addPerson(const QString& text);
    uint32_t checkedBits() const;  // bits that are fully on
    void syncOctal();
    void updatePreview();

    QStringList m_paths;
    QList<PermInfo> m_infos;
    bool m_remote;
    bool m_editable = true;
    QCheckBox* m_bits[12] = {};  // 0..8: rwx for owner/group/other, 9 setuid, 10 setgid, 11 sticky
    Qt::CheckState m_initial[12] = {};
    QComboBox* m_owner = nullptr;
    QComboBox* m_group = nullptr;
    QLineEdit* m_octal = nullptr;
    QLabel* m_symbolic = nullptr;
    QVBoxLayout* m_aclRows = nullptr;
    QList<Row> m_rows;
    QList<AclEntry> m_initialAcl;
    QLineEdit* m_add = nullptr;
    QCheckBox* m_inherit = nullptr;
    bool m_initialInherit = false;
    QList<QPair<uint32_t, QCheckBox*>> m_flags;
    QList<Qt::CheckState> m_initialFlags;
    QCheckBox* m_recursive = nullptr;
    QLabel* m_preview = nullptr;
    QPushButton* m_apply = nullptr;
    bool m_syncing = false;
};

}  // namespace tandem
