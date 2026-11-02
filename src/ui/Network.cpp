// GIO's headers use "signals" as a struct member; include them before Qt's
// keyword macros exist.
#ifdef TANDEM_HAVE_GIO
#include <gio/gio.h>
#endif

#include "ui/Network.h"

#include "core/Remote.h"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

namespace tandem {

Network& Network::instance()
{
    static Network* n = new Network;
    return *n;
}

bool Network::available()
{
#ifdef TANDEM_HAVE_GIO
    return true;
#else
    return false;
#endif
}

QStringList Network::recentServers() const
{
    return QSettings().value(QStringLiteral("network/recent")).toStringList();
}

void Network::rememberServer(const QString& uri)
{
    QStringList list = recentServers();
    list.removeAll(uri);
    list.prepend(uri);
    while (list.size() > 12)
        list.removeLast();
    QSettings().setValue(QStringLiteral("network/recent"), list);
}

void Network::forgetServer(const QString& uri)
{
    QStringList list = recentServers();
    list.removeAll(uri);
    QSettings().setValue(QStringLiteral("network/recent"), list);
}

#ifdef TANDEM_HAVE_GIO

namespace {

QString takeError(GError* err)
{
    const QString s = err ? QString::fromUtf8(err->message) : QString();
    if (err)
        g_error_free(err);
    return s;
}

QString pathOfMount(GMount* mount)
{
    GFile* root = g_mount_get_root(mount);
    char* p = g_file_get_path(root);
    const QString path = p ? QString::fromLocal8Bit(p) : QString();
    g_free(p);
    g_object_unref(root);
    return path;
}

QString iconNameOf(GIcon* icon)
{
    QString name;
    if (icon && G_IS_THEMED_ICON(icon)) {
        const gchar* const* names = g_themed_icon_get_names(G_THEMED_ICON(icon));
        if (names && names[0])
            name = QString::fromUtf8(names[0]);
    }
    if (icon)
        g_object_unref(icon);
    return name;
}

QString volumeIdOf(GVolume* v)
{
    char* uuid = g_volume_get_identifier(v, G_VOLUME_IDENTIFIER_KIND_UUID);
    char* name = g_volume_get_name(v);
    const QString id = QString::fromUtf8(uuid ? uuid : "") + QLatin1Char('|') + QString::fromUtf8(name ? name : "");
    g_free(uuid);
    g_free(name);
    return id;
}

// Answers GIO's questions with Qt dialogs. Lives as long as the mount call.
struct MountCall {
    GMountOperation* op = nullptr;
    QPointer<QWidget> parent;
    Network::Done done;
    std::function<void(const QString&)> doneUnmount;
    GFile* file = nullptr;
    bool eject = false;
    QString defaultUser;
    QPointer<QDialog> dialog;  // the open question: GVfs re-asks when one takes long

    ~MountCall()
    {
        if (dialog)  // the mount ended (failed, timed out) while still asking
            dialog->reject();
        if (op)
            g_object_unref(op);
        if (file)
            g_object_unref(file);
    }
};

void onAskPassword(GMountOperation* op, const char* message, const char* defaultUser, const char* defaultDomain,
                   GAskPasswordFlags flags, gpointer data)
{
    auto* call = static_cast<MountCall*>(data);
    if (call->dialog) {  // the same question again: the open dialog answers it
        call->dialog->raise();
        return;
    }
    auto* dlg = new QDialog(call->parent);
    call->dialog = dlg;
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(QObject::tr("Connect"));
    auto* layout = new QVBoxLayout(dlg);
    auto* text = new QLabel(QString::fromUtf8(message), dlg);
    text->setWordWrap(true);
    layout->addWidget(text);
    auto* form = new QFormLayout;
    QLineEdit* user = nullptr;
    QLineEdit* domain = nullptr;
    QLineEdit* password = nullptr;
    if (flags & G_ASK_PASSWORD_NEED_USERNAME) {
        user = new QLineEdit(QString::fromUtf8(defaultUser ? defaultUser : ""), dlg);
        if (user->text().isEmpty())
            user->setText(call->defaultUser);
        form->addRow(QObject::tr("User"), user);
    }
    if (flags & G_ASK_PASSWORD_NEED_DOMAIN) {
        domain = new QLineEdit(QString::fromUtf8(defaultDomain ? defaultDomain : ""), dlg);
        form->addRow(QObject::tr("Domain"), domain);
    }
    if (flags & G_ASK_PASSWORD_NEED_PASSWORD) {
        password = new QLineEdit(dlg);
        password->setEchoMode(QLineEdit::Password);
        form->addRow(QObject::tr("Password"), password);
    }
    layout->addLayout(form);
    QCheckBox* save = nullptr;
    if (flags & G_ASK_PASSWORD_SAVING_SUPPORTED) {
        save = new QCheckBox(QObject::tr("Remember the password (keyring)"), dlg);
        layout->addWidget(save);
    }
    QCheckBox* anonymous = nullptr;
    if (flags & G_ASK_PASSWORD_ANONYMOUS_SUPPORTED) {
        anonymous = new QCheckBox(QObject::tr("Connect anonymously"), dlg);
        layout->addWidget(anonymous);
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(QObject::tr("Connect"));
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    if (user && user->text().isEmpty())
        user->setFocus();
    else if (password)
        password->setFocus();

    g_object_ref(op);
    QObject::connect(dlg, &QDialog::finished, dlg, [=](int result) {
        if (result != QDialog::Accepted) {
            g_mount_operation_reply(op, G_MOUNT_OPERATION_ABORTED);
        } else {
            if (anonymous && anonymous->isChecked()) {
                g_mount_operation_set_anonymous(op, TRUE);
            } else {
                if (user)
                    g_mount_operation_set_username(op, user->text().toUtf8().constData());
                if (domain)
                    g_mount_operation_set_domain(op, domain->text().toUtf8().constData());
                if (password)
                    g_mount_operation_set_password(op, password->text().toUtf8().constData());
            }
            g_mount_operation_set_password_save(op, save && save->isChecked() ? G_PASSWORD_SAVE_PERMANENTLY
                                                                               : G_PASSWORD_SAVE_NEVER);
            g_mount_operation_reply(op, G_MOUNT_OPERATION_HANDLED);
        }
        g_object_unref(op);
    });
    dlg->open();
}

void onAskQuestion(GMountOperation* op, const char* message, const char** choices, gpointer data)
{
    auto* call = static_cast<MountCall*>(data);
    auto* box = new QMessageBox(call->parent);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setIcon(QMessageBox::Question);
    box->setWindowTitle(QObject::tr("Connect"));
    // GIO sends "title\nbody".
    const QString msg = QString::fromUtf8(message);
    box->setText(msg.section(QLatin1Char('\n'), 0, 0));
    box->setInformativeText(msg.section(QLatin1Char('\n'), 1));
    QList<QAbstractButton*> buttons;
    for (int i = 0; choices && choices[i]; ++i)
        buttons << box->addButton(QString::fromUtf8(choices[i]), QMessageBox::AcceptRole);
    g_object_ref(op);
    QObject::connect(box, &QMessageBox::finished, box, [=] {
        const int choice = int(buttons.indexOf(box->clickedButton()));
        if (choice < 0) {
            g_mount_operation_reply(op, G_MOUNT_OPERATION_ABORTED);
        } else {
            g_mount_operation_set_choice(op, choice);
            g_mount_operation_reply(op, G_MOUNT_OPERATION_HANDLED);
        }
        g_object_unref(op);
    });
    box->open();
}

GMountOperation* makeOperation(MountCall* call)
{
    call->op = g_mount_operation_new();
    g_signal_connect(call->op, "ask-password", G_CALLBACK(onAskPassword), call);
    g_signal_connect(call->op, "ask-question", G_CALLBACK(onAskQuestion), call);
    return call->op;
}

void onMounted(GObject* source, GAsyncResult* res, gpointer data)
{
    std::unique_ptr<MountCall> call(static_cast<MountCall*>(data));
    GError* err = nullptr;
    if (!g_file_mount_enclosing_volume_finish(G_FILE(source), res, &err)) {
        const bool already = g_error_matches(err, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED);
        const bool aborted = g_error_matches(err, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED);
        const QString msg = takeError(err);
        if (!already) {
            call->done(QString(), aborted ? QObject::tr("Cancelled") : msg);
            return;
        }
    }
    char* p = g_file_get_path(G_FILE(source));
    const QString path = p ? QString::fromLocal8Bit(p) : QString();
    g_free(p);
    if (path.isEmpty()) {
        call->done(QString(), QObject::tr("Mounted, but GVfs has no local path for it (is gvfsd-fuse running?)"));
        return;
    }
    emit Network::instance().placesChanged();
    call->done(path, QString());
}

void onVolumeMounted(GObject* source, GAsyncResult* res, gpointer data)
{
    std::unique_ptr<MountCall> call(static_cast<MountCall*>(data));
    GError* err = nullptr;
    GVolume* volume = G_VOLUME(source);
    if (!g_volume_mount_finish(volume, res, &err)) {
        call->done(QString(), takeError(err));
        return;
    }
    QString path;
    if (GMount* m = g_volume_get_mount(volume)) {
        path = pathOfMount(m);
        g_object_unref(m);
    }
    emit Network::instance().placesChanged();
    call->done(path, path.isEmpty() ? QObject::tr("Mounted, but it has no local path") : QString());
}

void onUnmounted(GObject* source, GAsyncResult* res, gpointer data)
{
    std::unique_ptr<MountCall> call(static_cast<MountCall*>(data));
    GError* err = nullptr;
    GMount* mount = G_MOUNT(source);
    const bool ok = call->eject ? g_mount_eject_with_operation_finish(mount, res, &err)
                            : g_mount_unmount_with_operation_finish(mount, res, &err);
    emit Network::instance().placesChanged();
    call->doneUnmount(ok ? QString() : takeError(err));
}

GMount* mountContaining(const QString& path)
{
    GVolumeMonitor* monitor = g_volume_monitor_get();
    GList* mounts = g_volume_monitor_get_mounts(monitor);
    GMount* best = nullptr;
    int bestLen = 0;
    for (GList* l = mounts; l; l = l->next) {
        GMount* m = G_MOUNT(l->data);
        const QString root = pathOfMount(m);
        if (root.isEmpty() || root == QLatin1String("/"))
            continue;
        if ((path == root || path.startsWith(root + QLatin1Char('/'))) && root.size() > bestLen) {
            best = m;
            bestLen = int(root.size());
        }
    }
    if (best)
        g_object_ref(best);
    g_list_free_full(mounts, g_object_unref);
    g_object_unref(monitor);
    return best;
}

}  // namespace

Network::Network()
{
    GVolumeMonitor* monitor = g_volume_monitor_get();  // kept for the program's life
    auto changed = +[](GVolumeMonitor*, gpointer, gpointer) { emit Network::instance().placesChanged(); };
    for (const char* sig : {"mount-added", "mount-removed", "volume-added", "volume-removed"})
        g_signal_connect(monitor, sig, G_CALLBACK(changed), nullptr);
}

void Network::mount(const QString& uri, QWidget* parent, Done done)
{
    auto* call = new MountCall;
    call->parent = parent;
    call->done = std::move(done);
    call->defaultUser = QString::fromLocal8Bit(qgetenv("USER"));
    call->file = g_file_new_for_uri(uri.toUtf8().constData());
    g_file_mount_enclosing_volume(call->file, G_MOUNT_MOUNT_NONE, makeOperation(call), nullptr, onMounted, call);
}

void Network::mountVolume(const QString& volumeId, QWidget* parent, Done done)
{
    GVolumeMonitor* monitor = g_volume_monitor_get();
    GList* volumes = g_volume_monitor_get_volumes(monitor);
    GVolume* found = nullptr;
    for (GList* l = volumes; l; l = l->next)
        if (volumeIdOf(G_VOLUME(l->data)) == volumeId)
            found = G_VOLUME(g_object_ref(l->data));
    g_list_free_full(volumes, g_object_unref);
    g_object_unref(monitor);
    if (!found) {
        done(QString(), tr("The drive is gone"));
        return;
    }
    auto* call = new MountCall;
    call->parent = parent;
    call->done = std::move(done);
    g_volume_mount(found, G_MOUNT_MOUNT_NONE, makeOperation(call), nullptr, onVolumeMounted, call);
    g_object_unref(found);
}

bool Network::canUnmount(const QString& path) const
{
    GMount* m = mountContaining(path);
    const bool can = m && (g_mount_can_unmount(m) || g_mount_can_eject(m));
    if (m)
        g_object_unref(m);
    return can;
}

void Network::unmount(const QString& path, QWidget* parent, std::function<void(const QString&)> done)
{
    GMount* m = mountContaining(path);
    if (!m) {
        done(tr("Not a mount Tandem can disconnect"));
        return;
    }
    auto* call = new MountCall;
    call->parent = parent;
    call->doneUnmount = std::move(done);
    // Removable drives are ejected (safe to unplug), network mounts unmounted.
    call->eject = g_mount_can_eject(m);
    if (call->eject)
        g_mount_eject_with_operation(m, G_MOUNT_UNMOUNT_NONE, makeOperation(call), nullptr, onUnmounted, call);
    else
        g_mount_unmount_with_operation(m, G_MOUNT_UNMOUNT_NONE, makeOperation(call), nullptr, onUnmounted, call);
    g_object_unref(m);
}

QList<Place> Network::places() const
{
    QList<Place> out;
    GVolumeMonitor* monitor = g_volume_monitor_get();
    const QString gvfs = gvfsDirectory() + QLatin1Char('/');
    GList* mounts = g_volume_monitor_get_mounts(monitor);
    for (GList* l = mounts; l; l = l->next) {
        GMount* m = G_MOUNT(l->data);
        if (g_mount_is_shadowed(m))
            continue;
        const QString path = pathOfMount(m);
        if (!path.startsWith(gvfs))  // kernel mounts come from listMounts()
            continue;
        Place p;
        char* name = g_mount_get_name(m);
        p.name = QString::fromUtf8(name);
        g_free(name);
        p.path = path;
        p.iconName = iconNameOf(g_mount_get_symbolic_icon(m));
        p.network = true;
        out << p;
    }
    g_list_free_full(mounts, g_object_unref);
    GList* volumes = g_volume_monitor_get_volumes(monitor);
    for (GList* l = volumes; l; l = l->next) {
        GVolume* v = G_VOLUME(l->data);
        if (GMount* m = g_volume_get_mount(v)) {  // already mounted: listed elsewhere
            g_object_unref(m);
            continue;
        }
        if (!g_volume_can_mount(v))
            continue;
        Place p;
        char* name = g_volume_get_name(v);
        p.name = QString::fromUtf8(name);
        g_free(name);
        p.volumeId = volumeIdOf(v);
        p.iconName = iconNameOf(g_volume_get_symbolic_icon(v));
        out << p;
    }
    g_list_free_full(volumes, g_object_unref);
    g_object_unref(monitor);
    return out;
}

#else  // no GIO: network places come only from kernel/FUSE mounts

Network::Network() = default;
void Network::mount(const QString&, QWidget*, Done done) { done(QString(), tr("Tandem was built without GIO")); }
void Network::mountVolume(const QString&, QWidget*, Done done) { done(QString(), tr("Tandem was built without GIO")); }
void Network::unmount(const QString&, QWidget*, std::function<void(const QString&)> done) { done(tr("Tandem was built without GIO")); }
bool Network::canUnmount(const QString&) const { return false; }
QList<Place> Network::places() const { return {}; }

#endif

}  // namespace tandem
