# Themes

Settings → Appearance picks a theme, or one for light and one for dark mode
that follow the desktop. Own themes are JSON files in
`~/.local/share/tandem-fm/themes/`; Tandem applies them as soon as they are
saved. Only the colours you want to change are needed:

```json
{
  "name": "My theme",
  "dark": true,
  "colors": { "window": "#1e1e2e", "base": "#24273a", "text": "#cad3f5", "accent": "#f5a97f" }
}
```

"Copy to edit" in Settings writes the current theme with every colour as a
starting point. A file named like a shipped theme (`nord.json`) replaces it.

With *Color file names by type* switched on (Settings → Appearance), the
`files` section colours names: kinds (`executable`, `image`, `video`,
`audio`, `archive`, `document`, `folder`), name patterns and MIME types,
which come first:

```json
{
  "name": "My theme",
  "dark": true,
  "files": {
    "image": "#c4a7e7",
    "archive": "#eb6f92",
    "*.rs": "#f5a97f",
    "text/x-python": "#8aadf4"
  }
}
```

A theme without a `files` section gets colours made from its own.

With `--config-dir DIR`, your themes live in `DIR/tandem-fm/themes/` instead.

[Back to the README](../README.md)
