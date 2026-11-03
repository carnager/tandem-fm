#pragma once

#include <QColor>
#include <QFont>
#include <QList>
#include <QPair>
#include <QObject>
#include <QString>

class QApplication;
class QWidget;

namespace tandem {

struct Theme {
    QString id;         // file name without .json, e.g. "nord"
    QString name;       // shown in Settings
    bool dark = true;
    QColor window, base, altBase, text, dimText, accent, accentText;
    QColor mark, markBg, cursorActive, cursorInactive, dropTarget;
    QColor border, header, headerActive, error, success;
    QColor gitModified, gitAdded, gitUntracked;
    QColor control;     // borders of inputs, buttons and checkboxes: visible, unlike @border
    // File name colours by type ("files" in the file; see FileColors.h).
    QList<QPair<QString, QColor>> files;
    quint64 serial = 0;  // counts up with every applyTheme()
    QFont listFont;     // file list
    QFont numberFont;   // tabular digits for sizes and dates

    QString css(const QString& tmpl) const;  // replaces @window, @accent, ...
};

const Theme& theme();
// The configured fixed-width font, guaranteed to really be monospaced.
QFont monoFont();

// Themes are JSON files: built in (:/themes) and the user's own in
// themesFolder(). A file names the colours it changes; the rest come from
// Tandem's own dark or light theme. A user theme with a built-in's id
// replaces it.
struct ThemeInfo {
    QString id, name;
    bool dark = true;
    bool builtIn = true;
};
QList<ThemeInfo> availableThemes();
QString themesFolder();
// Reads a theme file. Unknown keys are ignored, so files made for newer
// versions still load. False (with *error) if it isn't a theme at all.
bool loadThemeFile(const QString& path, Theme& out, QString* error = nullptr);
// A theme as a file with every colour spelled out, to start editing from.
QByteArray themeJson(const Theme& t);

// Picks the theme from the settings (view/theme: "system" or a theme id;
// in "system" mode view/themeLight and view/themeDark) and applies it to the
// whole application. Call again after changing those settings: everything
// that is shown is restyled at once.
void applyTheme(QApplication& app);

// A style sheet with @colour names that follows theme changes.
void setThemeStyle(QWidget* w, const QString& tmpl);

// Emits changed() after a theme was applied, for widgets that keep colours
// beyond their style sheet.
class ThemeNotifier : public QObject {
    Q_OBJECT
public:
    static ThemeNotifier& instance();
signals:
    void changed();
};

}  // namespace tandem
