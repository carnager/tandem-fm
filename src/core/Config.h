#pragma once

#include <QObject>
#include <QMap>
#include <QMutex>
#include <QSettings>
#include <QVariant>
#include <optional>

class QFileSystemWatcher;
class QTimer;

namespace tandem {

// A per-process cache with atomic, per-key INI writes. The location selector
// stays in native settings on this machine; preferences are portable.
class Config : public QObject {
    Q_OBJECT
public:
    static Config& instance();
    static QString defaultFile();
    static QString localLockFile(const QString& file);
    static QString historyFileFor(const QString& file);
    QString fileName() const;
    QString historyFile() const;
    bool isCustom() const;
    bool initialize(const QString& requestedFile = {}, QString* error = nullptr);
    bool useFile(const QString& file, QString* error = nullptr, bool remember = true);
    QVariantMap snapshot();
    bool commit(const QMap<QString, std::optional<QVariant>>& changes);
    void checkForChanges();
    void reportError(const QString& message);

signals:
    void changed(const QStringList& keys);
    void locationChanged();
    void historyChanged();
    void storageError(const QString& message);

private:
    Config();
    void ensureLoaded(); // caller holds m_mutex
    void startWatching();
    void watchPaths();
    bool writePending(QString* error); // caller holds m_mutex
    mutable QMutex m_mutex;
    QString m_file;
    QVariantMap m_values;
    QMap<QString, std::optional<QVariant>> m_pending;
    bool m_allowCreate = true;
    QFileSystemWatcher* m_watch = nullptr;
    QTimer* m_timer = nullptr;
    QByteArray m_historyHash;
    QString m_lastError;
};

// The same small API used by the app's QSettings call sites. Writes are
// flushed when this scope ends and merged with external changes per key.
class Settings {
public:
    Settings();
    ~Settings();
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;
    QVariant value(const QString& key, const QVariant& fallback = {}) const;
    bool contains(const QString& key) const;
    void setValue(const QString& key, const QVariant& value);
    void remove(const QString& key);
    void clear();
    void beginGroup(const QString& group);
    void endGroup();
    QStringList allKeys() const;
    void sync();
    QString fileName() const;
private:
    QString fullKey(const QString& key) const;
    QVariantMap m_values;
    QMap<QString, std::optional<QVariant>> m_changes;
    QStringList m_groups;
    QString m_file;
};

} // namespace tandem
