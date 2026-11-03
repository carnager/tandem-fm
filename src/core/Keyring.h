#pragma once

#include <QString>

#include <functional>
#include <optional>

namespace tandem {

// Logins in the desktop's keyring (Secret Service: GNOME Keyring, KWallet),
// stored the way GVfs and Nautilus store network passwords, so they share
// them both ways. Blocking (D-Bus): call from workers. Without libsecret
// at build time nothing is found and nothing is stored.
struct StoredLogin {
    QString user;
    QString domain;
    QString password;
};

bool keyringAvailable();
// user empty: any login stored for that server.
std::optional<StoredLogin> findLogin(const QString& protocol, const QString& server, const QString& user = {}, int port = 0);
bool storeLogin(const QString& protocol, const QString& server, const StoredLogin& login, int port = 0);

// Tests replace the keyring with their own (never the real one).
struct KeyringHooks {
    std::function<std::optional<StoredLogin>(const QString& protocol, const QString& server, const QString& user)> find;
    std::function<bool(const QString& protocol, const QString& server, const StoredLogin& login)> store;
};
void setKeyringHooksForTests(KeyringHooks hooks);

}  // namespace tandem
