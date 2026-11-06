# Plugins

A plugin is a folder with a `plugin.json` that names programs for Tandem to
run on the selected files or the shown folder. The programs can be anything:
shell scripts, Python, a tool you already have. Each action becomes a Tandem
command: it appears in the context menu, under Tools › Plugins, in the
command palette (Ctrl+Shift+P) and on the Keys page, where it can get a key.
Actions for the selected files are also offered in **Open with** (Shift+Enter),
after the applications: with `"types": ["*.pdf"]`, an action such as "Open in
my PDF tool" sits next to the apps for PDF files.

## Where plugins live

- Your own: `~/.local/share/tandem-fm/plugins/<name>/plugin.json`
  (with `--config-dir`, in that folder's `plugins/`)
- Installed by packages: `/usr/share/tandem-fm/plugins/<name>/plugin.json`
  (and the other `XDG_DATA_DIRS`)

A plugin of your own replaces a package's plugin with the same folder name.

Plugins run with your rights, so none runs until you switch it on in
Settings › Plugins. If its `plugin.json` changes later, it is switched off
again until you check it and switch it back on.

## plugin.json

```json
{
  "tandem": 1,
  "name": "pocketlink",
  "description": "Send files to your phone",
  "actions": [
    {
      "title": "Send to phone",
      "run": ["pocketlink", "send", "-to", "%c", "%s"],
      "choices": ["pocketlink", "devices"]
    },
    {
      "title": "Make thumbnails here",
      "run": ["./thumbs.sh", "%d"],
      "on": "folder"
    }
  ]
}
```

Plugin fields:

| Field | |
|---|---|
| `tandem` | Plugin format version, `1`. Plugins for a newer format are not loaded. |
| `name`, `description` | Shown in Settings. |
| `actions` | The commands. |

Action fields:

| Field | |
|---|---|
| `title` | The command's name. |
| `id` | For key bindings; made from the title if left out. |
| `run` | The program and its arguments, as a list (no shell) or one string. A program starting with `./` is taken from the plugin's folder. |
| `on` | `"selection"` (default): the marked files, or the one under the cursor. `"folder"`: the shown folder, from the menu that opens beside the files. |
| `types` | Only for these files: MIME types (`"image/*"`, told by the name) or name patterns (`"*.pdf"`). Every selected item must match one. |
| `folders` | `false`: not offered when a folder is among the selected items. |
| `local` | `true` (default): not offered on servers or inside archives. |
| `choices` | A program that prints entries to choose from, one per line, as `label` or `label<TAB>value`. The action becomes a submenu; the chosen value is `%c`. It gets five seconds. |
| `keys` | Default keys, e.g. `["Ctrl+Alt+P"]`. |

Unknown fields are ignored, so plugins written for newer versions of Tandem
still load where they can.

## Placeholders

| | |
|---|---|
| `%s` | As a whole argument: one argument per selected path. |
| `%f` | The path under the cursor. |
| `%n` | Its name. |
| `%d` | The shown folder. |
| `%D` | The other panel's folder. |
| `%c` | The chosen entry (see `choices`). |
| `%%` | A percent sign. |

Programs start in the shown folder, with `TANDEM_PLUGIN_DIR` set to the
plugin's folder.

## Talking back

Plain output is fine: the last line is shown in the panel's status line. On
failure (exit code not 0) the last line of the error output is shown instead.

A line of JSON asks Tandem for something:

| | |
|---|---|
| `{"message": "Sent 3 files"}` | Show a message. |
| `{"error": "Phone not connected"}` | Show an error. |
| `{"refresh": true}` | Read the folder again. |
| `{"select": ["a.jpg", "b.jpg"]}` | Mark these files in the shown folder. |
| `{"cd": "/some/folder"}` | Go there. |

Several keys can be in one line; lines are handled as they arrive.
