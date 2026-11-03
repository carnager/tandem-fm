# Tandem

A two-panel file manager for Linux, built for the keyboard. Total Commander
keys, modern tools, Qt 6.

## Features

- Tabs, quick search, filter as you type, bookmarks on letter keys, fuzzy jump to any visited folder
- Copy, move and delete in the background, queued per drive, with undo (Ctrl+Z)
- Built-in SFTP and SMB, other protocols through GVfs; logins can go in the desktop keyring
- Archives open like folders; zip, 7z and tar can be written to
- Viewer and side preview: text, hex, images, PDF, video, audio, fonts, Markdown
- Rename many files with a template and a live preview, or in your text editor
- Compare and synchronize folders, also between servers
- Permissions editor with ACLs and filesystem flags, locally and over SFTP
- Find duplicates, disk usage, checksums, links, trash, thumbnails
- Command palette (Ctrl+Shift+P), every key changeable, optional modal keys like vim and yazi, Git status
- Themes: Nord, Dracula, Gruvbox, Catppuccin, Solarized, Tokyo Night and more, or your own; can follow the desktop's light and dark mode; optional colours by file type
- Plugins: any program becomes a command on the selection, with menus to pick from ([docs/plugins.md](docs/plugins.md))

F1 lists all keys.

## Install

Arch Linux, from the `mooned` repository. Add to `/etc/pacman.conf`:

```
[mooned]
Server = https://repo.mooned.de/$arch
```

then `pacman -Sy tandem-fm-git`. The setup page at <https://repo.mooned.de>
has the signing key. Or build the package yourself:
`cd packaging/arch && makepkg -si`.

## Build

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/tandem-fm [left] [right]
```

Needs Qt 6.5 or newer (with QtPdf) and libarchive. Used when found: libssh 0.11 or newer (SFTP),
smbclient (SMB), GIO (GVfs), libsecret (keyring), libacl (ACLs),
KSyntaxHighlighting (syntax colours), qt6-tools (translations).
At run time it uses `ffmpeg` for video and audio, `fd` and `rg` for search
if installed.

Tests: `ctest --test-dir build`.

## Themes

Settings → Appearance picks a theme, or one for light and one for dark mode
that follow the desktop. Own themes are JSON files in
`~/.local/share/tandem-fm/themes/`; Tandem applies them as soon as they are
saved. Only the colours you want to change are needed:

```json
{
  "name": "My theme",
  "dark": true,
  "colors": { "window": "#1e1e2e", "base": "#24273a", "text": "#cad3f5", "accent": "#f5a97f" }
}
```

"Copy to edit" in Settings writes the current theme with every colour as a
starting point. A file named like a shipped theme (`nord.json`) replaces it.

With *Color file names by type* switched on (Settings → Appearance), the
`files` section colours names: kinds (`executable`, `image`, `video`,
`audio`, `archive`, `document`, `folder`), name patterns and MIME types,
which come first:

```json
"files": { "image": "#c4a7e7", "archive": "#eb6f92", "*.rs": "#f5a97f", "text/x-python": "#8aadf4" }
```

A theme without a `files` section gets colours made from its own.

## Settings in another folder

`tandem-fm --config-dir ~/Dropbox/tandem` (or `TANDEM_CONFIG_DIR=…`) keeps the
settings, bookmarks and folder history there instead of the usual places, as
plain INI files on every platform: a synced folder shares them between
computers. Changes from another computer apply at the next start.

## License

GPL-3.0-or-later
