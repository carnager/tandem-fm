# Tandem action plugins, schema 1

Tandem's first plugin API follows Melody's external-program design: a plugin is a folder with `plugin.json`, and each action is a program receiving JSON on stdin. It can use any language or executable runtime you have installed. The app loads no plugin library, Python interpreter or JavaScript engine into its own process. Qt's JSON, process and widget tools provide the host; no additional runtime is needed for the bundled example.

This implementation was informed by Melody's `docs/plugins.md`, ADR-0288 (actions), ADR-0300 (settings), and its workspace and UI implementation in the local Melody checkout. The architecture and manifest conventions match, while the request context describes files and panels. Existing Melody music plugins need adaptation: Tandem does not supply albums, tracks, an audio engine, music providers, side panels, or automatic triggers in this version. This is the initial action-plugin API, not a promise of Melody plugin binary/source compatibility.

## Using plugins

1. Restart the updated Tandem.
2. Open **Plugins → Manage plugins…** or **Settings → Plugins**.
3. Tick **Selection tools** to enable the bundled read-only example.
4. Mark files, or place the cursor on one. Use **Plugins → Selection tools**, or its submenu in the file context menu, to copy paths/names or show a selection report.
5. Its **Settings…** button changes the copy separator and whether reports include modified dates.

Plugins are discovered but off by default. Enabling a plugin adds its actions to the Plugins menu and file context menus. The parent `..` row is never supplied as a selected item. An action's location and selection restrictions determine whether it is enabled. A running action has a nonmodal Cancel window; **Plugins → Cancel running plugins** stops all plugin runs. Ordinary file operations remain available meanwhile.

Programs run with your account's filesystem and network rights. This is process separation, not a sandbox. Only enable plugins you trust. File changes made by third-party programs do not enter Tandem's file-job queue or undo journal. The host never interprets a returned result as an instruction to copy, rename, delete, extract, mount or navigate. The bundled Selection tools plugin does not modify files.

## Installation and discovery

**Install folder…** copies a chosen plugin folder into the user's plugin directory. It stages the copy and publishes it with Tandem's exclusive native rename. Existing user plugins are never overwritten. An installation can be cancelled; failed/cancelled copies are removed. The original folder is not modified. Packages are limited to 5000 entries and 100 MB, and may contain regular files/directories only (no symbolic links, devices or FIFOs). Executable permissions are preserved.

A plugin folder name is its stable ID: 1–64 lowercase letters, digits, underscores or hyphens, starting with a letter. A readable `plugin.json` belongs directly inside it. To remove a plugin, disable it, open the plugin folder, and move its folder to Trash. Built-in files stay inside the app; leave those installed and disable them instead. Replacing a user plugin is deliberate: disable it and remove the old folder before installing its replacement.

The user root on macOS is:

```
~/Library/Application Support/tandem/plugins/
    my-tool/
        plugin.json
        main.py
```

On Linux the user root is `$XDG_DATA_HOME/tandem/plugins` (normally `~/.local/share/tandem/plugins`); system roots follow Qt's standard generic data locations. The user copy of an ID takes precedence over system/bundled copies, even when the user manifest is invalid, so a broken override cannot silently run a different copy. Manifest errors appear in the manager.

macOS bundles include `Contents/Resources/plugins`; Linux installations include `share/tandem/plugins` beside `bin`. `TANDEM_PLUGIN_PATH`, when set, replaces all discovery roots with a colon-separated list; an explicitly empty value disables discovery. This is useful while developing. Install still writes to the normal user root, so include that root in your override if you want to see those installations. Reload the manager after changing files. Menus are rebuilt when opened, and a changed manifest is refused by a run captured before that change.

Enabled IDs and nonsecret preferences are stored in your selected INI configuration and follow its sync behavior. Plugin program folders are separate from that file: install the appropriate programs/runtimes on each device, or use a shared discovery root deliberately. Secret settings stay in a local owner-only JSON file (`plugin-private/secrets.json` under Qt's application-local data location), outside the selected/synced config. The private directory is mode 0700 and the file mode 0600. Secret values are sent on stdin, never added to command-line arguments or environment variables by Tandem.

## Manifest

For example, `my-tool/plugin.json`:

```json
{
  "schema": 1,
  "name": "My file tool",
  "description": "Summarize the chosen files",
  "actions": [
    {
      "id": "summarize",
      "label": "Summarize selection",
      "for": "selection",
      "locations": ["local"],
      "run": ["python3", "./main.py"],
      "result": "notify",
      "timeout": 120
    }
  ],
  "settings": [
    {"key": "prefix", "label": "Report heading", "default": "My selection"}
  ]
}
```

| Key | Contract |
| --- | --- |
| `schema` | Integer 1. |
| `name` / `description` | Required display name (up to 128 characters), optional description (up to 4096). |
| `actions` | 1–64 entries. |
| `id` | Unique action ID using the same characters as a folder ID. |
| `label` | Menu label, up to 128 characters. |
| `for` | `selection` (default): one run with all selected entries; `files`: all must be nondirectories; `folders`: all must be directories; `always`: also works with no selection. |
| `locations` | Nonempty subset of `local`, `remote`, `archive`; defaults to `local`. Explicit support is required for virtual/remote paths. Native SMB volumes mounted under `/Volumes` are local filesystem paths. |
| `run` | Program followed by literal arguments, up to 128 strings. `./program` is inside the plugin folder and cannot escape it; an absolute executable path or a bare name in PATH is also allowed. Arguments have no automatic shell interpolation or path placeholders. |
| `result` | `notify` (default), `copy`, `notify-and-copy`, `none`, or `list`. |
| `timeout` | Whole seconds, 1–3600, default 120. |
| `settings` | Optional list of at most 64 setting definitions. |

Unknown keys, wrong types, duplicate action/setting IDs, unsupported schema versions and invalid limits make the manifest unusable. Manifests are limited to 256 KB. For a bare executable name the app's binary directory is searched before PATH, allowing the bundled `tandem-plugin-tools` helper to be found without a separate install. A third-party plugin may use an explicit interpreter in `run`; that interpreter must exist on the device. The current directory is the plugin's own folder.

### Settings

Settings use `key`, `label`, `kind` and an optional `default`:

```json
"settings": [
  {"key": "upload", "label": "Enable uploads", "kind": "toggle", "default": false},
  {"key": "server", "label": "Server", "default": "https://example.org",
   "requires": "upload", "help": "Your own server address", "link": "https://example.org/setup"},
  {"key": "token", "label": "Token", "kind": "secret", "requires": "upload"},
  {"key": "format", "label": "Format", "kind": "choice", "choices": ["text", "json"]}
]
```

Kinds are `text` (default), `secret` (masked input), `toggle` (boolean), and `choice` (one of 1–64 unique text choices). A secret cannot have a manifest default. Text defaults are empty, toggle defaults false, choice defaults its first option. A `requires` must name an earlier toggle: its dependent control is disabled until the toggle is on. The request still includes all settings; the program must respect the toggle. `help` is plain explanatory text. `link` is an optional credential-free HTTP/HTTPS setup page, opened only when the user clicks its button. Settings dialogs save on Save and discard edits on Cancel. Values are limited to 8192 characters.

## Input on stdin

The host writes one UTF-8 JSON object, then closes stdin:

```json
{
  "schema": 1,
  "app": "tandem",
  "action": "summarize",
  "active_panel": {"path": "/Users/me/Documents", "kind": "local", "is_local": true},
  "other_panel": {"path": "/Volumes/media", "kind": "local", "is_local": true},
  "selection": [
    {
      "name": "notes.txt",
      "path": "/Users/me/Documents/notes.txt",
      "uri": "file:///Users/me/Documents/notes.txt",
      "is_local": true,
      "is_directory": false,
      "is_link": false,
      "broken_link": false,
      "marked": true,
      "size": 120,
      "mode": 33188,
      "modified": 1793700000,
      "created": 1793600000,
      "added": null
    }
  ],
  "settings": {"prefix": "My selection"}
}
```

The selection is the context-menu selection override, else marked entries, else the cursor entry. It is captured when the menu opens; moving the cursor afterwards does not retarget that action. There is at most one run for the whole selection. At most 5000 entries and 4 MB of JSON are accepted. Names and paths are JSON strings, so quotes, whitespace, newlines and Unicode are never interpolated into shell commands. Display metadata comes from the existing listing; building a request does not stat/read every selected file or contact a server. Treat it as a snapshot and revalidate anything your program plans to change.

Date values are seconds since Unix epoch or `null` when unavailable. Unknown sizes are `null`; sizes are bytes. `mode` is the listing's POSIX mode. Links retain their listing metadata. The host does not automatically read contents, download remote selections, or extract archive entries for plugins.

For remote locations, `kind` is `remote` and paths/URIs are credential-free remote addresses; passwords, query strings and fragments are stripped. For an archive, `kind` is `archive`, the panel has an `archive` container path, and each selected entry includes `archive_path` inside it; `is_local` is false and there is no local `file:` URI for a virtual entry. A plugin must explicitly declare the locations it can handle. An absent other panel is an empty object.

The example's `main.py` could be:

```python
import json
import sys

request = json.load(sys.stdin)
heading = request["settings"]["prefix"]
lines = [heading] + [entry["path"] for entry in request["selection"]]
sys.stdout.write("\n".join(lines))
```

## Results and process lifecycle

Exit zero means success. UTF-8 stdout is the result:

- `notify`: show text in the panel's status message; long text also gets a plain-text result window.
- `copy`: copy exact stdout to the clipboard. Leading/trailing whitespace is preserved, including spaces belonging to file names. Use `sys.stdout.write` if your program should not append a newline.
- `notify-and-copy`: both.
- `none`: ignore the output; report completion.
- `list`: parse the structured response below and show a result window with Copy all.

Nonzero exit, crash, missing executable, changed manifest, output overflow, timeout and cancellation are failures. The host does not copy failure output to the clipboard. Stderr supplies the failure description, otherwise the host reports the exit/crash. Output is limited to 1 MB combined stdout/stderr. Input and secret values are not included in host-generated failure messages; a plugin should not echo secrets itself.

At most four actions run concurrently per window. They use asynchronous QProcess I/O, without blocking file-manager navigation. Cancel, timeout and app close stop the program and its Unix process group, including normal child processes. A program that deliberately daemonizes or escapes that process group cannot be managed by this contract and must not be used as a long-lived background service. Cancellation is not a rollback of writes made by an external program.

### Structured list

```json
{
  "title": "Selection details",
  "items": [
    {"text": "notes.txt", "detail": "120 bytes"},
    {"text": "Documentation", "url": "https://example.org"}
  ]
}
```

A list has at most 5000 items. `text` is required, `detail` and `url` are optional. Unknown keys or nonweb/credential-bearing links are rejected. The app draws plain native rows, without plugin-provided HTML, JavaScript, widgets or CSS. Double-clicking a linked row opens its HTTP/HTTPS page; no link is opened automatically.

## Extending the API later

This release deliberately establishes manually invoked actions first, as Melody did in ADR-0288. Dock panels, content/preview providers, automatic triggers, repositories/automatic updates and host-managed file mutation plans are future API additions. They must be versioned and integrated with Tandem's existing file-job and undo tools before claiming host-managed mutations.
