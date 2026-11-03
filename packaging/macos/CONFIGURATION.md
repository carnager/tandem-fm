# Portable configuration and folder sync

Tandem 0.1.2 and later can keep its preferences and bookmarks/history in a folder you sync with Dropbox or another file-sync service. Tandem saves files and reloads changes visible on the local disk; the sync service transfers them between computers.

## Set up in the GUI

1. Open **Settings → Configuration**.
2. Click **Choose folder…** and choose a dedicated Tandem folder inside your Dropbox folder (or another synced folder).
3. On every other computer running this version of Tandem, select that same synced folder.

A new location receives your current preferences and bookmarks. An existing INI profile is adopted without replacing it. Your previous files remain in their original location. **Use default location** returns to the existing default profile; it does not overwrite that profile with the shared one.

The location selector is stored only on this computer, allowing different Dropbox paths on different devices. Changing the location applies immediately. If you are joining an existing profile, reopen Settings to refresh its controls.

The folder contains:

- `settings.ini`: appearance, font scale, dimming, file-name color rules, shortcuts, behaviour, external tools, recent servers, command history, viewer/search/compare choices, and saved window/panel/session state.
- `folders.tsv`: bookmarks, bookmark keys, and visited-folder history.

You can instead choose a named INI file. For example, `work.ini` uses `work.folders.tsv` in the same folder. Sync both files. Caches, temporary transfers, SSH private keys/known-host files, and saved server passwords remain in their normal local locations. Command history and filesystem/server paths are included in the portable settings; commands and paths are copied as written and may need different values on different computers.

## Automatic saving and reload

Preference changes are saved when applied. Bookmarks/history are saved after a short delay (about two seconds); saved window geometry and panel sessions are written at exit or before switching profiles. Files are installed atomically, and process locks stay in the local cache rather than the cloud folder.

Incoming changes reload within approximately one second **after the sync service has delivered them locally**. Appearance, fonts, dimming, hidden files, Vim mode and shortcuts update while Tandem is open. Tool commands, recent servers, command history and dialog choices are read on their next use. Language, window/session state, and preview layout apply at the next launch so syncing cannot navigate your current panels or interrupt operations. Reopen Settings after an incoming change to refresh its controls.

External edits and atomic file replacements are supported. A missing, unreadable or malformed INI does not discard the last valid settings while Tandem is running. Local changes remain in memory and are retried when access returns; keep Tandem open until the folder is available and changes can be saved. A configured profile missing at startup produces an error instead of silently creating a replacement. Restore it or launch with an explicit new configuration path.

Independent local settings writes merge only the changed keys with the latest file visible on disk. Bookmarks/history merge incoming records with pending local changes. Dropbox cannot guarantee a single conflict-free result when multiple offline computers edit the same files: simultaneous edits to the same key are last-writer dependent, and any conflict copies created by the sync service need manual review. This release does not contact Dropbox or resolve its remote conflict copies.

## Command line and environment

These are optional alternatives to the GUI; explicit launch overrides do not change the saved location selector.

```sh
/Applications/Tandem.app/Contents/MacOS/tandem-fm --config-dir "$HOME/Dropbox/Tandem"
/Applications/Tandem.app/Contents/MacOS/tandem-fm --config "$HOME/Dropbox/Tandem/work.ini"
```

Or set `TANDEM_CONFIG_DIR` or `TANDEM_CONFIG_FILE` for the launched process. Precedence is command line, then environment (file before directory), then the location chosen in the GUI, then the platform's default application configuration folder. Do not pass both command-line options together.

An INI file can be edited directly, for example:

```ini
[view]
theme=dark
fontScale=150
inactiveDim=35

[keys]
lynxNavigation=true
```

Qt's INI encoding preserves typed values such as shortcut lists and binary window geometry; keep those encodings when editing them manually. On first launch, existing native preferences and the old folder history are copied into the new storage format. The native preferences remain as a backup. Other devices need a Tandem build that includes this portable-configuration support; the unmodified upstream app does not read these profiles.

## File-name colors (0.1.3)

Use **Settings → File colors → Add…**. Choose Extension, File type (MIME), or File kind, then pick light/dark text colors. The ordered rules are stored as JSON in `view/fileColors`; the master switch is `view/fileColorsEnabled`. These are part of the same portable INI profile and incoming changes repaint the panels immediately. No default rules are installed.
