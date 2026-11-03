#include "core/Keyring.h"

#include <QMutex>

#ifdef TANDEM_HAVE_LIBSECRET
#pragma push_macro("signals")  // Qt's keyword clashes with a GLib struct member
#undef signals
#include <libsecret/secret.h>
#pragma pop_macro("signals")
#endif

namespace tandem {

namespace {
QMutex g_hooksMutex;
KeyringHooks g_hooks;
bool g_hooked = false;
}  // namespace

void setKeyringHooksForTests(KeyringHooks hooks)
{
    QMutexLocker lock(&g_hooksMutex);
    g_hooks = std::move(hooks);
    g_hooked = true;
}

bool keyringAvailable()
{
    {
        QMutexLocker lock(&g_hooksMutex);
        if (g_hooked)
            return true;
    }
#ifdef TANDEM_HAVE_LIBSECRET
    return true;
#else
    return false;
#endif
}

std::optional<StoredLogin> findLogin(const QString& protocol, const QString& server, const QString& user, int port)
{
    {
        QMutexLocker lock(&g_hooksMutex);
        if (g_hooked)
            return g_hooks.find ? g_hooks.find(protocol, server, user) : std::nullopt;
    }
#ifdef TANDEM_HAVE_LIBSECRET
    GHashTable* attrs = g_hash_table_new(g_str_hash, g_str_equal);
    const QByteArray p = protocol.toUtf8(), s = server.toUtf8(), u = user.toUtf8(), portText = QByteArray::number(port);
    g_hash_table_insert(attrs, const_cast<char*>("protocol"), const_cast<char*>(p.constData()));
    g_hash_table_insert(attrs, const_cast<char*>("server"), const_cast<char*>(s.constData()));
    if (!user.isEmpty())
        g_hash_table_insert(attrs, const_cast<char*>("user"), const_cast<char*>(u.constData()));
    if (port > 0)
        g_hash_table_insert(attrs, const_cast<char*>("port"), const_cast<char*>(portText.constData()));
    GError* error = nullptr;
    GList* found = secret_password_searchv_sync(SECRET_SCHEMA_COMPAT_NETWORK, attrs,
                                                SecretSearchFlags(SECRET_SEARCH_UNLOCK | SECRET_SEARCH_LOAD_SECRETS),
                                                nullptr, &error);
    g_hash_table_unref(attrs);
    std::optional<StoredLogin> out;
    if (error)
        g_error_free(error);
    for (GList* l = found; l && !out; l = l->next) {
        auto* item = SECRET_RETRIEVABLE(l->data);
        SecretValue* value = secret_retrievable_retrieve_secret_sync(item, nullptr, nullptr);
        if (!value)
            continue;
        GHashTable* a = secret_retrievable_get_attributes(item);
        StoredLogin login;
        login.user = QString::fromUtf8(static_cast<const char*>(g_hash_table_lookup(a, "user")));
        login.domain = QString::fromUtf8(static_cast<const char*>(g_hash_table_lookup(a, "domain")));
        login.password = QString::fromUtf8(secret_value_get_text(value));
        g_hash_table_unref(a);
        secret_value_unref(value);
        out = login;
    }
    g_list_free_full(found, g_object_unref);
    return out;
#else
    (void)protocol, (void)server, (void)user, (void)port;
    return std::nullopt;
#endif
}

bool storeLogin(const QString& protocol, const QString& server, const StoredLogin& login, int port)
{
    {
        QMutexLocker lock(&g_hooksMutex);
        if (g_hooked)
            return g_hooks.store ? g_hooks.store(protocol, server, login) : false;
    }
#ifdef TANDEM_HAVE_LIBSECRET
    GHashTable* attrs = g_hash_table_new(g_str_hash, g_str_equal);
    const QByteArray p = protocol.toUtf8(), s = server.toUtf8(), u = login.user.toUtf8(), d = login.domain.toUtf8(),
                     portText = QByteArray::number(port);
    g_hash_table_insert(attrs, const_cast<char*>("protocol"), const_cast<char*>(p.constData()));
    g_hash_table_insert(attrs, const_cast<char*>("server"), const_cast<char*>(s.constData()));
    g_hash_table_insert(attrs, const_cast<char*>("user"), const_cast<char*>(u.constData()));
    if (!login.domain.isEmpty())
        g_hash_table_insert(attrs, const_cast<char*>("domain"), const_cast<char*>(d.constData()));
    if (port > 0)
        g_hash_table_insert(attrs, const_cast<char*>("port"), const_cast<char*>(portText.constData()));
    const QByteArray label = QStringLiteral("%1@%2 (%3)").arg(login.user, server, protocol).toUtf8();
    GError* error = nullptr;
    const bool ok = secret_password_storev_sync(SECRET_SCHEMA_COMPAT_NETWORK, attrs, SECRET_COLLECTION_DEFAULT,
                                                label.constData(), login.password.toUtf8().constData(), nullptr, &error);
    g_hash_table_unref(attrs);
    if (error)
        g_error_free(error);
    return ok;
#else
    (void)protocol, (void)server, (void)login, (void)port;
    return false;
#endif
}

}  // namespace tandem
