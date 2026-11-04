#pragma once

#include "core/Servers.h"

#include <QDialog>
#include <QStringList>

#include <functional>

class QButtonGroup;
class QFormLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QWidget;

namespace tandem {

// Saved servers (servers.toml): add, edit, remove, import from ~/.ssh/config.
// Nothing is written until Save; passwords then go to the keyring.
class ServersDialog : public QDialog {
    Q_OBJECT
public:
    // addUri: start with a new server made from this address (a recent one).
    explicit ServersDialog(QWidget* parent, const QString& addUri = {});

    // Connect (saves first) calls this with the server's address.
    std::function<void(const QString& uri)> onConnect;

    QListWidget* list() const { return m_list; }
    QList<SavedServer> servers() const { return m_servers; }
    // Hosts offered by "Import from SSH config" (tests point it elsewhere).
    QString sshConfigPath;

    bool save();
    void importFromSsh();

private:
    void addServer(const SavedServer& s);
    void removeCurrent();
    void showServer(int row);
    void formChanged();
    void updateRow(int row);
    void connectCurrent();

    QList<SavedServer> m_servers;
    QStringList m_passwords;  // per row: typed, stored in the keyring on Save
    bool m_loading = false;
    QListWidget* m_list;
    QWidget* m_form;
    QFormLayout* m_fields;
    QLabel* m_error;
    QLineEdit* m_name;
    QButtonGroup* m_type;
    QLineEdit* m_host;
    QSpinBox* m_port;
    QLineEdit* m_user;
    QLineEdit* m_domain;
    QLineEdit* m_keyFile;
    QWidget* m_keyRow;
    QLineEdit* m_password;
    QLineEdit* m_folder;
    QLabel* m_address;
    QPushButton* m_remove;
    QPushButton* m_connect;
    QPushButton* m_save;
};

}  // namespace tandem
