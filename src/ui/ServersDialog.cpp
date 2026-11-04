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
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QStyledItemDelegate>
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

// "SSH · bob@nas.lan": the kind and the login, under the name.
QString subtitleFor(const SavedServer& s)
{
    const QString kind = s.type == QLatin1String("smb") ? QStringLiteral("SMB") : QStringLiteral("SSH");
    if (s.host.isEmpty())
        return kind;
    QString who = s.user.isEmpty() ? s.host : s.user + QLatin1Char('@') + s.host;
    if (s.port > 0)
        who += QLatin1Char(':') + QString::number(s.port);
    return kind + QStringLiteral("  ·  ") + who;
}

// Two lines per server, selected like the settings' sidebar.
class ServerDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex&) const override
    {
        return {opt.rect.width(), opt.fontMetrics.height() * 2 + 16};
    }
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override
    {
        const Theme& t = theme();
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRectF card = QRectF(opt.rect).adjusted(0, 1, 0, -1);
        if (opt.state & QStyle::State_Selected) {
            p->setPen(Qt::NoPen);
            p->setBrush(t.headerActive);
            p->drawRoundedRect(card, 6, 6);
        } else if (opt.state & QStyle::State_MouseOver) {
            p->setPen(Qt::NoPen);
            p->setBrush(t.altBase);
            p->drawRoundedRect(card, 6, 6);
        }
        QRect r = opt.rect.adjusted(10, 0, -10, 0);
        const int is = 22;
        idx.data(Qt::DecorationRole).value<QIcon>().paint(p, QRect(r.left(), r.center().y() - is / 2, is, is));
        r.setLeft(r.left() + is + 10);
        const int line = opt.fontMetrics.height();
        const int top = opt.rect.center().y() - line;
        p->setFont(opt.font);
        p->setPen(t.text);
        p->drawText(QRect(r.left(), top, r.width(), line), Qt::AlignLeft | Qt::AlignVCenter,
                    opt.fontMetrics.elidedText(idx.data(Qt::DisplayRole).toString(), Qt::ElideRight, r.width()));
        QFont small = opt.font;
        small.setPointSizeF(small.pointSizeF() * 0.9);
        p->setFont(small);
        p->setPen(t.dimText);
        p->drawText(QRect(r.left(), top + line + 1, r.width(), line), Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(small).elidedText(idx.data(Qt::UserRole).toString(), Qt::ElideMiddle, r.width()));
        p->restore();
    }
};

// A small heading inside the form, like the location popup's sections.
QLabel* sectionLabel(const QString& text, QWidget* parent)
{
    auto* l = new QLabel(text.toUpper(), parent);
    QFont f = l->font();
    f.setPointSizeF(f.pointSizeF() * 0.85);
    f.setBold(true);
    f.setLetterSpacing(QFont::PercentageSpacing, 108);
    l->setFont(f);
    setThemeStyle(l, QStringLiteral("color: @dimText; padding-top: 6px;"));
    return l;
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
    resize(820, 520);
    setThemeStyle(this, QStringLiteral(
        "QListWidget#servers { background: transparent; border: none; outline: none; }"
        "QPushButton#flat { background: transparent; border: none; color: @dimText; padding: 5px 8px;"
        "  border-radius: 6px; text-align: left; }"
        "QPushButton#flat:hover { background: @altBase; color: @text; }"
        "QPushButton#flat:disabled { color: @border; }"
        "QToolButton#segment { padding: 5px 16px; border: 1px solid @control; background: @base; }"
        "QToolButton#segment:checked { background: @accent; color: @accentText; border-color: @accent; }"
        "QToolButton#segment[edge=\"first\"] { border-top-left-radius: 6px; border-bottom-left-radius: 6px; }"
        "QToolButton#segment[edge=\"last\"] { border-top-right-radius: 6px; border-bottom-right-radius: 6px;"
        "  border-left: none; }"
        "QFrame#divider { background: @border; }"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 16, 16, 12);
    outer->setSpacing(12);

    m_error = new QLabel(this);
    m_error->setWordWrap(true);
    setThemeStyle(m_error, QStringLiteral("color: @error;"));
    m_error->hide();
    outer->addWidget(m_error);

    auto* columns = new QHBoxLayout;
    columns->setSpacing(18);
    outer->addLayout(columns, 1);

    // Left: the servers, as a sidebar.
    auto* left = new QVBoxLayout;
    left->setSpacing(4);
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("servers"));
    m_list->setItemDelegate(new ServerDelegate(m_list));
    m_list->setMouseTracking(true);
    m_list->setFixedWidth(250);
    left->addWidget(m_list, 1);
    auto* add = new QPushButton(IconCache::themed("list-add", "document-new"), tr("&Add"), this);
    m_remove = new QPushButton(IconCache::themed("list-remove", "edit-delete"), tr("&Remove"), this);
    auto* import = new QPushButton(IconCache::themed("document-import", "document-open"), tr("&Import from SSH config…"), this);
    for (QPushButton* b : {add, m_remove, import}) {
        b->setObjectName(QStringLiteral("flat"));
        b->setCursor(Qt::PointingHandCursor);
        left->addWidget(b);
    }
    columns->addLayout(left);

    auto* divider = new QFrame(this);
    divider->setObjectName(QStringLiteral("divider"));
    divider->setFixedWidth(1);
    columns->addWidget(divider);

    // Right: the selected one, under a heading with its address.
    auto* right = new QVBoxLayout;
    right->setSpacing(10);
    m_heading = new QLabel(this);
    QFont hf = m_heading->font();
    hf.setPointSizeF(hf.pointSizeF() * 1.3);
    hf.setWeight(QFont::DemiBold);
    m_heading->setFont(hf);
    m_address = new QLabel(this);
    m_address->setTextInteractionFlags(Qt::TextSelectableByMouse);
    setThemeStyle(m_address, QStringLiteral("color: @dimText;"));
    right->addWidget(m_heading);
    right->addWidget(m_address);
    m_empty = new QLabel(tr("No servers yet. Add one, or import hosts from your SSH config."), this);
    m_empty->setWordWrap(true);
    m_empty->setAlignment(Qt::AlignCenter);
    setThemeStyle(m_empty, QStringLiteral("color: @dimText;"));
    right->addWidget(m_empty, 100);  // nearly all the height: centred

    m_form = new QWidget(this);
    m_fields = new QFormLayout(m_form);
    m_fields->setContentsMargins(0, 0, 0, 0);
    m_fields->setHorizontalSpacing(14);
    m_fields->setVerticalSpacing(8);
    m_fields->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_fields->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_fields->addRow(sectionLabel(tr("Connection"), m_form));
    m_name = new QLineEdit(m_form);
    m_name->setPlaceholderText(tr("shown in Locations; empty: the host"));
    m_fields->addRow(tr("Name"), m_name);

    auto* types = new QWidget(m_form);
    auto* typesLayout = new QHBoxLayout(types);
    typesLayout->setContentsMargins(0, 0, 0, 0);
    typesLayout->setSpacing(0);
    m_type = new QButtonGroup(this);
    for (const auto& [id, label] : {std::pair{0, tr("SSH (sftp)")}, std::pair{1, tr("SMB (Windows share)")}}) {
        auto* b = new QToolButton(types);
        b->setObjectName(QStringLiteral("segment"));
        b->setProperty("edge", id == 0 ? QStringLiteral("first") : QStringLiteral("last"));
        b->setText(label);
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        m_type->addButton(b, id);
        typesLayout->addWidget(b);
    }
    typesLayout->addStretch(1);
    m_fields->addRow(tr("Type"), types);

    // Host and port on one line, the port only as wide as a port.
    auto* hostRow = new QWidget(m_form);
    auto* hostLayout = new QHBoxLayout(hostRow);
    hostLayout->setContentsMargins(0, 0, 0, 0);
    hostLayout->setSpacing(8);
    m_host = new QLineEdit(hostRow);
    m_host->setPlaceholderText(tr("nas.lan or 192.168.1.10"));
    m_port = new QLineEdit(hostRow);
    m_port->setObjectName(QStringLiteral("port"));
    m_port->setValidator(new QIntValidator(1, 65535, m_port));
    m_port->setFixedWidth(m_port->fontMetrics().horizontalAdvance(QStringLiteral("65535")) + 28);
    m_port->setToolTip(tr("Empty: the usual port"));
    hostLayout->addWidget(m_host, 1);
    auto* portLabel = new QLabel(tr("Port"), hostRow);
    hostLayout->addWidget(portLabel);
    hostLayout->addWidget(m_port);
    m_fields->addRow(tr("Host"), hostRow);

    m_folder = new QLineEdit(m_form);
    m_folder->setPlaceholderText(tr("empty: the home folder"));
    m_fields->addRow(tr("Start in"), m_folder);

    m_fields->addRow(sectionLabel(tr("Login"), m_form));
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
    keyLayout->setSpacing(6);
    m_keyFile = new QLineEdit(m_keyRow);
    m_keyFile->setPlaceholderText(tr("empty: the agent and the usual keys"));
    auto* browse = new QPushButton(tr("Choose…"), m_keyRow);
    browse->setToolTip(tr("Choose a key file"));
    keyLayout->addWidget(m_keyFile, 1);
    keyLayout->addWidget(browse);
    m_fields->addRow(tr("Key file"), m_keyRow);
    right->addWidget(m_form);
    right->addStretch(1);
    columns->addLayout(right, 1);

    // Bottom: where passwords go, then the buttons.
    auto* bottom = new QHBoxLayout;
    auto* where = new QLabel(keyringAvailable() ? tr("Passwords are kept in the desktop keyring.")
                                                : tr("No keyring here: passwords are asked when connecting."),
                             this);
    setThemeStyle(where, QStringLiteral("color: @dimText;"));
    bottom->addWidget(where, 1);
    auto* cancel = new QPushButton(tr("Cancel"), this);
    m_save = new QPushButton(tr("&Save"), this);
    m_connect = new QPushButton(tr("Save and &connect"), this);
    m_save->setDefault(true);
    for (QPushButton* b : {cancel, m_save, m_connect})
        bottom->addWidget(b);
    outer->addLayout(bottom);

    connect(m_list, &QListWidget::currentRowChanged, this, &ServersDialog::showServer);
    connect(m_list, &QListWidget::itemDoubleClicked, this, &ServersDialog::connectCurrent);
    connect(add, &QPushButton::clicked, this, [this] {
        addServer(SavedServer());
        m_host->setFocus();
    });
    connect(m_remove, &QPushButton::clicked, this, &ServersDialog::removeCurrent);
    connect(import, &QPushButton::clicked, this, &ServersDialog::importFromSsh);
    connect(browse, &QPushButton::clicked, this, [this] {
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
    connect(m_port, &QLineEdit::textChanged, this, &ServersDialog::formChanged);
    connect(m_type, &QButtonGroup::idToggled, this, [this](int, bool on) {
        if (on)
            formChanged();
    });
    connect(m_save, &QPushButton::clicked, this, [this] {
        if (save())
            accept();
    });
    connect(m_connect, &QPushButton::clicked, this, &ServersDialog::connectCurrent);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);

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
    item->setData(Qt::UserRole, subtitleFor(s));
    item->setIcon(iconFor(s));
    item->setToolTip(s.host.isEmpty() ? QString() : describe(s));
}

void ServersDialog::showServer(int row)
{
    const bool has = row >= 0 && row < m_servers.size();
    m_form->setVisible(has);
    m_heading->setVisible(has);
    m_address->setVisible(has);
    m_empty->setVisible(!has);
    m_remove->setEnabled(has);
    m_connect->setEnabled(has && m_save->isEnabled());
    const SavedServer s = has ? m_servers[row] : SavedServer();
    m_loading = true;
    m_name->setText(s.name);
    m_type->button(s.type == QLatin1String("smb") ? 1 : 0)->setChecked(true);
    m_host->setText(s.host);
    m_port->setText(s.port > 0 ? QString::number(s.port) : QString());
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
    m_port->setPlaceholderText(smb ? QStringLiteral("445") : QStringLiteral("22"));
    const int row = m_list->currentRow();
    if (m_loading || row < 0 || row >= m_servers.size()) {
        m_address->setText(row >= 0 && row < m_servers.size() ? m_servers[row].uri() : QString());
        updateHeading();
        return;
    }
    SavedServer& s = m_servers[row];
    s.name = m_name->text().trimmed();
    // Keep a type's other fields (a key file for SSH…) while switching back and forth.
    const QString type = smb ? QStringLiteral("smb") : QStringLiteral("sftp");
    if (s.type == QLatin1String("smb") || s.type == QLatin1String("sftp") || s.type.isEmpty())
        s.type = type;
    s.host = m_host->text().trimmed();
    s.port = m_port->text().toInt();
    s.user = m_user->text().trimmed();
    s.domain = smb ? m_domain->text().trimmed() : QString();
    s.keyFile = smb ? QString() : m_keyFile->text().trimmed();
    s.folder = m_folder->text().trimmed();
    m_passwords[row] = m_password->text();
    m_address->setText(s.host.isEmpty() ? tr("Type a host to see the address") : s.uri());
    updateRow(row);
    updateHeading();
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

void ServersDialog::updateHeading()
{
    const QListWidgetItem* item = m_list->currentItem();
    m_heading->setText(item ? item->text() : QString());
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
    auto* intro = new QLabel(hosts.isEmpty() ? tr("No hosts found in %1.").arg(sshConfigPath.isEmpty()
                                                                         ? QDir::homePath() + QStringLiteral("/.ssh/config")
                                                                         : sshConfigPath)
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

