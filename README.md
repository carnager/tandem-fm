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
- Command palette (Ctrl+Shift+P), every key changeable, optional Vim keys, Git status

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

## License

GPL-3.0-or-later
