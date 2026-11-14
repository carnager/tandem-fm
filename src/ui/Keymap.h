#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QStringList>
#include <QVariantMap>

#include <functional>
#include <optional>

class QAction;
class QFileSystemWatcher;
class QKeyEvent;
class QTimer;

namespace tandem {

// keys.toml: the keys of every command ([keys]) and the modal, yazi-like
// keys ([modal]). Watched: saving the file applies it right away.
//
//   [keys]
//   copy = "F5"                 # one key
//   move-to-trash = ["F8", "Del"]
//   delete-permanently = []     # none
//
//   [modal]
//   top = "g g"                 # a sequence
//   down = ["j", "Down"]
class Keymap : public QObject {
    Q_OBJECT
public:
    struct Binding {
        QStringList sequence;  // tokens: "g", "G", "Ctrl+D", "Space"
        QString action;        // a built-in (down, top…) or a command id
    };
    struct Command {
        QString id;
        QStringList defaults;  // portable key texts
    };

    static Keymap& instance();

    QString path() const;
    // Reads the file (and starts watching it). False with error() on a bad file;
    // what was read before stays in use.
    bool load();
    QString error() const { return m_error; }

    // Commands, so the file can list them all (commented out, with their defaults).
    void setCommands(const QList<Command>& commands);
    void ensureFile();

    // [keys]: what the file sets for a command, if anything.
    std::optional<QStringList> commandKeys(const QString& id) const;
    // Sets a command's keys in the file; nullopt goes back to the default.
    void setCommandKeys(const QString& id, const std::optional<QStringList>& keys);
    // From the old settings file ("keys/<id>" -> list): written by ensureFile.
    void setPendingImport(const QVariantMap& keys) { m_pendingImport = keys; }

    // [modal], merged with the defaults.
    const QList<Binding>& modalBindings() const { return m_modal; }
    static const QList<std::pair<QString, QString>>& modalDefaults();  // action -> keys
    static const QStringList& builtinActions();

    // The key as a token for matching: "j", "G", "Ctrl+D", "Space".
    static QString tokenFor(const QKeyEvent* e);
    QString modalTokenFor(const QKeyEvent* e) const;
    bool physicalModalKeys() const { return m_physicalModalKeys; }
    void setPhysicalModalKeys(bool on);
    static QString normalizeToken(const QString& t);
    // A readable name for an action (command titles come from the window).
    static std::function<QString(const QString&)> actionTitle;

signals:
    void changed();

private:
    Keymap();
    void watch();
    void rebuildModal(const QHash<QString, QStringList>& overrides);
    void writeFileText(const QString& text);
    void importOld(const QVariantMap& keys);

    QHash<QString, QStringList> m_commandKeys;
    QList<Binding> m_modal;
    QList<Command> m_commands;
    bool m_physicalModalKeys = false;
    QString m_error;
    QVariantMap m_pendingImport;
    QString m_written;  // the text last loaded or written, so the watcher skips no-change events
    QFileSystemWatcher* m_watcher = nullptr;
    QTimer* m_reload = nullptr;
};

}  // namespace tandem
