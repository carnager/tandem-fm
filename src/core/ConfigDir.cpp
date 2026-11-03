#include "core/ConfigDir.h"

#include <QDir>
#include <QSettings>

namespace tandem {

namespace {
QString g_dir;
}

void setupConfigDir(QStringList& args)
{
    QString dir = qEnvironmentVariable("TANDEM_CONFIG_DIR");
    for (int i = 0; i < args.size(); ++i) {
        if (args[i] == QLatin1String("--config-dir") && i + 1 < args.size()) {
            dir = args[i + 1];
            args.remove(i, 2);
            break;
        }
        if (args[i].startsWith(QLatin1String("--config-dir="))) {
            dir = args[i].mid(13);
            args.remove(i);
            break;
        }
    }
    useConfigDir(dir);
}

void useConfigDir(const QString& dirIn)
{
    QString dir = dirIn;
    if (dir == QLatin1String("~") || dir.startsWith(QLatin1String("~/")))
        dir = QDir::homePath() + dir.mid(1);
    g_dir = dir.isEmpty() ? QString() : QDir::cleanPath(QDir::current().absoluteFilePath(dir));
    if (g_dir.isEmpty()) {
        QSettings::setDefaultFormat(QSettings::NativeFormat);
        return;
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, g_dir);
}

QString configDir()
{
    return g_dir;
}

}  // namespace tandem
