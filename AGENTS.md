# Working on Tandem

Rules for anyone changing this code, people and coding agents alike. They
come from real reviews: changes that ignored them were rejected or rewritten.
Read this before writing code.

## What Tandem is

A two-panel file manager for Linux, built for the keyboard: Total Commander
keys, modern tools, Qt 6 Widgets, C++20. Its first goal is to be fast and
never freeze, also with slow disks and servers. Linux is the main platform;
macOS builds, but its code must not change how Linux behaves.

## Before you start

- **Ask first** about new dependencies, changes to how or where settings,
  history or files are stored, and big changes to how something looks or is
  used. Describe the plan in a few lines and wait for an answer.
- **One feature per change.** No unrelated fixes, reformatting, renames or
  clean-ups mixed in. A stack of changes that each depend on the last is
  hard to review; keep them independent where you can.
- **Never bump the version** (CMakeLists.txt, main.cpp). The maintainer does
  that.
- **No personal notes in the repo**: no "validation" files, port diaries,
  private branch names, or docs about your own machine. Docs describe what
  users and contributors need, nothing else.

## Speed comes first

- **No disk or network I/O on the UI thread.** Not even a stat. Use
  `ioPool()->start(...)` and `postToUi(...)` (core/Util.h). Directory
  listings, discovery of plugins, checks for files: all on a worker.
- **No polling** to notice changes, unless there is no other way (servers
  and network mounts already are polled every 5 s, see DirTab). Watch with
  inotify/kqueue (`DirWatcher`), or react to signals.
- **No fsync** per file, no lock files, no "write a temp copy of everything
  on every change". Safety that costs every user speed needs the
  maintainer's yes first.
- **Nothing expensive in paint().** No QSettings, no QMimeDatabase without a
  cache, no file reads. Read settings once and keep them in a member or a
  static that the settings change updates.
- **Linux copy code is tuned** (`FileOps.cpp`: reflink, copy_file_range,
  temp file only when overwriting). Don't route it through slower paths.

## Defaults

- A new feature must not change what existing users see or how keys work.
  New options are **off by default** (or keep today's behaviour) unless the
  maintainer says otherwise.
- Don't move existing config, history or caches. If a location has to
  change, migrate once, silently and safely.

## Platform code

- Platform parts live in their own files picked by CMake
  (`DirWatcher_mac.cpp`, `Mounts_mac.cpp`, `MacSmb.cpp`), not in `#ifdef`
  blocks around Linux code. Small, local `#ifdef`s are fine when a whole
  file would be silly (a struct field name, one system call).
- Code in shared files must behave on Linux exactly as before.
- libsmbclient is not thread-safe here: every call goes through
  `CallLock calls(g_smbCalls)` (SmbBackend.cpp).

## How things are done here

- **Commands** go through `MainWindow::addCommand(...)`: that gives them an
  id, a place in the command palette, the Keys settings page and key
  rebinding. Don't add shortcuts any other way.
- **Colours** come from the theme (`theme().accent`, …). Style sheets use
  `setThemeStyle(widget, "color: @dimText; …")` so they follow theme
  changes live; never hard-code colours. Widgets that keep colours outside
  their style sheet listen to `ThemeNotifier::changed`.
- **Themes** are JSON files (data/themes, docs in README). New colour roles
  need a default for themes that don't name them.
- **Settings** use `QSettings()`; never build paths to the config file
  yourself (`--config-dir` must keep working). Tandem's own data goes to
  `~/.local/share/tandem-fm/` and `~/.cache/tandem-fm/`
  (GenericDataLocation/GenericCacheLocation + "tandem-fm"), not Qt's
  AppDataLocation, which doubles the name.
- **Plugins** are external programs described by a plugin.json
  (docs/plugins.md). Keep the format backwards compatible: new keys are
  optional, unknown keys are ignored.
- **Fonts and sizes**: use `theme().listFont`, `monoFont()`,
  `FileView::zoomed()`, `interfaceScale()`. Don't set fonts on every widget.
- **Comments and messages** are short, plain sentences that say why. UI
  texts are friendly and specific; no jargon.

## UX

- Modern, fast, pleasant beats copying Total Commander: inline instead of
  dialogs, live previews, few controls, keyboard first, readable syntax.
  Keep TC's keys and habits as the base; don't copy its dated dialogs.
- Secondary areas (preview, info, search) must look clearly different from
  the two file panels, never like a third panel.
- An embedded terminal was declined; don't propose it.

## Translations

Every user-visible string goes through `tr()` (or `QT_TR_NOOP` /
`QT_TRANSLATE_NOOP` where needed) and German must stay complete:

```sh
cmake --build build --target update_translations
grep -c 'type="unfinished"' translations/tandem-fm_de.ts   # must be 0
```

German uses "du", "Strg" for Ctrl, and the existing terms (Panel, Tab,
Lesezeichen, Farbschema, …).

## Tests

```sh
cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build --output-on-failure
```

- test_core (no UI), test_ui (the real window, offscreen), test_viewer.
- Every feature gets a test; every fixed bug gets one that failed before.
- Tests never touch the user's real keyring, trash, config or data: use
  `QTemporaryDir`, `useConfigDir()`, `setKeyringHooksForTests`, and restore
  environment variables you change (a test that left `XDG_DATA_DIRS`
  pointing nowhere broke later tests).
- No raw string literals (`R"(…)"`) in test files: moc can't read them.
- Check how it looks, without opening windows on anyone's desktop:
  `QT_QPA_PLATFORM=offscreen TANDEM_SCREENSHOT=/tmp/shot.png ./build/tandem-fm`
  (with `TANDEM_CONFIG_DIR=/tmp/somewhere` to keep real settings out).

## Commits

Small, one subject each, message in plain words: what changed for the user
and why. All tests pass, German is complete, nothing unrelated is in it.
