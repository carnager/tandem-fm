#include "core/Launch.h"

#include <QMutex>
#include <QProcess>

namespace tandem {

namespace {
QMutex g_guardMutex;
LaunchGuard g_guard;
}  // namespace

void setLaunchGuardForTests(LaunchGuard guard)
{
    QMutexLocker lock(&g_guardMutex);
    g_guard = std::move(guard);
}

std::optional<bool> guardLaunch(const QString& program, const QStringList& args)
{
    LaunchGuard guard;
    {
        QMutexLocker lock(&g_guardMutex);
        guard = g_guard;
    }
    return guard ? guard(program, args) : std::nullopt;
}

bool launchDetached(const QString& program, const QStringList& args, const QString& workDir)
{
    if (const auto pretend = guardLaunch(program, args))
        return *pretend;
    return QProcess::startDetached(program, args, workDir);
}

}  // namespace tandem
