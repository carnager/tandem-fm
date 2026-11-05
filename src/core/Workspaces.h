#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace tandem {

// One panel's tabs, as saved for the session or a workspace.
struct PanelTabs {
    QStringList paths;
    QList<int> numbers;  // tab numbers (Alt+1…9)
    QList<bool> grids;   // which tabs show thumbnails
    int current = 0;
};

// A named set of both panels' tabs, opened with Alt+W or its own command.
struct Workspace {
    int id = 0;  // stays when renamed: the command id, and with it its keys, uses it
    QString name;
    PanelTabs left, right;
    bool rightActive = false;
};

// What Tandem opens at start: the tabs from last time (the default), a
// workspace, or the home folder in both panels.
enum class StartupMode { Session, Workspace, Home };

// Kept in the settings file, under [workspaces], sorted by name.
namespace Workspaces {
QList<Workspace> all();
std::optional<Workspace> byId(int id);
std::optional<Workspace> byName(const QString& name);
// Replaces the one with the same id; a new one (id 0) gets an id. Returns the id.
int save(Workspace workspace);
void remove(int id);
// False when another workspace has that name already.
bool rename(int id, const QString& name);

// The folders, for lists: "~/Photos, ~/Code │ ~/Downloads".
QString describe(const Workspace& workspace);

StartupMode startupMode();
int startupWorkspace();  // its id, for StartupMode::Workspace
void setStartup(StartupMode mode, int workspaceId = 0);
}  // namespace Workspaces

}  // namespace tandem
