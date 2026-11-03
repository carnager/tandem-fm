# Themes and custom colors

Choose **Theme → Nord** to use the included Nord palette immediately. It adapts the [Nord color palette](https://www.nordtheme.com/docs/colors-and-palettes/) to Tandem's controls and file panels. Dark, Light and desktop appearance remain available.

To make a custom theme:

1. Open **Theme → Add / manage themes…** (also available from Settings → Appearance).
2. Select Nord, Dark, Light, or an existing custom theme, then click **Add custom…**.
3. Name your copy. Edit a color with its picker or hex input. The row preview updates without changing the application.
4. Toggle **Alternate row backgrounds** and, optionally, **Use a separate selection text color**.
5. Click **Save and use**. Changes apply immediately to panels and open dialogs. Cancel discards editor changes.

Built-in palettes cannot be overwritten. To revise a custom palette, select it and click **Edit…**. Import adds a new named custom palette and selects it; a duplicate name gets a numeric suffix. Export writes a separate JSON file through an atomic save. Removing the active custom theme returns to desktop appearance.

## Color precedence

Normal and alternate rows use the profile's background colors. The mark background overlays a marked row; the current row's selection overlays that. Semi-transparent colors blend with the underlying row. Inactive-panel dimming is still controlled separately in Settings → Appearance.

File/extension rules are available in **Theme → File and extension colors…** and Settings → File colors. They remain shared across themes: dark profiles use the rules' dark colors; light profiles use the light colors. An enabled matching rule overrides the default row/extension text. Marked entries keep their mark text; broken links keep their error text. Optional selection text overrides ordinary/rule text on the active current row, while marks and broken links retain priority. Secondary metadata uses secondary text; thumbnails use the same mark/selection precedence. The default extension text color affects the detail table's extension column; filename rules color both names and extensions.

## Saving and syncing

Each custom palette is stored under its own `themes/custom-<UUID>` key in the active INI configuration. `view/theme` stores the selected profile. These use the same atomic writes and live configuration reload as other settings. Incoming palette changes repaint existing panels without changing directories or interrupting operations. Different profiles are separate settings keys; simultaneous edits to the same profile follow the existing configuration's last-write/conflict behavior. No cloud service is contacted by theme editing itself.

## JSON format

Use Export to obtain a complete, portable palette. Import accepts UTF-8 JSON up to 256 KB with `format: "tandem-theme"`, `version: 1`, a 1–80 character name, a boolean `dark`, and a `colors` object. Optional boolean options are `alternatingRows` (default true) and `selectionTextEnabled` (default false).

Colors use `#RRGGBB`. Selection backgrounds (`cursorActive`, `cursorInactive`), mark background (`markBg`), and drop highlight (`dropTarget`) also accept `#AARRGGBB`, where AA is opacity: 00 is transparent, FF opaque. Other roles must be opaque. Unknown color roles, invalid colors and unsupported versions are rejected. Imported files contain color data only; they cannot supply scripts, CSS, paths or plugins.

Supported roles: `window`, `base`, `altBase`, `text`, `extensionText`, `dimText`, `accent`, `accentText`, `cursorActive`, `cursorInactive`, `selectionText`, `mark`, `markBg`, `dropTarget`, `header`, `headerActive`, `border`, `control`, `error`, `success`, `gitModified`, `gitAdded`, `gitUntracked`.

A partial theme fills omitted roles from Tandem's default Dark or Light palette, according to `dark`. For example:

```json
{
  "format": "tandem-theme",
  "version": 1,
  "name": "My blue theme",
  "dark": true,
  "alternatingRows": true,
  "selectionTextEnabled": false,
  "colors": {
    "base": "#182432",
    "altBase": "#203044",
    "extensionText": "#88c0d0",
    "cursorActive": "#805e81ac",
    "mark": "#ebcb8b"
  }
}
```

Choose foreground/background combinations you can read comfortably. Tandem validates color syntax and opacity; it does not enforce a contrast ratio.
