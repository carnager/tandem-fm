#include "core/Workspaces.h"

#include <QDir>
#include <QSettings>

#include <algorithm>

namespace tandem {

namespace {

const QString kGroup = QStringLiteral("workspaces");

PanelTabs readTabs(QSettings& s, const QString& side)
{
    s.beginGroup(side);
    PanelTabs t;
    t.paths = s.value(QStringLiteral("tabs")).toStringList();
    for (const QVariant& v : s.value(QStringLiteral("tabSlots")).toList())
        t.numbers.append(v.toInt());
    for (const QVariant& v : s.value(QStringLiteral("tabGrids")).toList())
        t.grids.append(v.toBool());
    t.current = s.value(QStringLiteral("currentTab"), 0).toInt();
    s.endGroup();
    return t;
}

void writeTabs(QSettings& s, const QString& side, const PanelTabs& t)
{
    s.beginGroup(side);
    s.setValue(QStringLiteral("tabs"), t.paths);
    QVariantList numbers, grids;
    for (int n : t.numbers)
        numbers.append(n);
    for (bool g : t.grids)
        grids.append(g);
    s.setValue(QStringLiteral("tabSlots"), numbers);
    s.setValue(QStringLiteral("tabGrids"), grids);
    s.setValue(QStringLiteral("currentTab"), t.current);
    s.endGroup();
}

void writeAll(const QList<Workspace>& list)
{
    QSettings s;
    s.remove(kGroup);
    s.beginWriteArray(kGroup, int(list.size()));
    for (int i = 0; i < list.size(); ++i) {
        const Workspace& w = list[i];
        s.setArrayIndex(i);
        s.setValue(QStringLiteral("id"), w.id);
        s.setValue(QStringLiteral("name"), w.name);
        s.setValue(QStringLiteral("active"), w.rightActive ? QStringLiteral("right") : QStringLiteral("left"));
        writeTabs(s, QStringLiteral("left"), w.left);
        writeTabs(s, QStringLiteral("right"), w.right);
    }
    s.endArray();
}

}  // namespace

QList<Workspace> Workspaces::all()
{
    QSettings s;
    QList<Workspace> out;
    // Every [workspaces.N] table counts. Qt's array "size" isn't needed, so a
    // workspace written by hand without it isn't silently skipped.
    s.beginGroup(kGroup);
    for (const QString& group : s.childGroups()) {
        bool number = false;
        if (group.toInt(&number) <= 0 || !number)
            continue;
        s.beginGroup(group);
        Workspace w;
        w.id = s.value(QStringLiteral("id")).toInt();
        w.name = s.value(QStringLiteral("name")).toString();
        w.rightActive = s.value(QStringLiteral("active")).toString() == QLatin1String("right");
        w.left = readTabs(s, QStringLiteral("left"));
        w.right = readTabs(s, QStringLiteral("right"));
        s.endGroup();
        if (w.id > 0 && !w.name.isEmpty())
            out.append(w);
    }
    s.endGroup();
    std::sort(out.begin(), out.end(), [](const Workspace& a, const Workspace& b) {
        return QString::localeAwareCompare(a.name, b.name) < 0;
    });
    return out;
}

std::optional<Workspace> Workspaces::byId(int id)
{
    for (const Workspace& w : all())
        if (w.id == id)
            return w;
    return std::nullopt;
}

std::optional<Workspace> Workspaces::byName(const QString& name)
{
    for (const Workspace& w : all())
        if (w.name.compare(name.trimmed(), Qt::CaseInsensitive) == 0)
            return w;
    return std::nullopt;
}

int Workspaces::save(Workspace workspace)
{
    QList<Workspace> list = all();
    workspace.name = workspace.name.trimmed();
    if (workspace.id <= 0) {
        int top = 0;
        for (const Workspace& w : list)
            top = std::max(top, w.id);
        workspace.id = top + 1;
    }
    auto same = std::find_if(list.begin(), list.end(), [&](const Workspace& w) { return w.id == workspace.id; });
    if (same != list.end())
        *same = workspace;
    else
        list.append(workspace);
    writeAll(list);
    return workspace.id;
}

void Workspaces::remove(int id)
{
    QList<Workspace> list = all();
    list.removeIf([id](const Workspace& w) { return w.id == id; });
    writeAll(list);
    if (startupMode() == StartupMode::Workspace && startupWorkspace() == id)
        setStartup(StartupMode::Session);  // what it was before
}

bool Workspaces::rename(int id, const QString& name)
{
    const QString clean = name.trimmed();
    if (clean.isEmpty())
        return false;
    if (auto other = byName(clean); other && other->id != id)
        return false;
    std::optional<Workspace> w = byId(id);
    if (!w)
        return false;
    w->name = clean;
    save(*w);
    return true;
}

QString Workspaces::describe(const Workspace& w)
{
    const QString home = QDir::homePath();
    auto folders = [&](const PanelTabs& t) {
        QStringList out;
        for (QString p : t.paths) {
            if (p == home || p.startsWith(home + QLatin1Char('/')))
                p = QLatin1Char('~') + p.mid(home.size());
            out << p;
        }
        return out.join(QStringLiteral(", "));
    };
    return folders(w.left) + QStringLiteral("  │  ") + folders(w.right);
}

StartupMode Workspaces::startupMode()
{
    const QString mode = QSettings().value(QStringLiteral("startup/open")).toString();
    if (mode == QLatin1String("workspace"))
        return StartupMode::Workspace;
    if (mode == QLatin1String("home"))
        return StartupMode::Home;
    return StartupMode::Session;
}

int Workspaces::startupWorkspace()
{
    return QSettings().value(QStringLiteral("startup/workspace"), 0).toInt();
}

void Workspaces::setStartup(StartupMode mode, int workspaceId)
{
    QSettings s;
    switch (mode) {
    case StartupMode::Session:
        s.remove(QStringLiteral("startup"));  // the default
        return;
    case StartupMode::Workspace:
        s.setValue(QStringLiteral("startup/open"), QStringLiteral("workspace"));
        s.setValue(QStringLiteral("startup/workspace"), workspaceId);
        return;
    case StartupMode::Home:
        s.setValue(QStringLiteral("startup/open"), QStringLiteral("home"));
        s.remove(QStringLiteral("startup/workspace"));
        return;
    }
}

}  // namespace tandem
