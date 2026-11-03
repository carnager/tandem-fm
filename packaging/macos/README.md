# Tandem for macOS 0.1.1

An experimental macOS port of [carnager/tandem-fm](https://github.com/carnager/tandem-fm), based on upstream commit `f82cc2e955b9a58f066703f746661f29f00a6f36`. The upstream Qt 6 Widgets UI, libarchive, libssh, job queues, viewers, and fd/rg search tools are retained. macOS changes are guarded so the Linux implementations remain available.

## Changes in 0.1.1

- Left/Right navigation is enabled by default in the details view: Left opens the parent and Right enters a folder. Right never launches regular files. Toggle it in Settings → Behaviour; thumbnail grids retain normal arrow movement.
- Settings → Appearance switches Light, Dark, and system appearance immediately, including existing controls and dialogs. System mode follows desktop appearance changes.
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
- Native SFTP with the original libssh backend. Basic transfers and fallback traversal are tested against a temporary local sshd.

## Preview limits and follow-up

This is a development preview, not a guarantee of loss-free operation in every filesystem or failure scenario. Keep backups and first try it with disposable folders. Automated tests cover the cases recorded in VALIDATION.md; they do not establish power-loss recovery or resistance to concurrent malicious filesystem changes.

- No macOS Keychain integration yet: server passwords last for the session, using the existing fallback.
- The permissions editor exposes POSIX mode/owner/group only. Darwin ACL editing and BSD flags are not offered. Linux ACLs and flags remain available on Linux.
- GVfs, freedesktop notifications, and Linux desktop application discovery are unavailable on macOS. Custom commands still work; a native “Open with” application picker/default-association editor is pending.
- SMB via libsmbclient is optional and was not built or tested here. Finder-mounted SMB volumes can be browsed as local paths. Other GVfs protocols require an alternative integration.
- SFTP's GNU `find -printf` acceleration falls back to protocol traversal on BSD servers. Remote permissions/account editing still expects Linux server tools; it is not supported against macOS servers.
- Terminal commands that need an interactive terminal require configuring a compatible terminal command; the native default currently covers opening a folder in Terminal.
- Special APFS features (clones, snapshots), hard-link topology, sparse allocation, cross-volume metadata behavior, read-only volumes, low-disk-space errors, and network disconnect scenarios need more coverage. Directory extended attributes are tested; symlink metadata needs broader validation. External helpers and archives have their upstream preservation/security limits.
- Linux code paths are retained but were not executed on this Mac. An upstream contribution should follow Linux CI, broader filesystem testing, and maintainer review.

## Upstream work

The port is on `port/macos-v0.1`; `upstream` points at the original repository. Use `git diff upstream/main -- CMakeLists.txt src tests packaging/macos` to review the port independently of the workspace scaffold. No upstream branch or pull request has been published.
