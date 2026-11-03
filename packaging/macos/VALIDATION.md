# macOS 0.1.1 validation

Validated on November 3, 2026, on Apple Silicon (arm64), macOS 27.0, AppleClang 21, Qt 6.11.2, Homebrew libarchive 3.8.8, and libssh 0.12.1.

`./packaging/macos/build.sh` builds and runs CTest before bundling. The final CTest run passed all four executables (129 QtTest passes, 5 explicit skips, zero failures):

| Executable | Passes | Skips | Main coverage |
| --- | ---: | ---: | --- |
| test_macos | 3 | 0 | Temporary APFS disk image, actual cross-volume move, extended attributes, full-disk failed overwrite retaining both originals, temporary-file cleanup |
| test_core | 54 | 3 | Listing, copy/move/delete, interrupted overwrite, symlink descendant guard, exclusive rename over files/broken links, Unicode and metadata, directory attributes, archive read/write/update and traversal checks, rename/swap, checksums, duplicate/usage scans, Trash undo, search, compare, queues, localhost SFTP upload/download/reconnect |
| test_ui | 54 | 2 | Two-panel navigation and updates, F-key operations and prompts, cursor placement, tabs, rename/undo, bulk rename, editor integration, archive operations, permissions, preview and SFTP panels; Lynx navigation, live appearance/scale/dimming settings, cursor and marks during delayed refresh |
| test_viewer | 18 | 0 | Text/hex, images, search, large files, media, fonts, metadata and close behavior; live large-text/hex scaling preserving local zoom |

Counts include QtTest initialization and cleanup functions. UI/viewer automation uses Qt's offscreen backend. Syntax highlighting is optional and was absent: plain text behavior was asserted. SFTP tests launch a private sshd with temporary keys, rather than contacting an external server. GNU-only remote permissions/account and `find -printf` assertions are excluded for the local BSD server; protocol traversal is exercised instead.

Skips: freedesktop Trash browsing (core and UI), Linux libacl editing, the upstream queue test requiring two pre-existing drives, and GVfs localtest. The separate macOS disk-image test covers actual cross-volume move behavior; native Trash/undo is tested with generated files.

Not covered: another physical Mac, Intel, Linux execution, Developer ID/notarization, physical external drives/exFAT, native SMB, network disconnect recovery, power loss, adversarial path replacement races, every macOS privacy restriction, complete ACL/flags semantics, or all application-launch/association combinations. The source retains the upstream Linux paths; this log does not claim they were executed here.

The final bundle contains 74 Mach-O binaries. A recursive dependency audit found no missing libraries or external non-system library references. The bundle passed `codesign --verify --deep --strict` with a local ad-hoc signature, and launched with the native Cocoa backend, rendered a window, and exited successfully with Qt/DYLD override variables removed. This local smoke test does not replace validation on a clean second Mac.

For 0.1.1 the Lynx navigation and live appearance settings were also tested with the native Cocoa backend. Theme/scale tests cover existing controls and dialogs, explicitly bold fonts, widgets created while scaled, resetting to 100%, and switching themes without cumulative font growth. Inactive-panel tests verify focus switching, persistence, overlay bounds and transparent mouse handling. Native bundle smoke tests rendered both light and dark windows with Qt/DYLD overrides removed.
