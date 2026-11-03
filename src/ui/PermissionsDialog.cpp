#include "ui/PermissionsDialog.h"

#include "core/Util.h"
#include "ui/IconCache.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStringListModel>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <grp.h>
#include <pwd.h>
#include <unistd.h>

namespace tandem {

namespace {

constexpr uint32_t bitOf(int i)
{
    return i < 9 ? (0400u >> i) : i == 9 ? 04000u : i == 10 ? 02000u : 01000u;
}

QString permWords(uint32_t p)
{
    QStringList w;
    if (p & PermRead)
        w << QObject::tr("read");
    if (p & PermWrite)
        w << QObject::tr("write");
    if (p & PermExec)
        w << QObject::tr("run");
    return w.isEmpty() ? QObject::tr("nothing") : w.join(QStringLiteral(", "));
}

QLabel* sectionLabel(const QString& text, QWidget* parent)
{
    auto* l = new QLabel(text.toUpper(), parent);
    QFont f = l->font();
    f.setPointSizeF(f.pointSizeF() * 0.85);
    f.setBold(true);
    f.setLetterSpacing(QFont::PercentageSpacing, 108);
    l->setFont(f);
    setThemeStyle(l, QStringLiteral("color: @dimText; padding-top: 10px;"));
    return l;
}

QLabel* dimLabel(const QString& text, QWidget* parent)
{
    auto* l = new QLabel(text, parent);
    l->setWordWrap(true);
    setThemeStyle(l, QStringLiteral("color: @dimText;"));
    return l;
}

}  // namespace

PermAccounts PermAccounts::local()
{
    PermAccounts a;
    a.users = listUsers();
    a.groups = listGroups();
    a.canChangeOwner = ::geteuid() == 0;
    if (!a.canChangeOwner)
        a.pickableGroups = ownGroups();
    return a;
}

std::optional<uint32_t> PermissionsDialog::idOf(const QString& name, bool group) const
{
    for (const auto& [n, id] : group ? m_accounts.groups : m_accounts.users)
        if (n == name)
            return id;
    return std::nullopt;
}

PermissionsDialog::PermissionsDialog(QStringList paths, QList<PermInfo> infos, bool remote, PermAccounts accounts, QWidget* parent)
    : QDialog(parent), m_paths(std::move(paths)), m_infos(std::move(infos)), m_remote(remote), m_accounts(std::move(accounts))
{
    const int n = int(m_infos.size());
    const PermInfo& first = m_infos.first();
    bool anyDir = false, allDirs = true, aclOk = true, posix = true, sameOwner = true, sameGroup = true;
    uint32_t offered = remote ? 0 : ~0u;
    for (const PermInfo& i : std::as_const(m_infos)) {
        anyDir |= i.isDir;
        allDirs &= i.isDir;
        aclOk &= i.aclSupported;
        posix &= i.posixPerms;
        offered &= i.flagsOffered;
        sameOwner &= i.uid == first.uid;
        sameGroup &= i.gid == first.gid;
    }
    const bool root = m_accounts.canChangeOwner;
    bool mine = true;
    for (const PermInfo& i : std::as_const(m_infos))
        mine &= i.uid == ::geteuid();
    m_editable = posix && (remote || root || mine);  // a server says no itself if it must

    setWindowTitle(n == 1 ? tr("Permissions of %1").arg(baseName(m_paths.first()))
                          : plural(tr("Permissions of %n item(s)", nullptr, n), n));
    resize(620, 0);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 16, 20, 14);
    layout->setSpacing(6);

    // Which items, and where.
    auto* head = new QHBoxLayout;
    FileEntry fe;
    fe.name = baseName(m_paths.first());
    fe.isDir = first.isDir;
    auto* icon = new QLabel(this);
    icon->setPixmap(IconCache::forEntry(fe).pixmap(32, 32));
    head->addWidget(icon);
    auto* names = new QVBoxLayout;
    auto* title = new QLabel(n == 1 ? baseName(m_paths.first()) : plural(tr("%n item(s)", nullptr, n), n), this);
    QFont tf = title->font();
    tf.setPointSizeF(tf.pointSizeF() * 1.15);
    tf.setBold(true);
    title->setFont(tf);
    names->addWidget(title);
    QStringList where;
    where << (n == 1 ? parentPath(m_paths.first()) : tr("in %1").arg(parentPath(m_paths.first())));
    if (!first.fsName.isEmpty())
        where << first.fsName;
    names->addWidget(dimLabel(where.join(QStringLiteral("  ·  ")), this));
    head->addLayout(names, 1);
    layout->addLayout(head);

    if (!posix) {
        layout->addWidget(dimLabel(tr("This is on %1, which has no Linux permissions: who may read and write here is "
                                      "decided by how the drive is mounted, for all files at once.")
                                       .arg(first.fsName),
                                   this));
    } else if (!m_editable) {
        layout->addWidget(dimLabel(tr("Only the owner (%1) or root can change these.").arg(first.owner), this));
    }

    // Who can do what: one row each, the rights as toggles right after the name.
    m_symbolic = new QLabel(this);
    m_symbolic->setFont(monoFont());
    setThemeStyle(m_symbolic, QStringLiteral("color: @dimText;"));
    m_octal = new QLineEdit(this);
    m_octal->setFont(monoFont());
    m_octal->setFixedWidth(64);
    m_octal->setAlignment(Qt::AlignCenter);
    m_octal->setToolTip(tr("As a number, like chmod: 644, 755, 2775 …"));
    m_octal->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-7]{0,4}")), m_octal));
    auto* whoHead = new QHBoxLayout;
    whoHead->addWidget(sectionLabel(tr("Who can do what"), this), 1, Qt::AlignBottom);
    whoHead->addWidget(m_symbolic, 0, Qt::AlignBottom);
    whoHead->addSpacing(8);
    whoHead->addWidget(m_octal, 0, Qt::AlignBottom);
    layout->addLayout(whoHead);
    setThemeStyle(this, QStringLiteral(
        "QCheckBox#perm { border: 1px solid @control; border-radius: 13px; padding: 4px 14px; color: @dimText; }"
        "QCheckBox#perm::indicator { width: 0; height: 0; }"
        "QCheckBox#perm:hover { border-color: @accent; }"
        "QCheckBox#perm:checked { background: @accent; border-color: @accent; color: @accentText; }"
        "QCheckBox#perm:indeterminate { border: 1px dashed @accent; color: @text; }"
        "QCheckBox#perm:disabled { border-color: @border; }"));
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(8);
    grid->setColumnMinimumWidth(0, 110);
    const QString runWord = allDirs ? tr("Open") : (anyDir ? tr("Run / open") : tr("Run"));
    const QStringList chipNames = {tr("Read"), tr("Write"), runWord};
    const QStringList tips = {
        allDirs ? tr("See what's inside") : tr("Open and read it"),
        allDirs ? tr("Add, rename and delete items inside") : tr("Change it"),
        allDirs ? tr("Go into it and reach the items inside") : tr("Run it as a program"),
    };
    // Owner and group are picked right in their rows.
    auto accountBox = [&](bool users, bool same, bool canChange) -> QWidget* {
        const QString cur = same ? (users ? first.owner : first.group) : tr("(mixed)");
        if (!canChange || (users ? m_accounts.users : m_accounts.groups).isEmpty()) {
            auto* l = new QLabel(cur, this);
            l->setContentsMargins(9, 0, 0, 0);  // lines up with the text in a drop-down
            l->setToolTip(users ? tr("Only root can give files to someone else") : QString());
            return l;
        }
        auto* box = new QComboBox(this);
        box->setEditable(true);
        box->setInsertPolicy(QComboBox::NoInsert);
        QStringList names;
        if (users) {
            for (const auto& u : std::as_const(m_accounts.users))
                names << u.first;
        } else {
            for (const auto& g : std::as_const(m_accounts.groups))
                if (m_accounts.pickableGroups.isEmpty() || m_accounts.pickableGroups.contains(g.second))
                    names << g.first;
        }
        if (same && !names.contains(cur))
            names.prepend(cur);
        box->addItems(names);
        box->setCurrentText(same ? cur : QString());
        if (!same)
            box->lineEdit()->setPlaceholderText(tr("(mixed)"));
        box->completer()->setFilterMode(Qt::MatchContains);
        box->completer()->setCompletionMode(QCompleter::PopupCompletion);
        return box;
    };
    const QStringList rowNames = {tr("Owner"), tr("Group"), tr("Everyone else")};
    for (int r = 0; r < 3; ++r) {
        grid->addWidget(new QLabel(rowNames[r], this), r, 0);
        QWidget* account = r == 0 ? accountBox(true, sameOwner, root || (remote && !m_accounts.users.isEmpty()))
                                  : r == 1 ? accountBox(false, sameGroup, root || mine || remote) : new QWidget(this);
        account->setFixedWidth(180);
        if (r == 0)
            m_owner = qobject_cast<QComboBox*>(account);
        if (r == 1)
            m_group = qobject_cast<QComboBox*>(account);
        grid->addWidget(account, r, 1);
        auto* chips = new QHBoxLayout;
        chips->setSpacing(6);
        for (int c = 0; c < 3; ++c) {
            auto* b = new QCheckBox(chipNames[c], this);
            b->setObjectName(QStringLiteral("perm"));
            b->setToolTip(tips[c]);
            b->setCursor(Qt::PointingHandCursor);
            m_bits[r * 3 + c] = b;
            chips->addWidget(b);
        }
        chips->addStretch();
        grid->addLayout(chips, r, 2);
    }
    grid->setColumnStretch(2, 1);
    layout->addLayout(grid);

    // The three special bits, said plainly, under the toggles.
    auto* special = new QHBoxLayout;
    special->setSpacing(16);
    special->setContentsMargins(110 + 180 + 20, 4, 0, 0);
    m_bits[9] = new QCheckBox(tr("Runs as its owner"), this);
    m_bits[9]->setToolTip(tr("setuid: a program runs with its owner's rights, whoever starts it"));
    m_bits[10] = new QCheckBox(allDirs ? tr("New items get this group") : tr("Runs as its group"), this);
    m_bits[10]->setToolTip(allDirs ? tr("setgid: items created inside belong to the folder's group (shared folders)")
                                   : tr("setgid: a program runs with its group's rights"));
    m_bits[11] = new QCheckBox(tr("Only owners can delete"), this);
    m_bits[11]->setToolTip(tr("sticky: in a shared folder, people can only delete or rename their own items (like /tmp)"));
    for (int i = 9; i < 12; ++i)
        special->addWidget(m_bits[i]);
    special->addStretch();
    m_bits[9]->setVisible(!allDirs);
    m_bits[11]->setVisible(anyDir);
    layout->addLayout(special);

    for (int i = 0; i < 12; ++i) {
        int on = 0;
        for (const PermInfo& info : std::as_const(m_infos))
            on += (info.mode & bitOf(i)) ? 1 : 0;
        const Qt::CheckState st = on == 0 ? Qt::Unchecked : on == n ? Qt::Checked : Qt::PartiallyChecked;
        m_initial[i] = st;
        m_bits[i]->setTristate(st == Qt::PartiallyChecked);
        m_bits[i]->setCheckState(st);
        connect(m_bits[i], &QCheckBox::clicked, this, [this, i] {
            if (m_bits[i]->checkState() == Qt::PartiallyChecked)  // mixed → one click decides
                m_bits[i]->setCheckState(Qt::Checked);
            m_bits[i]->setTristate(false);
        });
        connect(m_bits[i], &QCheckBox::stateChanged, this, [this] {
            syncOctal();
            updatePreview();
        });
    }
    connect(m_octal, &QLineEdit::textEdited, this, [this](const QString& text) {
        if (text.size() < 3)
            return;
        const uint32_t mode = text.toUInt(nullptr, 8);
        m_syncing = true;
        for (int i = 0; i < 12; ++i) {
            m_bits[i]->setTristate(false);
            m_bits[i]->setChecked(mode & bitOf(i));
        }
        m_syncing = false;
        m_symbolic->setText(symbolicMode(mode, false));
        updatePreview();
    });

    // Extra people and groups (POSIX ACLs).
    if (aclOk) {
        layout->addWidget(sectionLabel(tr("Extra people and groups"), this));
        m_aclRows = new QVBoxLayout;
        m_aclRows->setSpacing(4);
        layout->addLayout(m_aclRows);
        // What all items share is shown; adding and removing applies to all.
        for (const AclEntry& e : first.acl) {
            bool everywhere = true;
            for (const PermInfo& i : std::as_const(m_infos))
                everywhere &= i.acl.contains(e);
            if (everywhere)
                m_initialAcl << e;
        }
        for (const AclEntry& e : std::as_const(m_initialAcl))
            addAclRow(e);
        m_add = new QLineEdit(this);
        m_add->setPlaceholderText(tr("Add a person or group: type a name"));
        QStringList people;
        for (const auto& u : std::as_const(m_accounts.users))
            people << tr("%1 (person)").arg(u.first);
        for (const auto& g : std::as_const(m_accounts.groups))
            people << tr("%1 (group)").arg(g.first);
        auto* completer = new QCompleter(people, m_add);
        completer->setFilterMode(Qt::MatchContains);
        completer->setCaseSensitivity(Qt::CaseInsensitive);
        completer->setMaxVisibleItems(10);
        m_add->setCompleter(completer);
        connect(completer, qOverload<const QString&>(&QCompleter::activated), this, [this](const QString& t) {
            QMetaObject::invokeMethod(this, [this, t] { addPerson(t); }, Qt::QueuedConnection);
        });
        connect(m_add, &QLineEdit::returnPressed, this, [this] { addPerson(m_add->text()); });
        layout->addWidget(m_add);
        if (anyDir) {
            m_inherit = new QCheckBox(tr("New items inside get the same extra people and groups"), this);
            m_inherit->setToolTip(tr("A default ACL on the folder: files and folders created inside inherit these entries"));
            bool all = true;
            for (const PermInfo& i : std::as_const(m_infos))
                if (i.isDir)
                    all &= i.hasDefault;
            m_initialInherit = all;
            m_inherit->setChecked(all);
            connect(m_inherit, &QCheckBox::toggled, this, &PermissionsDialog::updatePreview);
            layout->addWidget(m_inherit);
        }
    } else if (posix) {
        layout->addWidget(dimLabel(first.fsName.isEmpty() || first.fsName == QLatin1String("nfs")
                                       ? tr("Extra people and groups (ACLs) aren't available here.")
                                       : tr("Extra people and groups (ACLs) aren't available on %1.").arg(first.fsName),
                                   this));
    }

    // Filesystem flags that this filesystem knows.
    struct FlagText {
        uint32_t bit;
        QString text, tip;
    };
    const QList<FlagText> flagTexts = {
        {FlagNoCow, tr("No copy-on-write"), tr("For VM images and databases on btrfs; takes effect for new data")},
        {FlagCompress, tr("Compress"), tr("btrfs compresses what's written from now on")},
        {FlagNoAtime, tr("Don't record reads"), tr("noatime: reading doesn't update the access time")},
        {FlagNoDump, tr("Skip in dump backups"), tr("nodump: dump(8) and some backup tools leave it out")},
        {FlagAppend, tr("Append only"), tr("Can only be added to, never changed or shortened (root only)")},
        {FlagImmutable, tr("Locked"), tr("immutable: nobody can change, rename or delete it, not even root, until unlocked")},
    };
    if (offered && posix) {
        layout->addWidget(sectionLabel(tr("Filesystem flags"), this));
        auto* flagGrid = new QGridLayout;
        flagGrid->setHorizontalSpacing(18);
        int k = 0;
        for (const FlagText& f : flagTexts) {
            if (!(offered & f.bit))
                continue;
            auto* b = new QCheckBox(f.text, this);
            b->setToolTip(f.tip);
            int on = 0;
            for (const PermInfo& i : std::as_const(m_infos))
                on += (i.flags & f.bit) ? 1 : 0;
            const Qt::CheckState st = on == 0 ? Qt::Unchecked : on == n ? Qt::Checked : Qt::PartiallyChecked;
            b->setTristate(st == Qt::PartiallyChecked);
            b->setCheckState(st);
            connect(b, &QCheckBox::clicked, this, [b] { b->setTristate(false); });
            connect(b, &QCheckBox::stateChanged, this, &PermissionsDialog::updatePreview);
            m_flags.append({f.bit, b});
            m_initialFlags << st;
            flagGrid->addWidget(b, k / 2, k % 2);
            ++k;
        }
        layout->addLayout(flagGrid);
    }

    if (anyDir && posix && (!remote || aclOk)) {
        m_recursive = new QCheckBox(tr("Also change everything inside"), this);
        m_recursive->setToolTip(tr("Folders inside stay openable where they can be read; files keep their run "
                                   "permission as it is unless you set it here"));
        connect(m_recursive, &QCheckBox::toggled, this, &PermissionsDialog::updatePreview);
        layout->addSpacing(8);
        layout->addWidget(m_recursive);
    }

    layout->addSpacing(8);
    m_preview = dimLabel(QString(), this);
    m_preview->setTextFormat(Qt::PlainText);
    layout->addWidget(m_preview);
    if (remote && !aclOk)
        layout->addWidget(dimLabel(tr("This server only lets Tandem change the basic permissions (no shell login)."), this));

    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    auto* cancel = new QPushButton(m_editable ? tr("Cancel") : tr("Close"), this);
    cancel->setAutoDefault(false);
    m_apply = new QPushButton(tr("Apply"), this);
    m_apply->setDefault(true);
    m_apply->setVisible(m_editable);
    buttons->addWidget(cancel);
    buttons->addWidget(m_apply);
    layout->addLayout(buttons);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_apply, &QPushButton::clicked, this, &QDialog::accept);
    for (QComboBox* c : {m_owner, m_group})
        if (c)
            connect(c, &QComboBox::currentTextChanged, this, &PermissionsDialog::updatePreview);

    if (!m_editable)
        for (QWidget* w : findChildren<QWidget*>())
            if (qobject_cast<QCheckBox*>(w) || qobject_cast<QLineEdit*>(w) || qobject_cast<QComboBox*>(w) ||
                qobject_cast<QToolButton*>(w))
                w->setEnabled(false);
    syncOctal();
    updatePreview();
}

QCheckBox* PermissionsDialog::bitBox(int who, int perm) const
{
    return m_bits[who * 3 + perm];
}

void PermissionsDialog::addAclRow(const AclEntry& e)
{
    auto* w = new QWidget(this);
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(6);
    auto* name = new QLabel(QStringLiteral("%1  %2").arg(e.isGroup ? QStringLiteral("👥") : QStringLiteral("👤"), e.name), w);
    name->setToolTip(e.isGroup ? tr("Group %1").arg(e.name) : tr("Person %1").arg(e.name));
    name->setFixedWidth(110 + 10 + 180 + 10 - 6);  // the toggles line up with the rows above
    h->addWidget(name);
    Row row;
    row.entry = e;
    row.widget = w;
    const QStringList words = {tr("Read"), tr("Write"), tr("Run")};
    for (int c = 0; c < 3; ++c) {
        row.box[c] = new QCheckBox(words[c], w);
        row.box[c]->setObjectName(QStringLiteral("perm"));
        row.box[c]->setCursor(Qt::PointingHandCursor);
        row.box[c]->setChecked(e.perms & (PermRead >> c));
        connect(row.box[c], &QCheckBox::toggled, this, &PermissionsDialog::updatePreview);
        h->addWidget(row.box[c]);
    }
    h->addStretch();
    auto* remove = new QToolButton(w);
    remove->setText(QStringLiteral("✕"));
    remove->setToolTip(tr("Remove"));
    remove->setAutoRaise(true);
    h->addWidget(remove);
    connect(remove, &QToolButton::clicked, this, [this, w] {
        m_rows.removeIf([w](const Row& r) { return r.widget == w; });
        w->deleteLater();
        updatePreview();
    });
    m_aclRows->addWidget(w);
    m_rows << row;
    if (isVisible())  // grow to fit the new row
        QTimer::singleShot(0, this, [this] { resize(width(), qMax(height(), sizeHint().height())); });
}

void PermissionsDialog::addPerson(const QString& textIn)
{
    QString text = textIn.trimmed();
    if (text.isEmpty())
        return;
    bool wantGroup = false, wantUser = false;
    for (const QString& suffix : {tr(" (group)"), tr(" (person)")}) {
        if (text.endsWith(suffix)) {
            wantGroup = suffix == tr(" (group)");
            wantUser = !wantGroup;
            text.chop(suffix.size());
        }
    }
    AclEntry e;
    if (!wantGroup) {
        if (const auto id = idOf(text, false)) {
            e.id = *id;
            e.name = text;
        }
    }
    if (e.name.isEmpty() && !wantUser) {
        if (const auto id = idOf(text, true)) {
            e.isGroup = true;
            e.id = *id;
            e.name = text;
        }
    }
    if (e.name.isEmpty()) {
        m_preview->setText(tr("No person or group called “%1”").arg(text));
        return;
    }
    for (const Row& r : std::as_const(m_rows))
        if (r.entry.isGroup == e.isGroup && r.entry.id == e.id)
            return;
    bool anyDir = false;
    for (const PermInfo& i : std::as_const(m_infos))
        anyDir |= i.isDir;
    e.perms = PermRead | (anyDir ? PermExec : 0);  // reading a folder needs "open" too
    addAclRow(e);
    m_add->clear();
    updatePreview();
}

uint32_t PermissionsDialog::checkedBits() const
{
    uint32_t m = 0;
    for (int i = 0; i < 12; ++i)
        if (m_bits[i]->checkState() == Qt::Checked)
            m |= bitOf(i);
    return m;
}

void PermissionsDialog::syncOctal()
{
    if (m_syncing)
        return;
    bool mixed = false;
    for (QCheckBox* b : m_bits)
        mixed |= b->checkState() == Qt::PartiallyChecked;
    const uint32_t m = checkedBits();
    m_symbolic->setText(mixed ? tr("(mixed)") : symbolicMode(m, false));
    m_octal->setText(mixed ? QString() : QStringLiteral("%1").arg(m, m > 0777 ? 4 : 3, 8, QLatin1Char('0')));
    m_octal->setPlaceholderText(mixed ? tr("mixed") : QString());
}

PermChange PermissionsDialog::change() const
{
    PermChange c;
    for (int i = 0; i < 12; ++i) {
        const Qt::CheckState st = m_bits[i]->checkState();
        if (st == m_initial[i] || st == Qt::PartiallyChecked)
            continue;
        (st == Qt::Checked ? c.setBits : c.clearBits) |= bitOf(i);
    }
    const PermInfo& first = m_infos.first();
    if (m_owner && !m_owner->currentText().isEmpty() && m_owner->currentText() != first.owner)
        if (const auto id = idOf(m_owner->currentText(), false))
            c.uid = *id;
    if (m_group && !m_group->currentText().isEmpty() && m_group->currentText() != first.group)
        if (const auto id = idOf(m_group->currentText(), true))
            c.gid = *id;

    if (m_aclRows) {
        QList<AclEntry> now;
        for (const Row& r : m_rows) {
            AclEntry e = r.entry;
            e.perms = (r.box[0]->isChecked() ? PermRead : 0) | (r.box[1]->isChecked() ? PermWrite : 0) |
                      (r.box[2]->isChecked() ? PermExec : 0);
            now << e;
            if (!m_initialAcl.contains(e))
                c.aclSet << e;
        }
        for (const AclEntry& e : m_initialAcl) {
            const bool kept = std::any_of(now.begin(), now.end(),
                                          [&](const AclEntry& x) { return x.isGroup == e.isGroup && x.id == e.id; });
            if (!kept)
                c.aclRemove << e;
        }
        if (m_inherit) {
            const bool on = m_inherit->isChecked();
            if (on != m_initialInherit)
                c.defaultEnabled = on;
            if (on) {  // the defaults follow the folder's own entries
                c.defaultSet = on != m_initialInherit ? now : c.aclSet;
                c.defaultRemove = c.aclRemove;
            }
        }
    }
    for (int k = 0; k < m_flags.size(); ++k) {
        const Qt::CheckState st = m_flags[k].second->checkState();
        if (st == m_initialFlags[k] || st == Qt::PartiallyChecked)
            continue;
        (st == Qt::Checked ? c.flagsSet : c.flagsClear) |= m_flags[k].first;
    }
    c.recursive = m_recursive && m_recursive->isChecked();
    return c;
}

void PermissionsDialog::updatePreview()
{
    if (!m_preview || !m_apply)
        return;
    const PermChange c = change();
    const bool something = !c.isEmpty();
    m_apply->setEnabled(m_editable && something);
    const int n = int(m_paths.size());
    m_apply->setText(n > 1 || c.recursive ? (c.recursive ? tr("Apply to all inside") : plural(tr("Apply to %n item(s)", nullptr, n), n))
                                          : tr("Apply"));
    if (!something) {
        m_preview->setText(m_editable ? tr("Nothing changes yet.") : QString());
        return;
    }
    QStringList parts;
    if (c.setBits || c.clearBits) {
        if (n == 1) {
            const PermInfo& i = m_infos.first();
            const uint32_t after = (i.mode & ~c.clearBits) | c.setBits;
            parts << tr("%1 → %2").arg(symbolicMode(i.mode, i.isDir), symbolicMode(after, i.isDir));
        } else {
            static const char* who[] = {QT_TR_NOOP("owner"), QT_TR_NOOP("group"), QT_TR_NOOP("others")};
            static const char* what[] = {QT_TR_NOOP("read"), QT_TR_NOOP("write"), QT_TR_NOOP("run")};
            for (int i = 0; i < 9; ++i) {
                if (c.setBits & bitOf(i))
                    parts << tr("+ %1 %2").arg(tr(who[i / 3]), tr(what[i % 3]));
                if (c.clearBits & bitOf(i))
                    parts << tr("− %1 %2").arg(tr(who[i / 3]), tr(what[i % 3]));
            }
        }
    }
    if (c.uid)
        parts << tr("owner → %1").arg(m_owner->currentText());
    if (c.gid)
        parts << tr("group → %1").arg(m_group->currentText());
    for (const AclEntry& e : c.aclSet)
        parts << tr("%1 may %2").arg(e.name, permWords(e.perms));
    for (const AclEntry& e : c.aclRemove)
        parts << tr("%1 removed").arg(e.name);
    if (c.defaultEnabled)
        parts << (*c.defaultEnabled ? tr("new items inside inherit them") : tr("new items no longer inherit"));
    if (c.flagsSet || c.flagsClear)
        parts << tr("flags changed");
    QString text = parts.join(QStringLiteral("  ·  "));
    if (c.recursive)
        text += tr("  ·  and everything inside");
    m_preview->setText(text);
}

}  // namespace tandem
