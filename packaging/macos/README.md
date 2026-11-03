# Tandem for macOS 0.1.5

An experimental macOS port of [carnager/tandem-fm](https://github.com/carnager/tandem-fm), based on upstream commit `f82cc2e955b9a58f066703f746661f29f00a6f36`. The upstream Qt 6 Widgets UI, libarchive, libssh, job queues, viewers, and fd/rg search tools are retained. macOS changes are guarded so the Linux implementations remain available.

## Changes in 0.1.5

Right-click a detail-table heading to enable **Date Created** and **Date Added** independently in each panel. Both are optional and initially hidden, preserving existing layouts. Click a date heading to sort, or choose **Sort by date created** / **Sort by date added** in the Sort menu. New date sorts start newest first; selecting the same sort again reverses the order. Existing modified-date shortcuts remain unchanged. Column choices and sort settings persist in the selected configuration profile.

Date Created is the filesystem birth time (macOS `stat`, Linux `statx` when available). Date Added is the macOS filesystem's added-to-folder timestamp, read through `getattrlistat`; it is not inferred from creation, modification, access, or download times. Links show their own dates. Missing/unsupported values stay blank and sort after known dates in either direction, while folders and the parent entry retain their usual priority. Remote protocols, archives, and mounted filesystems may not provide these dates. Metadata-only date changes participate in background refresh detection. The existing narrow-panel layout can temporarily hide date columns to keep names readable; widening the panel restores them.

## Changes in 0.1.4

SMB connections on macOS now use Apple's NetFS service and native SMB filesystem. In **Go → Connect to server…**, enter `smb://server/share` (optionally followed by a subfolder), or `smb://server` to use the system share chooser. macOS handles login and share selection. Mounted folders open in Tandem and remain available to Finder and other apps. If several shares are selected, the first opens in the panel; the rest are available in Locations.

Existing shares are identified by their actual remount URL (server, share, port and requested account), avoiding guesses from `/Volumes` names. The read-only local probe successfully reused the existing media mount. Credentials are passed directly to NetFS and stripped from saved recent addresses and status text. Native mounts run asynchronously, with cancellation/error/timeout handling. Navigating away prevents a late connection result from taking over the panel. SMB disconnect uses normal macOS `diskutil unmount`, without force; an in-use volume can be refused by the OS.

The Linux GIO/libsmbclient and cross-platform libssh paths are retained. A fresh NAS login and live SMB write/disconnect tests have not been performed against the user's share; native login outcomes still depend on the server, permissions and macOS authentication.

## Changes in 0.1.3

Settings → File colors adds ordered file-name color rules, with a color picker or hex value for each light/dark theme. Match extensions (including lists and compound suffixes), MIME types such as `image/*`, or kinds such as folders, links and executables. Rules apply immediately in both detail panels and thumbnail grids and travel with your portable settings. An Appearance-page button also opens this page.

The first enabled match wins. Extension matching ignores case and accepts `pdf`, `.pdf`, or `*.pdf`, with multiple extensions separated by commas, semicolons or spaces. MIME matching uses Qt's filename database (including aliases/parent types); it does not open files or access remote servers. Put specific rules above broad ones. Marked files and broken links retain their mark/error colors; the parent entry and metadata columns retain normal theme styling. Custom rules override ordinary hidden/Git-ignored name dimming, and the inactive-panel dimming option still applies. Turn rules off individually or use the master checkbox to restore normal colors. Choose colors visible against each theme's background; set both theme colors to the same value if desired.

## Changes in 0.1.2

Settings → Configuration lets you choose a synced folder or named INI file, migrate existing preferences, and reload incoming settings automatically. Bookmarks/history live beside the INI. See [portable configuration setup and behaviour](CONFIGURATION.md), including command-line/environment overrides and cloud-conflict limitations.

## Changes in 0.1.1

- Left/Right navigation is enabled by default in the details view: Left opens the parent and Right enters a folder. Right never launches regular files. Toggle it in Settings → Behaviour; thumbnail grids retain normal arrow movement.
- Settings → Appearance switches Light, Dark, and system appearance immediately, including existing controls and dialogs. System mode follows desktop appearance changes.
- Settings → Appearance → Text size scales application fonts from 75% to 200%, including file lists, dialogs, text viewers, and large text/hex metrics, without restarting.
- Settings → Appearance → Dim inactive panel fades the inactive panel towards the theme background. Default 25%; 0% disables it, maximum 70%. The overlay does not intercept clicks or drags.
- Directory notifications refresh only affected macOS panels; background refreshes preserve cursor/marks changed while listing was in progress.

## Build and package

On an Apple Silicon or Intel Mac with Xcode Command Line Tools and Homebrew:

```sh
brew install cmake ninja qt libarchive libssh
# Optional runtime tools, identical to Linux:
brew install fd ripgrep ffmpeg
./packaging/macos/build.sh
```

The script builds for the current machine, runs all tests, creates a dated directory under `dist/`, bundles Qt and linked libraries into `Tandem.app`, completes Homebrew plugin dependency relocation, verifies that linked libraries are self-contained, applies a local ad-hoc signature, verifies it, and creates a ZIP. Use Homebrew libarchive explicitly: Apple's system library can have different format support. Optional tools such as ffmpeg/fd/rg are discovered in PATH, including the standard Homebrew paths when launched from Finder; they are not bundled.

Open `Tandem.app` from its output folder or copy it into Applications. This preview is not Developer ID signed or notarized. It has not been verified on another Mac or on Intel. Build from source on Intel; the Apple Silicon package is not universal.

## Implemented

- The original two-panel Qt interface, tabs, filtering, bookmarks, jobs, shortcuts, viewers, archives, checksums, comparisons, and rename tools.
- Local listing through POSIX metadata, kqueue directory notifications, and asynchronous polling for file-content changes (about one second for visible panels).
- Atomic exclusive renames with macOS `RENAME_EXCL`. Unsupported exclusive renames fail instead of falling back to a racy overwrite.
- Regular-file copies stage data beside the destination, copy macOS metadata (including extended attributes/resource forks and ACLs), flush and close before installation, and keep the previous destination when canceled or when copying fails. Concurrent destination changes detected before installation abort the replacement.
- Copying a directory into itself through a symlink is rejected. Symlinks are copied as links by the existing engine.
- Qt's native move-to-Trash and session undo. Trash browsing opens Finder; its private restore metadata is never parsed or rewritten.
- Mounted volumes under `/Volumes`, including network mounts. Default file opening uses Qt/macOS associations. “Open terminal here” opens Terminal when no custom terminal is set.
- Native macOS SMB through NetFS, including reuse of existing mounts and subfolder navigation.
- Native SFTP with the original libssh backend. Basic transfers and fallback traversal are tested against a temporary local sshd.

## Preview limits and follow-up

This is a development preview, not a guarantee of loss-free operation in every filesystem or failure scenario. Keep backups and first try it with disposable folders. Automated tests cover the cases recorded in VALIDATION.md; they do not establish power-loss recovery or resistance to concurrent malicious filesystem changes.

- SFTP passwords use the existing session fallback. SMB authentication and its credential UI are managed by macOS.
- The permissions editor exposes POSIX mode/owner/group only. Darwin ACL editing and BSD flags are not offered. Linux ACLs and flags remain available on Linux.
- GVfs, freedesktop notifications, and Linux desktop application discovery are unavailable on macOS. Custom commands still work; a native “Open with” application picker/default-association editor is pending.
- SMB on macOS uses NetFS and mounted filesystem paths. Other GVfs protocols require an alternative integration. SMB-specific write/metadata and disconnect recovery need further live-server coverage.
- SFTP's GNU `find -printf` acceleration falls back to protocol traversal on BSD servers. Remote permissions/account editing still expects Linux server tools; it is not supported against macOS servers.
- Terminal commands that need an interactive terminal require configuring a compatible terminal command; the native default currently covers opening a folder in Terminal.
- Special APFS features (clones, snapshots), hard-link topology, sparse allocation, cross-volume metadata behavior, read-only volumes, low-disk-space errors, and network disconnect scenarios need more coverage. Directory extended attributes are tested; symlink metadata needs broader validation. External helpers and archives have their upstream preservation/security limits.
- Linux code paths are retained but were not executed on this Mac. An upstream contribution should follow Linux CI, broader filesystem testing, and maintainer review.

## Upstream work

The port is on `port/macos-v0.1`; `upstream` points at the original repository. Use `git diff upstream/main -- CMakeLists.txt src tests packaging/macos` to review the port independently of the workspace scaffold. No upstream branch or pull request has been published.
