#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>

#include <functional>

class QProcess;

namespace tandem {

// Plugins are folders with a plugin.json: programs (any language) that
// Tandem runs on the selection or the shown folder. Each action becomes a
// command of its own, in the menus, the palette and the key settings.
// The format is described in docs/plugins.md.

constexpr int kPluginApi = 1;  // plugin.json "tandem": the newest version understood

struct PluginAction {
    QString id, title;
    QStringList run;       // argv; placeholders %s %f %n %d %D %c, see expandPluginArgs
    QStringList choices;   // optional argv that prints entries for a submenu
    bool forFolder = false;  // runs on the shown folder instead of the selection
    bool localOnly = true;   // not for servers and archives
    QStringList types;       // "image/*" (by name) or "*.pdf"; each selected item must match one
    QStringList keys;        // default keys, portable text ("Ctrl+Alt+P")
};

struct Plugin {
    QString id;           // folder name
    QString name, description, dir;
    QByteArray hash;      // of plugin.json: what the user trusted
    bool system = false;  // installed by a package, not in the user's folder
    QString error;        // set when it can't be used
    QList<PluginAction> actions;
};

QString pluginsFolder();  // the user's own: ~/.local/share/tandem-fm/plugins
// Reads every plugin folder (the user's, then /usr/share/tandem-fm/plugins
// and the like). Small local files, but still: call it on a worker.
QList<Plugin> discoverPlugins();
// Parses a manifest. Unknown keys are ignored so that newer plugins load;
// returns false with out.error for files that aren't usable.
bool parsePluginManifest(const QByteArray& json, Plugin& out);

// A plugin runs only after the user switched it on, and again only after
// that when its plugin.json changes.
enum class PluginTrust { Off, On, Changed };
PluginTrust pluginTrust(const Plugin& p);
void setPluginEnabled(const Plugin& p, bool on);

struct PluginContext {
    QStringList selection;  // marked items, or the one under the cursor
    QString current;        // under the cursor
    QString dir, otherDir;  // shown folders, this panel and the other
    bool local = true;      // a plain folder: not a server, not inside an archive
};
bool pluginApplies(const PluginAction& a, const PluginContext& ctx);
// Placeholders, without a shell: an argument "%s" becomes one argument per
// selected path; %f (under the cursor), %n (its name), %d, %D (folders) and
// %c (the chosen entry) are replaced inside arguments; %% is a percent sign.
// A program starting with ./ is taken from the plugin's folder.
QStringList expandPluginArgs(const Plugin& p, const QStringList& argv, const PluginContext& ctx,
                             const QString& choice = {});

// What a plugin asks of Tandem: lines of JSON on its output, e.g.
// {"message": "Sent"}, {"error": "…"}, {"refresh": true},
// {"select": ["a.txt"]}, {"cd": "/some/folder"}.
struct PluginRequest {
    QString message, error, cd;
    QStringList select;
    bool refresh = false;
};
bool parsePluginRequest(const QByteArray& line, PluginRequest& out);

// One run of an action. Deletes itself when done.
class PluginRun : public QObject {
    Q_OBJECT
public:
    PluginRun(const Plugin& p, const PluginAction& a, const PluginContext& ctx, const QString& choice,
              QObject* parent);
    void start();
signals:
    void request(const PluginRequest& r);
    // ok: exited with 0. text: its last line of plain output (or of errors).
    void finished(bool ok, const QString& text);

private:
    void readLines(bool all);

    QProcess* m_proc;
    QStringList m_argv;
    QString m_title;
    QByteArray m_buffer;
    QString m_lastLine;
    bool m_requestedMessage = false;
};

// Runs an action's "choices" program (a few seconds at most) and returns
// its entries: one per line, "label" or "label<TAB>value".
void listPluginChoices(const Plugin& p, const PluginAction& a, const PluginContext& ctx, QObject* context,
                       std::function<void(const QList<QPair<QString, QString>>& entries, const QString& error)> done);

}  // namespace tandem
