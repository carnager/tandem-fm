#pragma once
#include "ui/Theme.h"
#include <QList>
#include <optional>

namespace tandem {
struct ThemeColorRole {
    const char* key;
    const char* label;
    QColor Theme::*member;
    bool alpha = false;
};
struct ThemeProfile {
    QString id, name;
    Theme palette;
    bool builtIn = false;
};
const QList<ThemeColorRole>& themeColorRoles();
QList<ThemeProfile> themeProfiles(); // excludes automatic system appearance
std::optional<ThemeProfile> findThemeProfile(const QString& id);
ThemeProfile duplicateTheme(const ThemeProfile& source);
QString themeProfileError(const ThemeProfile& profile);
QByteArray exportTheme(const ThemeProfile& profile);
std::optional<ThemeProfile> importTheme(const QByteArray& data, QString* error = nullptr);
bool saveThemeProfile(const ThemeProfile& profile, QString* error = nullptr);
bool removeThemeProfile(const QString& id);
void selectTheme(const QString& id);
} // namespace tandem
