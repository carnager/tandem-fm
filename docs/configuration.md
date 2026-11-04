# Settings and keys

Open Settings with **Ctrl+,** to change appearance, behaviour, external tools,
key bindings and plugins. Most changes apply immediately; a language change
takes effect after restarting Tandem.

Settings and key bindings are also plain TOML files you can edit yourself.
The default locations on Linux are:

- `~/.config/tandem-fm/tandem-fm.toml`: the settings. The settings dialog
  changes single lines and keeps the rest, comments included.
- `~/.config/tandem-fm/keys.toml`: the keys of every command, listed with
  their defaults (commented out), and the modal, yazi-like keys. Saving the
  file applies it right away; a mistake shows a message with the line.

```toml
[keys]
copy = "F5"
move-to-trash = ["F8", "Del"]
delete-permanently = []          # no key

[modal]
top = "g g"                      # a sequence
sort-by-size = ", s"
down = ["j", "Down"]
```

The paths above follow your XDG configuration directories.

Window size, columns and open tabs are Tandem's own business and live in
`~/.local/state/tandem-fm/state.toml`. Settings from older versions
(`tandem-fm.conf`) move over on the first start; the old file stays as
`tandem-fm.conf.bak`.

## Use another configuration folder

`tandem-fm --config-dir ~/Dropbox/tandem` (or `TANDEM_CONFIG_DIR=…`) keeps the
settings, keys, state, bookmarks and folder history there instead of the usual
places, on every platform: a synced folder shares them between computers.
Changes from another computer apply at the next start (keys right away).

The files sit together under `DIR/tandem-fm/` when you use `--config-dir DIR`.
This also puts custom themes and plugins beside the settings.

[Back to the README](../README.md)
