#pragma once

#include <QSettings>
#include <QString>
#include <QVariant>

#include <optional>

namespace tandem {

// Settings as TOML files people can read and edit:
//   <config>/tandem-fm.toml   what the settings dialog changes; comments stay
//   <config>/keys.toml        key bindings (see Keymap)
//   <state>/state.toml        window size, columns, open tabs: Tandem's own
// Every QSettings() in the program reads and writes through this format.
QSettings::Format tomlFormat();

// Where window state goes: <configDir>/state.toml with --config-dir,
// otherwise $XDG_STATE_HOME/tandem-fm/state.toml.
QString stateFilePath();
// keys.toml, next to the settings file.
QString keysFilePath();
// Keys that are state, not settings ("window/geometry", "left/tabs"…).
bool isStateKey(const QString& key);

// The last problem reading a file ("tandem-fm.toml, line 12: …"), or empty.
QString tomlSettingsError();

// Moves settings from the old INI file (or the macOS plist) into the TOML
// files once. Key bindings from it are handed to keysOut ("keys/<id>").
void migrateOldSettings(QSettings::Format oldFormat, const QString& oldPath, QVariantMap* keysOut);

// ------------------------------------------------- TOML text, line by line

// A value as TOML text: true, 42, "text", ["a", "b"].
QString tomlValueText(const QVariant& v);
// Sets (or with no value, removes) table.key in TOML text, changing only
// that line. A commented-out "# key = …" line is reused; on removal,
// commentedDefault (if given) leaves "# key = default" in its place.
QString setTomlLine(const QString& text, const QString& table, const QString& key,
                    const std::optional<QString>& valueText, const QString& commentedDefault = {});
// Parses TOML into "table/sub/key" -> value. On error returns false and
// describes it (with the line) in *error.
bool parseToml(const QString& text, QVariantMap* out, QString* error = nullptr);

}  // namespace tandem
