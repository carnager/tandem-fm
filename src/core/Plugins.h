#pragma once
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QVariant>
#include <atomic>
#include <optional>

namespace tandem {
struct PluginSetting {
    QString key, label, kind = QStringLiteral("text"), help, requiresToggle, link;
    QVariant fallback;
    QStringList choices;
};
struct PluginAction {
    QString id, label, target = QStringLiteral("selection"), result = QStringLiteral("notify");
    QStringList run, locations = {QStringLiteral("local")};
    int timeout = 120;
};
struct Plugin {
    QString id, name, description, folder;
    QByteArray manifestHash;
    QList<PluginAction> actions;
    QList<PluginSetting> settings;
};
struct FoundPlugin {
    QString id, folder, problem;
    std::optional<Plugin> plugin;
};
std::optional<Plugin> parsePluginManifest(const QByteArray& data, const QString& folder, QString* error);
QString userPluginFolder();
QStringList pluginFolders();
QList<FoundPlugin> discoverPlugins();
bool pluginEnabled(const QString& id);
void setPluginEnabled(const QString& id, bool enabled);
QString pluginSettingKey(const QString& id, const QString& key);
QVariant pluginSettingValue(const Plugin& plugin, const PluginSetting& setting);
QJsonObject pluginSettings(const Plugin& plugin);
// Secrets stay in an owner-only local file, outside the selected/synced config.
bool savePluginSettings(const Plugin& plugin, const QJsonObject& values, QString* error);
struct PluginInstallResult { QString id, error; };
// Blocking copy into a new directory; never overwrites an installed plugin.
PluginInstallResult installPluginFolder(const QString& source, const QString& destinationRoot,
                                        const std::atomic<bool>* cancel = nullptr);
struct PluginContext {
    QJsonObject active, other;
    QJsonArray selected;
    QString location = QStringLiteral("local");
    QString error;
};
bool pluginActionAvailable(const PluginAction& action, const PluginContext& context);
QByteArray pluginInput(const Plugin& plugin, const PluginAction& action, const PluginContext& context);
struct PluginListItem { QString text, detail, url; };
struct PluginList { QString title; QList<PluginListItem> items; };
std::optional<PluginList> parsePluginList(const QByteArray& data, QString* error);

// One asynchronous, bounded subprocess. No shell interpolation. Child processes
// share a separate process group on Unix, stopped on cancel/timeout/owner death.
class PluginRun : public QObject {
    Q_OBJECT
public:
    PluginRun(Plugin plugin, PluginAction action, QByteArray input, QObject* parent = nullptr);
    ~PluginRun() override;
    void start();
    void cancel();
    bool succeeded() const { return m_succeeded; }
    QString text() const { return m_text; }
    qint64 processId() const { return m_pid; }
signals:
    void finished();
private:
    void readOutput();
    void stop(const QString& error);
    void complete(bool success, const QString& text);
    void killGroup();
    Plugin m_plugin;
    PluginAction m_action;
    QByteArray m_input, m_stdout, m_stderr;
    QProcess m_process;
    qint64 m_pid = 0;
    bool m_started = false, m_done = false, m_succeeded = false;
    QString m_failure, m_text;
};
} // namespace tandem
