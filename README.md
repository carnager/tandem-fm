# Tandem

A fast, keyboard-driven, two-panel file manager for Linux in the spirit of
Total Commander. Written from scratch in C++20 and Qt 6.

## Design goals

- **The UI never waits on the disk.** Every filesystem call (listing, stat,
  mkdir, rename, folder sizes) runs on worker threads. A dead NFS/SMB mount
  leaves one tab "loading" (Esc cancels); everything else keeps working.
- **Transfers run in the background** with their own threads, pause/resume,
  cancel, and conflict prompts that queue up instead of blocking.
- **Fast with huge folders.** Listings are read, stat'ed, given locale-aware
  natural sort keys and sorted off-thread; the view only paints visible rows.
- **Live updates without re-listing.** inotify reports *which* names changed,
  so only those rows are re-stat'ed (coalesced every 120 ms).
- **Use the kernel, don't reinvent it.** Reflinks (`FICLONE`) on btrfs/xfs,
  `copy_file_range` (server-side copy on NFS 4.2/SMB3), `AT_STATX_DONT_SYNC`
  for cached attributes on network filesystems.

## Build

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/tandem-fm [left-path] [right-path]
```

Requires Qt ≥ 6.5 (Core, Gui, Widgets, Test) and a C++20 compiler.

## Tests and benchmarks

```sh
ctest --test-dir build --output-on-failure        # core + UI (UI runs offscreen)
./build/tandem-bench                               # listing: 200k entries vs QDir
./build/tandem-bench --copy-latency FILE DESTDIR --dir SOME_BIG_DIR
                                                   # UI-thread stalls during a copy
```

Visual check without a display:
`QT_QPA_PLATFORM=offscreen TANDEM_SCREENSHOT=/tmp/shot.png ./build/tandem-fm`.
`TANDEM_THEME=dark|light` overrides the detected color scheme.

## Keys (Total Commander defaults)

| Key | Action |
|---|---|
| Tab | Switch panel |
| Enter / Backspace | Open / parent folder |
| Type letters | Quick search (↑/↓ next match, Esc close) |
| Insert, Space, Shift+↑/↓ | Mark (Space also computes folder size) |
| Num + / Num − / Num * | Mark group / unmark group / invert |
| F3 / F4 | Built-in viewer / edit (default app; set `tools/editor` in the config for F4) |
| F5 / F6 | Copy / move to the other panel (`name/` = into that folder) |
| Shift+F6 / Ctrl+M / Ctrl+E | Rename in place / multi-rename tool / rename in your text editor (vidir-style) |
| F7 / Shift+F4 | New folder (`a/b/c` creates nested folders) / new file |
| F8, Del / Shift+F8, Shift+Del | Move to trash / delete permanently |
| Ctrl+T / Ctrl+W / Ctrl+Tab | New / close / next tab (double-click a tab closes it) |
| Alt+← / Alt+→ | History back / forward (mouse back/forward buttons too) |
| Ctrl+PgUp, Ctrl+\\, Ctrl+\` | Parent, root, home |
| Ctrl+L | Type a path |
| Alt+F1 / Alt+F2 | Locations (mounts) for left / right panel |
| Ctrl+← / Ctrl+→ | Show folder under cursor in left / right panel |
| Ctrl+U, Alt+= | Swap panels, same folder in other panel |
| Ctrl+F3…F6 | Sort by name / extension / date / size |
| Ctrl+H, Ctrl+Shift+M | Hidden files, menu bar |
| Ctrl+C / X / V | Clipboard, interoperable with other file managers |
| Ctrl+Shift+C | Copy full paths as text |
| Alt+Shift+Enter | Compute all folder sizes |
| Ctrl+Alt+T | Open terminal here (`$TERMINAL`, or `tools/terminal` in the config) |
| Right click, Menu key, Shift+F10 | Context menu |
| Alt+F5 / Alt+F9 | Pack into a new archive / unpack archives to the other panel |
| Ctrl+PgDn | Enter folder, or browse any file as an archive |
| Esc | Cancel a slow folder load |

Drag and drop works within Tandem and with other applications (Shift = move).

Right-click menus: on files (Open with…, clipboard, copy/move to other panel,
rename, terminal, trash…), on empty space (new folder/file, paste, sort,
hidden files) and on the column header (show/hide columns). Right-clicking a
marked file acts on all marked files; an unmarked file is acted on alone and
your marks stay. Each tab remembers cursor and marks per folder, so Back,
Forward and re-entering a folder put you where you were.

## Archives

Enter on an archive browses into it like a folder; the path shows where you
are (`…/photos.tar.gz/2024/summer`), Backspace climbs back out. Formats are
whatever libarchive reads: zip, 7z, rar, tar (gz/bz2/xz/zst/lz4…), iso, cab,
cpio, deb, rpm, xar and single compressed files. The table of contents is
read once off the UI thread and cached, so moving around inside is instant.

Inside an archive: F3 and Enter work on a cached extracted copy, F5 extracts
(conflict prompts as usual), and folder sizes are shown right away.
Alt+F9 unpacks whole archives (into their own folder unless the archive has
a single top-level folder); Alt+F5 packs into a new archive whose format
follows the extension. Archives are read-only for now: deleting, renaming or
adding inside an existing archive comes later. Member paths with `..` and
writes through links the archive itself created are refused.

## Viewer (F3)

Text with syntax highlighting (KSyntaxHighlighting, optional; files up to 1 MiB),
text of any size (only the visible part is read, so multi-GB logs open
instantly), hex, and images (every format Qt and kimageformats support: AVIF,
HEIF, JXL, WebP, RAW, PSD…). All reads happen off the UI thread.

| Key | Action |
|---|---|
| Esc, Q | Close |
| N / P | Next / previous file in the folder |
| 1 / 3 / 4 | Text / hex / image |
| W | Wrap long lines |
| F7, Ctrl+F / F3, Shift+F3 | Find (case-insensitive) / next / previous |
| + / − / 0, Ctrl+wheel | Zoom (text size or image) / reset (image: fit) |

## Layout

```
src/core/   QtCore only, no UI: lister, inotify watcher, mounts, job engine
src/ui/     Widgets: model/view, panels, tabs, dialogs, jobs strip, main window
src/ui/viewer/  F3 viewer: block cache, large-text/hex/image views
tests/      test_core (engine), test_ui (drives the real window), test_viewer
bench/      tandem-bench
```

## Roadmap

1. ✅ Dual panels, tabs, keyboard flow, quick search, background F5–F8, watcher, benchmarks
2. ✅ Built-in viewer (F3) · ✅ archives as folders (libarchive) · multi-rename, compare/sync folders
3. Network backends (SFTP, SMB, WebDAV, S3), connection manager, button bar, command line
4. Plugins, themes, configurable keys
