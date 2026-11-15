#include "core/Launch.h"

#include <QMutex>
#include <QProcess>

namespace tandem {

namespace {
QMutex g_guardMutex;
LaunchGuard g_guard;
std::function<QString()> g_tokenProvider;
}  // namespace

void setActivationTokenProvider(std::function<QString()> provider)
{
    g_tokenProvider = std::move(provider);
}

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
    QProcess process;
    process.setProgram(program);
    process.setArguments(args);
    process.setWorkingDirectory(workDir);
    // Never pass on a token Tandem was started with: it is used up.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("XDG_ACTIVATION_TOKEN"));
    env.remove(QStringLiteral("DESKTOP_STARTUP_ID"));
    const QString token = g_tokenProvider ? g_tokenProvider() : QString();
    if (!token.isEmpty()) {
        env.insert(QStringLiteral("XDG_ACTIVATION_TOKEN"), token);
        env.insert(QStringLiteral("DESKTOP_STARTUP_ID"), token);  // older toolkits read this one
    }
    process.setProcessEnvironment(env);
    return process.startDetached();
}

}  // namespace tandem
