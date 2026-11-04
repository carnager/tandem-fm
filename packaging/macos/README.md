# Tandem on macOS (experimental)

Linux is Tandem's main platform. The macOS build works, but it is tested by
contributors on their Macs, not by the maintainer.

## Build

With the Xcode command line tools and Homebrew:

```sh
brew install cmake ninja qt libarchive libssh tomlplusplus
brew install fd ripgrep ffmpeg   # optional, as on Linux
./packaging/macos/build.sh
```

The script builds, runs the tests, bundles Qt and the libraries into
`dist/macos-<date>/Tandem.app`, signs it ad hoc and zips it. The app is not
notarized, so the first start needs a right click → Open.

## What differs from Linux

- Folder changes come from kqueue. It sees files appear and disappear, but not
  files that change in place, so shown folders are also re-read every 5 seconds.
- Copies keep Finder metadata (tags, extended attributes, ACLs).
- With the system icon theme, the file panels use native macOS type icons and
  Finder's icons for local files and folders, including custom icons. Icons
  load in the background; servers and archives use type icons without looking
  up individual paths.
- SMB shares (`smb://server/share/folder`) are mounted by macOS, with its own
  login dialog and Keychain, and browsed under `/Volumes` like any folder.
  Finder sees them too. SFTP is built in, as on Linux. GVfs protocols
  (WebDAV, FTP, MTP …) are not available.
- The trash window opens the Trash in Finder; deleting to the trash and undo
  work as on Linux.
- "Open terminal here" opens Terminal.app unless another terminal is set.
- Not available: desktop notifications, the system keyring for SFTP passwords,
  ACL and file flag editing, "Open with" app lists (opening files with their
  default app works).
