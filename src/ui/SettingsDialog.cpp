#include "ui/SettingsDialog.h"

#include "ui/IconCache.h"
#include "ui/MainWindow.h"
#include "ui/Panel.h"
#include "ui/Theme.h"

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace tandem {

// ------------------------------------------------------------ key bindings

namespace {

QList<QKeySequence> defaultKeys(QAction* a)
{
    return a->property("defaultKeys").value<QList<QKeySequence>>();
}

QString keysText(const QList<QKeySequence>& keys)
{
    QStringList out;
    for (const QKeySequence& k : keys)
        if (!k.isEmpty())
            out << k.toString(QKeySequence::NativeText);
    return out.join(QStringLiteral(", "));
}

}  // namespace

void applyKeyBindings(const QList<QAction*>& actions)
{
    QSettings s;
    for (QAction* a : actions) {
        const QString id = a->property("commandId").toString();
        if (id.isEmpty() || !s.contains(QStringLiteral("keys/") + id))
            continue;
        QList<QKeySequence> keys;
        for (const QString& k : s.value(QStringLiteral("keys/") + id).toStringList())
            if (!k.isEmpty() && k != QLatin1String("none"))
                keys << QKeySequence::fromString(k, QKeySequence::PortableText);
        a->setShortcuts(keys);
    }
}

void saveKeyBinding(QAction* a)
{
    const QString id = a->property("commandId").toString();
    if (id.isEmpty())
        return;
    QSettings s;
    if (a->shortcuts() == defaultKeys(a)) {
        s.remove(QStringLiteral("keys/") + id);
        return;
    }
    QStringList keys;
    for (const QKeySequence& k : a->shortcuts())
        keys << k.toString(QKeySequence::PortableText);
    s.setValue(QStringLiteral("keys/") + id, keys.isEmpty() ? QStringList{QStringLiteral("none")} : keys);
}

// ------------------------------------------------------------------ dialog

namespace {

QLabel* hint(const QString& text, QWidget* parent)
{
    auto* l = new QLabel(text, parent);
    l->setWordWrap(true);
    setThemeStyle(l, QStringLiteral("color: @dimText;"));
    return l;
}

QLabel* title(const QString& text, QWidget* parent)
{
    auto* l = new QLabel(text, parent);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    return l;
}

// A page: rows of (title, control, hint), scrolling if needed.
struct Page {
    QScrollArea* scroll;
    QVBoxLayout* rows;
    explicit Page(QWidget* parent)
    {
        scroll = new QScrollArea(parent);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto* inner = new QWidget(scroll);
        rows = new QVBoxLayout(inner);
        rows->setContentsMargins(6, 4, 14, 10);
        rows->setSpacing(4);
        scroll->setWidget(inner);
    }
    void add(const QString& name, QWidget* control, const QString& help = {})
    {
        QWidget* parent = rows->parentWidget();
        if (rows->count())
            rows->addSpacing(name.isEmpty() ? 2 : 14);  // untitled rows belong to the one above
        if (!name.isEmpty())
            rows->addWidget(title(name, parent));
        rows->addWidget(control);
        if (!help.isEmpty())
            rows->addWidget(hint(help, parent));
    }
    QWidget* finish()
    {
        rows->addStretch(1);
        return scroll;
    }
};

// One choice out of a few, as a row of toggles.
QWidget* segments(QWidget* parent, const QList<QPair<QString, QString>>& choices, const QString& current,
                  std::function<void(const QString&)> chosen)
{
    auto* w = new QWidget(parent);
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);
    auto* group = new QButtonGroup(w);
    for (const auto& [code, label] : choices) {
        auto* b = new QToolButton(w);
        b->setObjectName(QStringLiteral("segment"));
        b->setText(label);
        b->setCheckable(true);
        b->setChecked(code == current);
        group->addButton(b);
        h->addWidget(b);
        QObject::connect(b, &QToolButton::clicked, w, [chosen, code] { chosen(code); });
    }
    h->addStretch(1);
    return w;
}

// An editable choice of installed programs ("" = automatic).
QComboBox* programBox(QWidget* parent, const QString& key, const QStringList& candidates, const QString& automatic)
{
    auto* box = new QComboBox(parent);
    box->setEditable(true);
    box->setInsertPolicy(QComboBox::NoInsert);
    box->addItem(automatic, QString());
    for (const QString& c : candidates) {
        const QString program = c.section(QLatin1Char(' '), 0, 0);
        if (!QStandardPaths::findExecutable(program).isEmpty())
            box->addItem(c, c);
    }
    const QString current = QSettings().value(key).toString();
    if (current.isEmpty()) {
        box->setCurrentIndex(0);
    } else {
        const int at = box->findData(current);
        if (at >= 0)
            box->setCurrentIndex(at);
        else
            box->setEditText(current);
    }
    auto save = [box, key, automatic] {
        QString v = box->currentText().trimmed();
        if (v == automatic)
            v.clear();
        QSettings().setValue(key, v);
    };
    QObject::connect(box, &QComboBox::currentIndexChanged, box, save);
    QObject::connect(box->lineEdit(), &QLineEdit::editingFinished, box, save);
    return box;
}

}  // namespace

SettingsDialog::SettingsDialog(MainWindow* window) : QDialog(window), m_window(window)
{
    setWindowTitle(tr("Settings"));
    resize(860, 600);
    setThemeStyle(this, QStringLiteral(
        "QListWidget#nav { background: transparent; border: none; outline: none; }"
        "QListWidget#nav::item { padding: 8px 12px; border-radius: 6px; color: @text; }"
        "QListWidget#nav::item:selected { background: @headerActive; color: @text; }"
        "QTreeWidget::item { padding: 4px 2px; }"
        "QToolButton#segment { padding: 5px 14px; border: 1px solid @control; background: @base; }"
        "QToolButton#segment:checked { background: @accent; color: @accentText; border-color: @accent; }"));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 16, 16, 12);
    auto* body = new QHBoxLayout;
    body->setSpacing(18);
    m_nav = new QListWidget(this);
    m_nav->setObjectName(QStringLiteral("nav"));
    m_nav->setFixedWidth(180);
    m_nav->setIconSize(QSize(18, 18));
    const QList<QPair<QString, QString>> pages = {
        {tr("Appearance"), QStringLiteral("preferences-desktop-theme")},
        {tr("Behaviour"), QStringLiteral("preferences-system")},
        {tr("Tools"), QStringLiteral("applications-utilities")},
        {tr("Keys"), QStringLiteral("preferences-desktop-keyboard")},
        {tr("Plugins"), QStringLiteral("preferences-plugin")},
    };
    for (const auto& [name, icon] : pages)
        new QListWidgetItem(IconCache::themed(icon.toUtf8().constData(), "preferences-system"), name, m_nav);
    body->addWidget(m_nav);
    m_pages = new QStackedWidget(this);
    m_pages->addWidget(appearancePage());
    m_pages->addWidget(behaviourPage());
    m_pages->addWidget(toolsPage());
    m_pages->addWidget(keysPage());
    m_pages->addWidget(pluginsPage());
    body->addWidget(m_pages, 1);
    outer->addLayout(body, 1);
    connect(m_nav, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(hint(tr("Changes apply right away."), this), 1);
    auto* close = new QPushButton(tr("Close"), this);
    close->setDefault(true);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addWidget(close);
    outer->addLayout(buttons);
    m_nav->setCurrentRow(0);
}

void SettingsDialog::showPage(int index)
{
    m_nav->setCurrentRow(index);
}

// Theme: one for always, or one for light and one for dark following the
// desktop. Applied at once; own themes are JSON files in the themes folder.
QWidget* SettingsDialog::themeChooser()
{
    auto* box = new QWidget(this);
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(6);
    auto* main = new QComboBox(box);
    main->setObjectName(QStringLiteral("theme"));
    auto* pair = new QWidget(box);
    auto* ph = new QHBoxLayout(pair);
    ph->setContentsMargins(0, 0, 0, 0);
    auto* light = new QComboBox(pair);
    auto* dark = new QComboBox(pair);
    ph->addWidget(new QLabel(tr("Light:"), pair));
    ph->addWidget(light, 1);
    ph->addSpacing(12);
    ph->addWidget(new QLabel(tr("Dark:"), pair));
    ph->addWidget(dark, 1);
    for (QComboBox* c : {main, light, dark})
        c->setMaximumWidth(320);
    pair->setMaximumWidth(560);

    auto fill = [main, light, dark] {
        const QSettings s;
        const QSignalBlocker b1(main), b2(light), b3(dark);
        main->clear();
        light->clear();
        dark->clear();
        main->addItem(tr("Like the desktop"), QStringLiteral("system"));
        for (const ThemeInfo& t : availableThemes()) {
            main->addItem(t.name, t.id);
            (t.dark ? dark : light)->addItem(t.name, t.id);
        }
        QString chosen = s.value(QStringLiteral("view/theme"), QStringLiteral("system")).toString();
        if (chosen == QLatin1String("dark") || chosen == QLatin1String("light"))
            chosen = QStringLiteral("tandem-") + chosen;
        main->setCurrentIndex(qMax(0, main->findData(chosen)));
        light->setCurrentIndex(qMax(0, light->findData(s.value(QStringLiteral("view/themeLight"), QStringLiteral("tandem-light")))));
        dark->setCurrentIndex(qMax(0, dark->findData(s.value(QStringLiteral("view/themeDark"), QStringLiteral("tandem-dark")))));
    };
    fill();
    pair->setVisible(main->currentIndex() == 0);
    auto apply = [] { applyTheme(*qApp); };
    connect(main, &QComboBox::currentIndexChanged, this, [main, pair, apply] {
        QSettings().setValue(QStringLiteral("view/theme"), main->currentData());
        pair->setVisible(main->currentIndex() == 0);
        apply();
    });
    connect(light, &QComboBox::currentIndexChanged, this, [light, apply] {
        QSettings().setValue(QStringLiteral("view/themeLight"), light->currentData());
        apply();
    });
    connect(dark, &QComboBox::currentIndexChanged, this, [dark, apply] {
        QSettings().setValue(QStringLiteral("view/themeDark"), dark->currentData());
        apply();
    });

    auto* tools = new QWidget(box);
    auto* th = new QHBoxLayout(tools);
    th->setContentsMargins(0, 0, 0, 0);
    auto* copy = new QPushButton(tr("Copy to edit"), tools);
    copy->setToolTip(tr("Saves the current theme with all its colours as your own theme, to change in a text editor"));
    auto* folder = new QPushButton(tr("Open themes folder"), tools);
    th->addWidget(copy);
    th->addWidget(folder);
    th->addStretch(1);
    connect(folder, &QPushButton::clicked, this, [] {
        QDir().mkpath(themesFolder());
        QDesktopServices::openUrl(QUrl::fromLocalFile(themesFolder()));
    });
    connect(copy, &QPushButton::clicked, this, [this, main, fill] {
        Theme t = theme();
        t.name = tr("%1 (copy)").arg(t.name);
        QString id = theme().id + QStringLiteral("-copy");
        for (int i = 2; QFileInfo::exists(themesFolder() + QLatin1Char('/') + id + QStringLiteral(".json")); ++i)
            id = theme().id + QStringLiteral("-copy-%1").arg(i);
        QDir().mkpath(themesFolder());
        QFile f(themesFolder() + QLatin1Char('/') + id + QStringLiteral(".json"));
        if (!f.open(QIODevice::WriteOnly) || f.write(themeJson(t)) < 0) {
            m_window->active()->showMessage(tr("Cannot write %1: %2").arg(f.fileName(), f.errorString()), true);
            return;
        }
        f.close();
        fill();
        main->setCurrentIndex(main->findData(id));  // applies it
        QDesktopServices::openUrl(QUrl::fromLocalFile(f.fileName()));
    });

    v->addWidget(main);
    v->addWidget(pair);
    v->addWidget(tools);
    v->addWidget(hint(tr("Changes apply at once. Your own themes are JSON files in the themes folder; they are "
                         "reloaded as soon as you save them."),
                      box));
    return box;
}

QWidget* SettingsDialog::appearancePage()
{
    Page page(this);
    QSettings s;
    page.add(tr("Theme"), themeChooser());

    auto* language = new QComboBox(this);
    language->addItem(tr("Like the system"), QString());
    language->addItem(QStringLiteral("English"), QStringLiteral("en"));
    language->addItem(QStringLiteral("Deutsch"), QStringLiteral("de"));
    language->setCurrentIndex(qMax(0, language->findData(s.value(QStringLiteral("view/language")).toString())));
    connect(language, &QComboBox::currentIndexChanged, this,
            [language] { QSettings().setValue(QStringLiteral("view/language"), language->currentData()); });
    language->setMaximumWidth(320);
    page.add(tr("Language"), language, tr("Takes effect the next time Tandem starts."));

    auto* icons = new QComboBox(this);
    icons->addItem(tr("Like the desktop (%1)").arg(IconCache::systemTheme()), QString());
    for (const IconTheme& it : std::as_const(m_window->m_iconThemes))
        icons->addItem(it.name == it.id ? it.name : QStringLiteral("%1  (%2)").arg(it.name, it.id), it.id);
    icons->setCurrentIndex(qMax(0, icons->findData(s.value(QStringLiteral("view/iconTheme")).toString())));
    connect(icons, &QComboBox::currentIndexChanged, this, [this, icons] { m_window->setIconTheme(icons->currentData().toString()); });
    icons->setMaximumWidth(320);
    page.add(tr("Icons"), icons);

    page.add(tr("Preview (Ctrl+Q)"),
             segments(this, {{QStringLiteral("each"), tr("Below each panel")}, {QStringLiteral("bottom"), tr("Below both")},
                             {QStringLiteral("right"), tr("On the right")}},
                      m_window->previewMode(), [this](const QString& code) { m_window->setPreviewPosition(code); }));

    auto toggle = [this](const QString& text, QAction* action) {
        auto* box = new QCheckBox(text, this);
        box->setChecked(action->isChecked());
        connect(box, &QCheckBox::toggled, this, [action](bool on) {
            if (action->isChecked() != on)
                action->trigger();
        });
        return box;
    };
    page.add(tr("In the file lists"), toggle(tr("Show hidden files (Ctrl+H)"), m_window->m_hiddenAction));
    page.add(QString(), toggle(tr("Show Git status (changed, new, ignored files)"), m_window->m_actions.value(QStringLiteral("git"))));
    auto* menu = new QCheckBox(tr("Show the menu bar (Ctrl+Shift+M)"), this);
    menu->setChecked(m_window->menuBar()->isVisible());
    connect(menu, &QCheckBox::toggled, this, [this](bool on) {
        m_window->menuBar()->setVisible(on);
        QSettings().setValue(QStringLiteral("window/menuBar"), on);
    });
    page.add(QString(), menu);
    return page.finish();
}

QWidget* SettingsDialog::behaviourPage()
{
    Page page(this);
    auto toggle = [this](const QString& text, QAction* action) {
        auto* box = new QCheckBox(text, this);
        box->setChecked(action->isChecked());
        connect(box, &QCheckBox::toggled, this, [action](bool on) {
            if (action->isChecked() != on)
                action->trigger();
        });
        return box;
    };
    page.add(tr("Keyboard"),
             toggle(tr("Arrow keys walk folders"), m_window->m_actions.value(QStringLiteral("arrowFolders"))),
             tr("→ enters the folder or archive under the cursor, ← goes up, like lynx or mc. → never opens a "
                "file."));
    page.add(QString(), toggle(tr("Vim keys"), m_window->m_actions.value(QStringLiteral("vim"))),
             tr("j k move, h l out of and into folders, gg G, / to search, v to mark, : for the palette. Typing "
                "letters no longer starts the quick search."));
    page.add(tr("Notifications"),
             toggle(tr("Notify when long operations finish"), m_window->m_actions.value(QStringLiteral("notifications"))),
             tr("A desktop notification when a copy or move that took a while is done and Tandem isn't in front."));
    return page.finish();
}

// Plugins run with the user's rights: each one is switched on by hand, and
// again after its plugin.json changed.
QWidget* SettingsDialog::pluginsPage()
{
    Page page(this);
    m_pluginList = new QWidget(this);
    auto* v = new QVBoxLayout(m_pluginList);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);
    fillPlugins();
    page.add(tr("Plugins"), m_pluginList,
             tr("Plugins are programs that run with your rights: switch on only those you trust. One that changes "
                "is switched off until you switch it on again. Their commands appear in the context menu, under "
                "Tools › Plugins, in the command palette and on the Keys page."));

    auto* tools = new QWidget(this);
    auto* th = new QHBoxLayout(tools);
    th->setContentsMargins(0, 0, 0, 0);
    auto* folder = new QPushButton(tr("Open plugins folder"), tools);
    auto* reload = new QPushButton(tr("Reload"), tools);
    th->addWidget(folder);
    th->addWidget(reload);
    th->addStretch(1);
    connect(folder, &QPushButton::clicked, this, [] {
        QDir().mkpath(pluginsFolder());
        QDesktopServices::openUrl(QUrl::fromLocalFile(pluginsFolder()));
    });
    connect(reload, &QPushButton::clicked, this, [this] {
        QPointer<SettingsDialog> self(this);
        m_window->loadPlugins([self] {
            if (self)
                self->fillPlugins();
        });
    });
    page.add(QString(), tools, tr("Own plugins go in %1, each in a folder with a plugin.json.").arg(pluginsFolder()));
    return page.finish();
}

void SettingsDialog::fillPlugins()
{
    auto* v = static_cast<QVBoxLayout*>(m_pluginList->layout());
    while (QLayoutItem* item = v->takeAt(0)) {
        if (QWidget* w = item->widget()) {  // later: one of them may be the box just clicked
            w->hide();
            w->deleteLater();
        }
        delete item;
    }
    if (m_window->m_plugins.isEmpty())
        v->addWidget(hint(tr("No plugins installed."), m_pluginList));
    for (const Plugin& plugin : std::as_const(m_window->m_plugins)) {
        const PluginTrust trust = pluginTrust(plugin);
        auto* box = new QCheckBox(plugin.name, m_pluginList);
        box->setChecked(trust == PluginTrust::On);
        box->setEnabled(plugin.error.isEmpty());
        QStringList about;
        if (!plugin.error.isEmpty())
            about << tr("Can't be used: %1").arg(plugin.error);
        else if (trust == PluginTrust::Changed)
            about << tr("Changed since you switched it on: check it, then switch it on again.");
        if (!plugin.description.isEmpty())
            about << plugin.description;
        about << plugin.dir;
        v->addWidget(box);
        QLabel* note = hint(about.join(QLatin1Char('\n')), m_pluginList);
        note->setContentsMargins(26, 0, 0, 6);
        v->addWidget(note);
        connect(box, &QCheckBox::toggled, this, [this, plugin](bool on) {
            setPluginEnabled(plugin, on);
            m_window->installPluginCommands();
            fillPlugins();
        });
    }
}

QWidget* SettingsDialog::toolsPage()
{
    Page page(this);
    page.add(tr("Editor (F4)"),
             programBox(this, QStringLiteral("tools/editor"),
                        {QStringLiteral("kate"), QStringLiteral("gnome-text-editor"), QStringLiteral("gedit"),
                         QStringLiteral("code"), QStringLiteral("codium"), QStringLiteral("zed"), QStringLiteral("mousepad"),
                         QStringLiteral("xed"), QStringLiteral("pluma"), QStringLiteral("featherpad"),
                         QStringLiteral("kwrite"), QStringLiteral("subl")},
                        tr("The default app for the file type")),
             tr("A command; the file is added at the end. Terminal editors (vim, nano) need a terminal: "
                "e.g. \"kitty nvim\"."));
    page.add(tr("Terminal (Ctrl+Alt+T)"),
             programBox(this, QStringLiteral("tools/terminal"),
                        {QStringLiteral("kitty"), QStringLiteral("alacritty"), QStringLiteral("foot"), QStringLiteral("wezterm"),
                         QStringLiteral("ghostty"), QStringLiteral("konsole"), QStringLiteral("gnome-terminal"),
                         QStringLiteral("xfce4-terminal"), QStringLiteral("tilix"), QStringLiteral("xterm")},
                        tr("Found automatically")),
             tr("Also used for commands run with Ctrl+Enter and for $EDITOR."));
    page.add(tr("Comparing two files"),
             programBox(this, QStringLiteral("tools/diff"),
                        {QStringLiteral("meld"), QStringLiteral("kdiff3"), QStringLiteral("kompare"), QStringLiteral("diffuse")},
                        tr("Found automatically (else diff -u)")),
             tr("Used by the sync dialog's Enter."));
    auto* names = new QLineEdit(this);
    names->setPlaceholderText(tr("$EDITOR in your terminal"));
    names->setText(QSettings().value(QStringLiteral("tools/nameEditor")).toString());
    connect(names, &QLineEdit::editingFinished, this,
            [names] { QSettings().setValue(QStringLiteral("tools/nameEditor"), names->text().trimmed()); });
    page.add(tr("Renaming in an editor (Ctrl+E)"), names,
             tr("A command that waits until you close the file; %f is the file with the names. Empty: $EDITOR "
                "in your terminal."));
    return page.finish();
}

QWidget* SettingsDialog::keysPage()
{
    auto* w = new QWidget(this);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(6, 4, 6, 4);
    m_keyFilter = new QLineEdit(w);
    m_keyFilter->setPlaceholderText(tr("Find a command or a key"));
    m_keyFilter->setClearButtonEnabled(true);
    v->addWidget(m_keyFilter);
    m_keys = new QTreeWidget(w);
    m_keys->setColumnCount(2);
    m_keys->setHeaderLabels({tr("Command"), tr("Keys")});
    m_keys->setRootIsDecorated(false);
    m_keys->setUniformRowHeights(true);
    m_keys->setAlternatingRowColors(true);
    m_keys->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_keys->header()->resizeSection(1, 200);
    v->addWidget(m_keys, 1);

    auto* editRow = new QHBoxLayout;
    editRow->addWidget(new QLabel(tr("Press the new keys:"), w));
    m_keyEdit = new QKeySequenceEdit(w);
    m_keyEdit->setMaximumSequenceLength(1);
    m_keyEdit->setClearButtonEnabled(false);
    editRow->addWidget(m_keyEdit, 1);
    m_keyClear = new QPushButton(tr("No key"), w);
    m_keyReset = new QPushButton(tr("Default"), w);
    for (QPushButton* b : {m_keyClear, m_keyReset})
        b->setAutoDefault(false);
    editRow->addWidget(m_keyClear);
    editRow->addWidget(m_keyReset);
    v->addLayout(editRow);
    m_keyNote = hint(tr("Pick a command, then press its new keys."), w);
    v->addWidget(m_keyNote);
    auto* resetAll = new QPushButton(tr("Reset all keys"), w);
    resetAll->setAutoDefault(false);
    auto* bottom = new QHBoxLayout;
    bottom->addStretch(1);
    bottom->addWidget(resetAll);
    v->addLayout(bottom);

    fillKeys();
    connect(m_keyFilter, &QLineEdit::textChanged, this, &SettingsDialog::fillKeys);
    connect(m_keys, &QTreeWidget::currentItemChanged, this, &SettingsDialog::keySelected);
    connect(m_keyEdit, &QKeySequenceEdit::keySequenceChanged, this, [this](const QKeySequence& seq) {
        if (!m_loadingKey && !seq.isEmpty())
            assignKeys(seq);
    });
    connect(m_keyClear, &QPushButton::clicked, this, [this] { assignKeys({}); });
    connect(m_keyReset, &QPushButton::clicked, this, [this] {
        QTreeWidgetItem* it = m_keys->currentItem();
        auto* a = it ? it->data(0, Qt::UserRole).value<QAction*>() : nullptr;
        if (!a)
            return;
        a->setShortcuts(defaultKeys(a));
        saveKeyBinding(a);
        fillKeys();
    });
    connect(resetAll, &QPushButton::clicked, this, [this] {
        for (QAction* a : m_window->actions()) {
            if (a->property("commandId").toString().isEmpty())
                continue;
            a->setShortcuts(defaultKeys(a));
            saveKeyBinding(a);
        }
        m_keyNote->setText(tr("All keys are back to their defaults."));
        fillKeys();
    });
    return w;
}

void SettingsDialog::fillKeys()
{
    QAction* selected = m_keys->currentItem() ? m_keys->currentItem()->data(0, Qt::UserRole).value<QAction*>() : nullptr;
    const QString q = m_keyFilter->text().trimmed();
    m_keys->clear();
    QTreeWidgetItem* reselect = nullptr;
    for (QAction* a : m_window->actions()) {
        if (a->property("commandId").toString().isEmpty())
            continue;
        const QString name = QStringLiteral("%1 › %2").arg(a->property("category").toString(), a->text().remove(QLatin1Char('&')));
        const QString keys = keysText(a->shortcuts());
        if (!q.isEmpty() && !name.contains(q, Qt::CaseInsensitive) && !keys.contains(q, Qt::CaseInsensitive))
            continue;
        auto* it = new QTreeWidgetItem(m_keys, {name, keys});
        it->setData(0, Qt::UserRole, QVariant::fromValue(a));
        if (a->shortcuts() != defaultKeys(a)) {  // changed: in the accent colour
            it->setForeground(1, theme().accent);
            it->setToolTip(1, tr("Default: %1").arg(keysText(defaultKeys(a)).isEmpty() ? tr("none") : keysText(defaultKeys(a))));
        }
        if (a == selected)
            reselect = it;
    }
    if (reselect)
        m_keys->setCurrentItem(reselect);
    keySelected();
}

void SettingsDialog::keySelected()
{
    QTreeWidgetItem* it = m_keys->currentItem();
    auto* a = it ? it->data(0, Qt::UserRole).value<QAction*>() : nullptr;
    m_loadingKey = true;
    m_keyEdit->setKeySequence(a && !a->shortcuts().isEmpty() ? a->shortcuts().first() : QKeySequence());
    m_loadingKey = false;
    for (QWidget* w : {static_cast<QWidget*>(m_keyEdit), static_cast<QWidget*>(m_keyClear), static_cast<QWidget*>(m_keyReset)})
        w->setEnabled(a != nullptr);
}

void SettingsDialog::assignKeys(const QKeySequence& seq)
{
    QTreeWidgetItem* it = m_keys->currentItem();
    auto* a = it ? it->data(0, Qt::UserRole).value<QAction*>() : nullptr;
    if (!a)
        return;
    QString note;
    if (!seq.isEmpty()) {
        // A key does one thing: whoever had it gives it up.
        for (QAction* other : m_window->actions()) {
            if (other == a || !other->shortcuts().contains(seq))
                continue;
            QList<QKeySequence> keys = other->shortcuts();
            keys.removeAll(seq);
            other->setShortcuts(keys);
            saveKeyBinding(other);
            note = tr("%1 was used by “%2”, which now has %3.")
                       .arg(seq.toString(QKeySequence::NativeText), other->text().remove(QLatin1Char('&')),
                            keys.isEmpty() ? tr("no key") : keysText(keys));
        }
    }
    a->setShortcuts(seq.isEmpty() ? QList<QKeySequence>{} : QList<QKeySequence>{seq});
    saveKeyBinding(a);
    m_keyNote->setText(note.isEmpty() ? (seq.isEmpty() ? tr("“%1” has no key now.").arg(a->text().remove(QLatin1Char('&')))
                                                       : tr("“%1” is now %2.").arg(a->text().remove(QLatin1Char('&')),
                                                                                 seq.toString(QKeySequence::NativeText)))
                                      : note);
    fillKeys();
}

}  // namespace tandem
