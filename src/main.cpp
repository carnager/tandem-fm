#include "core/Backend.h"
#include "core/ConfigDir.h"
#include "core/DirWatcher.h"
#include "ui/AuthDialogs.h"
#include "ui/Network.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QIcon>
#include <QLibraryInfo>
#include <QLocale>
#include <QSettings>
#include <QTranslator>
#include <QTimer>

int main(int argc, char** argv)
{
    QCoreApplication::setApplicationName(QStringLiteral("tandem-fm"));
    QCoreApplication::setOrganizationName(QStringLiteral("tandem-fm"));
    QStringList paths;
    for (int i = 1; i < argc; ++i)
        paths << QString::fromLocal8Bit(argv[i]);
    tandem::setupConfigDir(paths);  // before anything reads settings

    QApplication app(argc, argv);
    QApplication::setApplicationDisplayName(QStringLiteral("Tandem"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QGuiApplication::setDesktopFileName(QStringLiteral("tandem-fm"));

    // UI language: TANDEM_LANG=de, else View > Language, else the system's.
    QString lang = qEnvironmentVariable("TANDEM_LANG");
    if (lang.isEmpty())
        lang = QSettings().value(QStringLiteral("view/language")).toString();
    const QLocale locale = lang.isEmpty() ? QLocale() : QLocale(lang);
    QTranslator qtTranslator, appTranslator;
    if (qtTranslator.load(locale, QStringLiteral("qtbase"), QStringLiteral("_"),
                          QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        QApplication::installTranslator(&qtTranslator);
    if (appTranslator.load(locale, QStringLiteral("tandem-fm"), QStringLiteral("_"), QStringLiteral(":/i18n")))
        QApplication::installTranslator(&appTranslator);

    tandem::applyTheme(app);
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("tandem-fm"), QIcon(QStringLiteral(":/icons/tandem-fm.svg"))));
    tandem::DirWatcher::instance();  // must be created on the UI thread
    tandem::registerBuiltinBackends();
    tandem::installAuthDialogs();
    tandem::Network::instance().recentServers();  // drops passwords older versions stored

    tandem::MainWindow w(paths);
    w.show();

    // Developer hook: TANDEM_SCREENSHOT=/path.png renders the window and quits
    // (handy with QT_QPA_PLATFORM=offscreen for visual checks).
    const QString shot = qEnvironmentVariable("TANDEM_SCREENSHOT");
    if (!shot.isEmpty()) {
        QTimer::singleShot(qEnvironmentVariableIntValue("TANDEM_SCREENSHOT_DELAY") ?: 1200, &w, [&w, shot] {
            w.grab().save(shot);
            QApplication::exit(0);
        });
    }
    return app.exec();
}
