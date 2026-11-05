#include "ui/Keymap.h"

#include "core/TomlSettings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QKeyEvent>
#include <QKeySequence>
#include <QSaveFile>
#include <QTimer>

#include <utility>

namespace tandem {

std::function<QString(const QString&)> Keymap::actionTitle;

namespace {

QString readFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

// A TOML value of keys: "F5", ["F8", "Del"] or [].
QString keysValue(const QStringList& keys)
{
    if (keys.size() == 1)
        return tomlValueText(keys.first());
    return tomlValueText(QVariant(keys));
}

// "F5" / ["F8", "Del"] / "g g" -> list of entries (each may be a sequence).
QStringList entriesOf(const QVariant& v)
{
    if (v.typeId() == QMetaType::QString)
        return {v.toString()};
    QStringList out;
    for (const QVariant& e : v.toList())
        out << e.toString();
    return out;
}

const QString kHeader = QStringLiteral(
    "# Tandem key bindings. Saved changes apply right away.\n"
    "#\n"
    "# One key: \"F5\", \"Ctrl+Shift+C\", \"Alt+Left\". Several: [\"F8\", \"Del\"]. None: [].\n"
    "# Remove the # in front of a line to change it; put it back for the default.\n");

const QString kModalHeader = QStringLiteral(
    "# Keys while modal keys (vim, yazi) are on: Settings › Behaviour.\n"
    "# A sequence is keys with spaces between: \"g g\". Letters are case-sensitive.\n"
    "# Besides the commands above: down, up, parent, open, top, bottom,\n"
    "# half-page-down, half-page-up, visual, visual-unmark, find, find-next,\n"
    "# find-previous, palette, command-line, bookmark-jump, bookmark-set,\n"
    "# create (a name ending in / makes a folder), jobs, preview-up, preview-down.\n"
    "# bookmark-prefix: after it, a key bound to nothing goes to that bookmark (g h: home).\n");

}  // namespace

Keymap& Keymap::instance()
{
    static Keymap k;
    return k;
}

Keymap::Keymap()
{
    m_reload = new QTimer(this);
    m_reload->setSingleShot(true);
    m_reload->setInterval(150);  // editors write in steps
    connect(m_reload, &QTimer::timeout, this, [this] {
        if (readFile(path()) == m_written)
            return;
        load();
        emit changed();
    });
    rebuildModal({});
}

QString Keymap::path() const
{
    return keysFilePath();
}

const QStringList& Keymap::builtinActions()
{
    static const QStringList a = {
        QStringLiteral("down"),          QStringLiteral("up"),             QStringLiteral("parent"),
        QStringLiteral("open"),          QStringLiteral("top"),            QStringLiteral("bottom"),
        QStringLiteral("half-page-down"), QStringLiteral("half-page-up"),  QStringLiteral("visual"),
        QStringLiteral("visual-unmark"), QStringLiteral("find"),           QStringLiteral("find-next"),
        QStringLiteral("find-previous"), QStringLiteral("palette"),        QStringLiteral("command-line"),
        QStringLiteral("bookmark-jump"), QStringLiteral("bookmark-set"),   QStringLiteral("bookmark-prefix"),
        // Handled by the window, without a menu entry of their own:
        QStringLiteral("create"),        QStringLiteral("jobs"),           QStringLiteral("preview-up"),
        QStringLiteral("preview-down"),
    };
    return a;
}

const QList<std::pair<QString, QString>>& Keymap::modalDefaults()
{
    // yazi's keys, where Tandem has the thing.
    static const QList<std::pair<QString, QString>> d = {
        {QStringLiteral("down"), QStringLiteral("j")},
        {QStringLiteral("up"), QStringLiteral("k")},
        {QStringLiteral("parent"), QStringLiteral("h")},
        {QStringLiteral("open"), QStringLiteral("l")},
        {QStringLiteral("top"), QStringLiteral("g g")},
        {QStringLiteral("bottom"), QStringLiteral("G")},
        {QStringLiteral("half-page-down"), QStringLiteral("Ctrl+D")},
        {QStringLiteral("half-page-up"), QStringLiteral("Ctrl+U")},
        {QStringLiteral("visual"), QStringLiteral("v")},
        {QStringLiteral("visual-unmark"), QStringLiteral("V")},
        {QStringLiteral("find"), QStringLiteral("/")},
        {QStringLiteral("find-next"), QStringLiteral("n")},
        {QStringLiteral("find-previous"), QStringLiteral("N")},
        {QStringLiteral("palette"), QStringLiteral(":")},
        {QStringLiteral("command-line"), QStringLiteral("!")},
        {QStringLiteral("bookmark-jump"), QStringLiteral("'")},
        {QStringLiteral("bookmark-set"), QStringLiteral("m")},
        {QStringLiteral("bookmark-prefix"), QStringLiteral("g")},
        {QStringLiteral("copy-to-clipboard"), QStringLiteral("y")},
        {QStringLiteral("cut-to-clipboard"), QStringLiteral("x")},
        {QStringLiteral("paste"), QStringLiteral("p")},
        {QStringLiteral("move-to-trash"), QStringLiteral("d")},
        {QStringLiteral("delete-permanently"), QStringLiteral("D")},
        {QStringLiteral("rename"), QStringLiteral("r")},
        {QStringLiteral("open-with"), QStringLiteral("o")},
        {QStringLiteral("create"), QStringLiteral("a")},
        {QStringLiteral("show-hidden-files"), QStringLiteral(".")},
        {QStringLiteral("filter-as-you-type"), QStringLiteral("f")},
        {QStringLiteral("search-files-names-or-contents"), QStringLiteral("s")},
        {QStringLiteral("jump-to-folder-search"), QStringLiteral("z")},
        {QStringLiteral("new-tab"), QStringLiteral("t")},
        {QStringLiteral("close-tab"), QStringLiteral("q")},
        {QStringLiteral("previous-tab"), QStringLiteral("[")},
        {QStringLiteral("next-tab"), QStringLiteral("]")},
        {QStringLiteral("jobs"), QStringLiteral("w")},
        {QStringLiteral("preview-up"), QStringLiteral("K")},
        {QStringLiteral("preview-down"), QStringLiteral("J")},
        {QStringLiteral("keyboard-shortcuts"), QStringLiteral("~")},
        {QStringLiteral("sort-by-name"), QStringLiteral(", n")},
        {QStringLiteral("sort-by-extension"), QStringLiteral(", e")},
        {QStringLiteral("sort-by-date"), QStringLiteral(", m")},
        {QStringLiteral("sort-by-size"), QStringLiteral(", s")},
        {QStringLiteral("tab-1"), QStringLiteral("1")},
        {QStringLiteral("tab-2"), QStringLiteral("2")},
        {QStringLiteral("tab-3"), QStringLiteral("3")},
        {QStringLiteral("tab-4"), QStringLiteral("4")},
        {QStringLiteral("tab-5"), QStringLiteral("5")},
        {QStringLiteral("tab-6"), QStringLiteral("6")},
        {QStringLiteral("tab-7"), QStringLiteral("7")},
        {QStringLiteral("tab-8"), QStringLiteral("8")},
        {QStringLiteral("tab-9"), QStringLiteral("9")},
        {QStringLiteral("put-on-shelf-1"), QStringLiteral("b 1")},
        {QStringLiteral("put-on-shelf-2"), QStringLiteral("b 2")},
        {QStringLiteral("put-on-shelf-3"), QStringLiteral("b 3")},
        {QStringLiteral("put-on-shelf-4"), QStringLiteral("b 4")},
        {QStringLiteral("put-on-shelf-5"), QStringLiteral("b 5")},
        {QStringLiteral("put-on-shelf-6"), QStringLiteral("b 6")},
        {QStringLiteral("put-on-shelf-7"), QStringLiteral("b 7")},
        {QStringLiteral("put-on-shelf-8"), QStringLiteral("b 8")},
        {QStringLiteral("put-on-shelf-9"), QStringLiteral("b 9")},
        {QStringLiteral("copy-shelf-1-here"), QStringLiteral("B 1")},
        {QStringLiteral("copy-shelf-2-here"), QStringLiteral("B 2")},
        {QStringLiteral("copy-shelf-3-here"), QStringLiteral("B 3")},
        {QStringLiteral("copy-shelf-4-here"), QStringLiteral("B 4")},
        {QStringLiteral("copy-shelf-5-here"), QStringLiteral("B 5")},
        {QStringLiteral("copy-shelf-6-here"), QStringLiteral("B 6")},
        {QStringLiteral("copy-shelf-7-here"), QStringLiteral("B 7")},
        {QStringLiteral("copy-shelf-8-here"), QStringLiteral("B 8")},
        {QStringLiteral("copy-shelf-9-here"), QStringLiteral("B 9")},
    };
    return d;
}

QString Keymap::normalizeToken(const QString& t)
{
    if (t.size() == 1)
        return t;
    const QKeySequence k = QKeySequence::fromString(t, QKeySequence::PortableText);
    if (k.isEmpty() || k.count() != 1)
        return t;
    return k.toString(QKeySequence::PortableText);
}

QString Keymap::tokenFor(const QKeyEvent* e)
{
    const Qt::KeyboardModifiers mods = e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    if (mods) {
        const Qt::KeyboardModifiers all = e->modifiers() & ~Qt::KeypadModifier;
        return QKeySequence(QKeyCombination(all, Qt::Key(e->key()))).toString(QKeySequence::PortableText);
    }
    const QString text = e->text();
    if (text.size() == 1 && text.at(0).isPrint() && text != QLatin1String(" "))
        return text;
    return QKeySequence(e->key()).toString(QKeySequence::PortableText);
}

void Keymap::rebuildModal(const QHash<QString, QStringList>& overrides)
{
    QList<Binding> out;
    QSet<QString> taken;  // sequences the file gives to something
    auto add = [&out](const QString& action, const QString& entry) {
        Binding b;
        for (const QString& t : entry.split(QLatin1Char(' '), Qt::SkipEmptyParts))
            b.sequence << normalizeToken(t);
        if (!b.sequence.isEmpty()) {
            b.action = action;
            out.append(b);
        }
    };
    for (auto it = overrides.cbegin(); it != overrides.cend(); ++it)
        for (const QString& e : it.value()) {
            const int before = int(out.size());
            add(it.key(), e);
            if (out.size() > before)
                taken.insert(out.last().sequence.join(QLatin1Char(' ')));
        }
    for (const auto& [action, keys] : modalDefaults()) {
        if (overrides.contains(action))
            continue;
        const int before = int(out.size());
        add(action, keys);
        if (out.size() > before && taken.contains(out.last().sequence.join(QLatin1Char(' '))))
            out.removeLast();  // the file gave this key to something else
    }
    m_modal = out;
}

bool Keymap::load()
{
    watch();
    const QString text = readFile(path());
    m_written = text;  // what the watcher compares against: reload only when it differs
    QVariantMap map;
    QString error;
    if (!parseToml(text, &map, &error)) {
        m_error = QStringLiteral("keys.toml, %1").arg(error);
        return false;
    }
    m_error.clear();
    QHash<QString, QStringList> commands, modal;
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        if (it.key().startsWith(QLatin1String("keys/")))
            commands.insert(it.key().mid(5), entriesOf(it.value()));
        else if (it.key().startsWith(QLatin1String("modal/")))
            modal.insert(it.key().mid(6), entriesOf(it.value()));
    }
    m_commandKeys = commands;
    rebuildModal(modal);
    return true;
}

void Keymap::watch()
{
    if (!m_watcher) {
        m_watcher = new QFileSystemWatcher(this);
        connect(m_watcher, &QFileSystemWatcher::fileChanged, m_reload, qOverload<>(&QTimer::start));
        connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] {
            // Editors save by replacing the file: watch the new one.
            if (QFile::exists(path()) && !m_watcher->files().contains(path()))
                m_watcher->addPath(path());
            m_reload->start();
        });
    }
    const QString dir = QFileInfo(path()).path();
    if (!m_watcher->directories().isEmpty() && m_watcher->directories() != QStringList{dir})
        m_watcher->removePaths(m_watcher->directories() + m_watcher->files());
    if (QDir(dir).exists() && !m_watcher->directories().contains(dir))
        m_watcher->addPath(dir);
    if (QFile::exists(path()) && !m_watcher->files().contains(path()))
        m_watcher->addPath(path());
}

void Keymap::setCommands(const QList<Command>& commands)
{
    m_commands = commands;
}

void Keymap::writeFileText(const QString& text)
{
    QDir().mkpath(QFileInfo(path()).path());
    QSaveFile f(path());
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.write(text.toUtf8());
    if (f.commit())
        m_written = text;
    watch();
}

void Keymap::ensureFile()
{
    if (QFile::exists(path())) {
        load();
        return;
    }
    const QVariantMap pending = std::exchange(m_pendingImport, {});
    QString text = kHeader + QStringLiteral("\n[keys]\n");
    for (const Command& c : std::as_const(m_commands))
        text += QStringLiteral("# %1 = %2\n").arg(c.id, keysValue(c.defaults));
    text += QStringLiteral("\n[modal]\n") + kModalHeader;
    for (const auto& [action, keys] : modalDefaults())
        text += QStringLiteral("# %1 = %2\n").arg(action, tomlValueText(keys));
    writeFileText(text);
    importOld(pending);  // keys changed in older versions
    load();
}

std::optional<QStringList> Keymap::commandKeys(const QString& id) const
{
    const auto it = m_commandKeys.constFind(id);
    if (it == m_commandKeys.cend())
        return std::nullopt;
    return *it;
}

void Keymap::setCommandKeys(const QString& id, const std::optional<QStringList>& keys)
{
    QString text = readFile(path());
    QVariantMap check;
    if (!parseToml(text, &check))
        return;  // a broken hand edit: leave it as it is
    if (text.isEmpty())
        text = kHeader;
    QStringList defaults;
    for (const Command& c : std::as_const(m_commands))
        if (c.id == id)
            defaults = c.defaults;
    if (keys)
        text = setTomlLine(text, QStringLiteral("keys"), id, keysValue(*keys));
    else
        text = setTomlLine(text, QStringLiteral("keys"), id, std::nullopt, keysValue(defaults));
    writeFileText(text);
    if (keys)
        m_commandKeys.insert(id, *keys);
    else
        m_commandKeys.remove(id);
}

void Keymap::importOld(const QVariantMap& keys)
{
    for (auto it = keys.cbegin(); it != keys.cend(); ++it) {
        QStringList list = it.value().toStringList();
        if (list == QStringList{QStringLiteral("none")})
            list.clear();
        setCommandKeys(it.key().mid(5), list);
    }
}

}  // namespace tandem
