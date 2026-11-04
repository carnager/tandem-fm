#include "ui/ServersDialog.h"

#include "core/Backend.h"
#include "core/Keyring.h"
#include "core/Util.h"
#include "ui/IconCache.h"
#include "ui/Theme.h"

#include <QButtonGroup>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace tandem {

namespace {

QIcon iconFor(const SavedServer& s)
{
    return s.type == QLatin1String("smb") ? IconCache::themed("folder-remote", "network-server")
                                           : IconCache::themed("network-server", "folder-remote");
}

QString describe(const SavedServer& s)
{
    QString text = s.authority();
    if (!s.keyFile.isEmpty())
        text += QStringLiteral("  ·  ") + s.keyFile;
    return text;
}

// The password goes to the keyring the way the backends look for it, and to
// the backend for this session. Blocking (D-Bus): on a worker.
void storePassword(const SavedServer& s, const QString& password)
{
    const QString user = s.user.isEmpty() ? QString::fromLocal8Bit(qgetenv("USER")) : s.user;
    if (s.type == QLatin1String("smb")) {
        storeLogin(QStringLiteral("smb"), s.host, {user, s.domain, password});
    } else {
        storeLogin(s.type, s.host, {user, QString(), password}, s.port == 22 ? 0 : s.port);
    }
    if (Backend* b = backendFor(s.authority() + QLatin1Char('/')))
        b->offerPassword(s.authority() + QLatin1Char('/'), s.domain.isEmpty() ? user : s.domain + QLatin1Char('\\') + user,
                         password);
}

}  // namespace

ServersDialog::ServersDialog(QWidget* parent, const QString& uri) : QDialog(parent)
{
    setWindowTitle(tr("Servers"));
    resize(760, 460);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(18, 16, 18, 14);
    outer->setSpacing(10);

    m_error = new QLabel(this);
    m_error->setWordWrap(true);
    setThemeStyle(m_error, QStringLiteral("color: @error;"));
    m_error->hide();
    outer->addWidget(m_error);

    auto* columns = new QHBoxLayout;
    columns->setSpacing(16);
    outer->addLayout(columns, 1);

    // Left: the servers.
    auto* left = new QVBoxLayout;
    m_list = new QListWidget(this);
    m_list->setMinimumWidth(220);
    left->addWidget(m_list, 1);
    auto* listButtons = new QHBoxLayout;
    auto* add = new QPushButton(tr("&Add"), this);
    m_remove = new QPushButton(tr("&Remove"), this);
    listButtons->addWidget(add);
    listButtons->addWidget(m_remove);
    listButtons->addStretch(1);
    left->addLayout(listButtons);
    auto* import = new QPushButton(tr("&Import from SSH config…"), this);
    left->addWidget(import);
    columns->addLayout(left, 2);

    // Right: the selected one.
    m_form = new QWidget(this);
    m_fields = new QFormLayout(m_form);
    m_fields->setContentsMargins(0, 0, 0, 0);
    m_fields->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    setThemeStyle(m_form, QStringLiteral(
        "QToolButton#segment { padding: 5px 14px; border: 1px solid @control; background: @base; }"
        "QToolButton#segment:checked { background: @accent; color: @accentText; border-color: @accent; }"));

    m_name = new QLineEdit(m_form);
    m_fields->addRow(tr("Name"), m_name);

    auto* types = new QWidget(m_form);
    auto* typesLayout = new QHBoxLayout(types);
    typesLayout->setContentsMargins(0, 0, 0, 0);
    typesLayout->setSpacing(0);
    m_type = new QButtonGroup(this);
    for (const auto& [id, label] : {std::pair{0, tr("SSH (sftp)")}, std::pair{1, tr("SMB (Windows share)")}}) {
        auto* b = new QToolButton(types);
        b->setObjectName(QStringLiteral("segment"));
        b->setText(label);
        b->setCheckable(true);
        m_type->addButton(b, id);
        typesLayout->addWidget(b);
    }
    typesLayout->addStretch(1);
    m_fields->addRow(tr("Type"), types);

    m_host = new QLineEdit(m_form);
    m_host->setPlaceholderText(tr("nas.lan or 192.168.1.10"));
    m_fields->addRow(tr("Host"), m_host);
    m_port = new QSpinBox(m_form);
    m_port->setRange(0, 65535);
    m_port->setSpecialValueText(tr("default"));
    m_port->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_port->setMinimumWidth(m_port->fontMetrics().horizontalAdvance(QStringLiteral("65535")) * 3);
    m_fields->addRow(tr("Port"), m_port);
    m_user = new QLineEdit(m_form);
    m_user->setPlaceholderText(QString::fromLocal8Bit(qgetenv("USER")));
    m_fields->addRow(tr("User"), m_user);
    m_domain = new QLineEdit(m_form);
    m_domain->setPlaceholderText(tr("usually empty"));
    m_fields->addRow(tr("Domain"), m_domain);
    m_password = new QLineEdit(m_form);
    m_password->setEchoMode(QLineEdit::Password);
    if (keyringAvailable()) {
        m_password->setPlaceholderText(tr("empty: keep the saved one, or ask"));
    } else {
        m_password->setEnabled(false);
        m_password->setPlaceholderText(tr("no keyring: asked when connecting"));
    }
    m_fields->addRow(tr("Password"), m_password);

    m_keyRow = new QWidget(m_form);
    auto* keyLayout = new QHBoxLayout(m_keyRow);
    keyLayout->setContentsMargins(0, 0, 0, 0);
    m_keyFile = new QLineEdit(m_keyRow);
    m_keyFile->setPlaceholderText(tr("empty: the agent and the usual keys"));
    auto* browse = new QToolButton(m_keyRow);
    browse->setText(QStringLiteral("…"));
    browse->setToolTip(tr("Choose a key file"));
    keyLayout->addWidget(m_keyFile, 1);
    keyLayout->addWidget(browse);
    m_fields->addRow(tr("Key file"), m_keyRow);

    m_folder = new QLineEdit(m_form);
    m_folder->setPlaceholderText(tr("empty: the home folder"));
    m_fields->addRow(tr("Start in"), m_folder);

    m_address = new QLabel(m_form);
    m_address->setTextInteractionFlags(Qt::TextSelectableByMouse);
    setThemeStyle(m_address, QStringLiteral("color: @dimText;"));
    m_fields->addRow(QString(), m_address);
    columns->addWidget(m_form, 3);

    auto* buttons = new QDialogButtonBox(this);
    m_connect = buttons->addButton(tr("Save and &connect"), QDialogButtonBox::ActionRole);
    m_save = buttons->addButton(QDialogButtonBox::Save);
    buttons->addButton(QDialogButtonBox::Cancel);
    m_save->setDefault(true);
    outer->addWidget(buttons);

    connect(m_list, &QListWidget::currentRowChanged, this, &ServersDialog::showServer);
    connect(m_list, &QListWidget::itemDoubleClicked, this, &ServersDialog::connectCurrent);
    connect(add, &QPushButton::clicked, this, [this] {
        addServer(SavedServer());
        m_host->setFocus();
    });
    connect(m_remove, &QPushButton::clicked, this, &ServersDialog::removeCurrent);
    connect(import, &QPushButton::clicked, this, &ServersDialog::importFromSsh);
    connect(browse, &QToolButton::clicked, this, [this] {
        const QString start = m_keyFile->text().isEmpty() ? QDir::homePath() + QStringLiteral("/.ssh")
                                                          : expandUserPath(m_keyFile->text());
        QString file = QFileDialog::getOpenFileName(this, tr("Key file"), start);
        if (file.isEmpty())
            return;
        if (file.startsWith(QDir::homePath() + QLatin1Char('/')))
            file = QLatin1Char('~') + file.mid(QDir::homePath().size());
        m_keyFile->setText(file);
    });
    m_name->setObjectName(QStringLiteral("name"));
    m_host->setObjectName(QStringLiteral("host"));
    m_user->setObjectName(QStringLiteral("user"));
    m_domain->setObjectName(QStringLiteral("domain"));
    m_keyFile->setObjectName(QStringLiteral("keyFile"));
    m_password->setObjectName(QStringLiteral("password"));
    m_folder->setObjectName(QStringLiteral("folder"));
    for (QLineEdit* e : {m_name, m_host, m_user, m_domain, m_keyFile, m_password, m_folder})
        connect(e, &QLineEdit::textChanged, this, &ServersDialog::formChanged);
    connect(m_port, &QSpinBox::valueChanged, this, &ServersDialog::formChanged);
    connect(m_type, &QButtonGroup::idToggled, this, [this](int, bool on) {
        if (on)
            formChanged();
    });
    connect(m_save, &QPushButton::clicked, this, [this] {
        if (save())
            accept();
    });
    connect(m_connect, &QPushButton::clicked, this, &ServersDialog::connectCurrent);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    ServerList& saved = ServerList::instance();
    if (!saved.error().isEmpty()) {
        // Saving here would replace what the hand edit says.
        m_error->setText(tr("%1. Fix it in an editor; Tandem won't overwrite it.").arg(saved.error()));
        m_error->show();
        m_save->setEnabled(false);
        m_connect->setEnabled(false);
    }
    for (const SavedServer& s : saved.servers())
        addServer(s);
    const auto known = uri.isEmpty() ? std::nullopt : saved.find(uri);
    if (known) {
        m_list->setCurrentRow(int(m_servers.indexOf(*known)));
    } else if (!uri.isEmpty()) {
        SavedServer s = serverFromUri(uri);
        if (!s.host.isEmpty())
            addServer(s);
    } else if (m_list->count() > 0) {
        m_list->setCurrentRow(0);
    }
    showServer(m_list->currentRow());
}

void ServersDialog::addServer(const SavedServer& s)
{
    m_servers.append(s);
    m_passwords.append(QString());
    auto* item = new QListWidgetItem(m_list);
    (void)item;
    updateRow(int(m_servers.size()) - 1);
    m_list->setCurrentRow(int(m_servers.size()) - 1);
}

void ServersDialog::removeCurrent()
{
    const int row = m_list->currentRow();
    if (row < 0)
        return;
    m_servers.removeAt(row);
    m_passwords.removeAt(row);
    delete m_list->takeItem(row);
    showServer(m_list->currentRow());
}

void ServersDialog::updateRow(int row)
{
    QListWidgetItem* item = m_list->item(row);
    if (!item || row >= m_servers.size())
        return;
    const SavedServer& s = m_servers[row];
    const QString name = s.name.isEmpty() ? (s.host.isEmpty() ? tr("New server") : s.host) : s.name;
    item->setText(name);
    item->setIcon(iconFor(s));
    item->setToolTip(s.host.isEmpty() ? QString() : describe(s));
}

void ServersDialog::showServer(int row)
{
    const bool has = row >= 0 && row < m_servers.size();
    m_form->setEnabled(has);
    m_remove->setEnabled(has);
    m_connect->setEnabled(has && m_save->isEnabled());
    const SavedServer s = has ? m_servers[row] : SavedServer();
    m_loading = true;
    m_name->setText(s.name);
    m_type->button(s.type == QLatin1String("smb") ? 1 : 0)->setChecked(true);
    m_host->setText(s.host);
    m_port->setValue(s.port);
    m_user->setText(s.user);
    m_domain->setText(s.domain);
    m_keyFile->setText(s.keyFile);
    m_password->setText(has ? m_passwords[row] : QString());
    m_folder->setText(s.folder);
    m_loading = false;
    formChanged();
}

void ServersDialog::formChanged()
{
    const bool smb = m_type->checkedId() == 1;
    m_fields->setRowVisible(m_domain, smb);
    m_fields->setRowVisible(m_keyRow, !smb);
    m_folder->setPlaceholderText(smb ? tr("share/folder; empty: the list of shares") : tr("empty: the home folder"));
    const int row = m_list->currentRow();
    if (m_loading || row < 0 || row >= m_servers.size()) {
        m_address->setText(row >= 0 && row < m_servers.size() ? m_servers[row].uri() : QString());
        return;
    }
    SavedServer& s = m_servers[row];
    s.name = m_name->text().trimmed();
    // Keep a type's other fields (a key file for SSH…) while switching back and forth.
    const QString type = smb ? QStringLiteral("smb") : QStringLiteral("sftp");
    if (s.type == QLatin1String("smb") || s.type == QLatin1String("sftp") || s.type.isEmpty())
        s.type = type;
    s.host = m_host->text().trimmed();
    s.port = m_port->value();
    s.user = m_user->text().trimmed();
    s.domain = smb ? m_domain->text().trimmed() : QString();
    s.keyFile = smb ? QString() : m_keyFile->text().trimmed();
    s.folder = m_folder->text().trimmed();
    m_passwords[row] = m_password->text();
    m_address->setText(s.host.isEmpty() ? QString() : s.uri());
    updateRow(row);
}

bool ServersDialog::save()
{
    QList<SavedServer> list;
    for (int i = 0; i < m_servers.size(); ++i) {
        SavedServer s = m_servers[i];
        if (s.host.isEmpty()) {
            m_list->setCurrentRow(i);
            m_error->setText(tr("A server needs a host."));
            m_error->show();
            m_host->setFocus();
            return false;
        }
        if (s.name.isEmpty())
            s.name = s.host;
        list.append(s);
    }
    if (!ServerList::instance().save(list)) {
        m_error->setText(tr("Cannot write %1").arg(ServerList::instance().path()));
        m_error->show();
        return false;
    }
    QList<std::pair<SavedServer, QString>> passwords;
    for (int i = 0; i < list.size(); ++i)
        if (!m_passwords[i].isEmpty())
            passwords.append({list[i], m_passwords[i]});
    if (!passwords.isEmpty())
        ioPool()->start([passwords] {
            for (const auto& [s, password] : passwords)
                storePassword(s, password);
        });
    return true;
}

void ServersDialog::connectCurrent()
{
    const int row = m_list->currentRow();
    if (row < 0 || !m_save->isEnabled() || !save())
        return;
    const QString uri = ServerList::instance().servers().value(row).uri();
    accept();
    if (onConnect && !uri.isEmpty())
        onConnect(uri);
}

void ServersDialog::importFromSsh()
{
    const QList<SavedServer> hosts = readSshConfig(sshConfigPath);
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Import from SSH config"));
    dlg.resize(520, 420);
    auto* layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(18, 16, 18, 14);
    auto* intro = new QLabel(hosts.isEmpty() ? tr("No hosts found in %1.").arg(tildePath(sshConfigPath.isEmpty()
                                                                                     ? QDir::homePath() + QStringLiteral("/.ssh/config")
                                                                                     : sshConfigPath))
                                             : tr("Pick the hosts to copy. Later changes to the SSH config don't change them."),
                             &dlg);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto* picks = new QListWidget(&dlg);
    for (const SavedServer& h : hosts) {
        bool known = false;
        for (const SavedServer& s : std::as_const(m_servers))
            known |= s.name == h.name || s.authority() == h.authority();
        auto* item = new QListWidgetItem(iconFor(h), QStringLiteral("%1   %2").arg(h.name, describe(h)), picks);
        item->setFlags(known ? Qt::NoItemFlags : Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
        if (known)
            item->setToolTip(tr("Already saved"));
    }
    layout->addWidget(picks, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QPushButton* ok = buttons->button(QDialogButtonBox::Ok);
    auto updateOk = [picks, ok] {
        int n = 0;
        for (int i = 0; i < picks->count(); ++i)
            n += picks->item(i)->checkState() == Qt::Checked;
        ok->setEnabled(n > 0);
        ok->setText(n > 0 ? plural(tr("Import %n host(s)", nullptr, n), n) : tr("Import"));
    };
    connect(picks, &QListWidget::itemChanged, &dlg, updateOk);
    connect(picks, &QListWidget::itemActivated, &dlg, [](QListWidgetItem* item) {
        if (item->flags() & Qt::ItemIsUserCheckable)
            item->setCheckState(item->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
    });
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);
    updateOk();
    picks->setFocus();
    if (dlg.exec() != QDialog::Accepted)
        return;
    for (int i = 0; i < picks->count(); ++i)
        if (picks->item(i)->checkState() == Qt::Checked)
            addServer(hosts[i]);
}

}  // namespace tandem
