#pragma once

#include "core/Launch.h"

#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QStandardPaths>
#include <QStringList>

// Tests never start real apps on the desktop: only shell tools, programs
// the test wrote into the temp folder, and programs that don't exist (they
// just fail) are run. Everything else (default apps, xdg-open, terminals,
// editors, diff tools) is recorded here instead and reported as started.
inline QStringList& launchedForTests()
{
    static QStringList launched;
    return launched;
}

inline QMutex& launchedMutex()
{
    static QMutex mutex;
    return mutex;
}

inline void installTestLaunchGuard()
{
    tandem::setLaunchGuardForTests([](const QString& program, const QStringList& args) -> std::optional<bool> {
        if (program == QLatin1String("sh") || program == QLatin1String("true") ||
            program.startsWith(QDir::tempPath() + QLatin1Char('/')))
            return std::nullopt;
        // A program that isn't there fails by itself, as the test may expect.
        if (program != QLatin1String("open-url") && QStandardPaths::findExecutable(program).isEmpty() &&
            !QFileInfo(program).isExecutable())
            return std::nullopt;
        QMutexLocker lock(&launchedMutex());
        launchedForTests() << (QStringList{program} + args).join(QLatin1Char(' '));
        return true;
    });
}
