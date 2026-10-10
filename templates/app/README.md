# {{title}}

An app for iotdash boards (ESP32-S3 7" touch panel, 800 × 480), id `{{id}}`.

```sh
iotdash run              # build, install and open it in the simulator
iotdash run --device     # ... on the board over USB
iotdash run --web        # ... in the browser (WebAssembly)
iotdash shot             # the simulator's rendering next to Chromium's (build/compare.png)
iotdash pack             # build/{{id}}-<version>.idbz, the package the store installs
iotdash publish --store https://…   # upload it to an iotstore
```

| file | |
|---|---|
| `app.json` | id, title (home screen), version (raise it for every release), description, `os.min` (oldest OS it runs on) |
| `icon.svg` / `icon.png` | home-screen icon |
| `main.html` | the first page; other pages are opened with `navigate('pages/x.html')` |
| `build/` | made by the commands; not source |

Everything a page uses must be in this folder (or `/system/…` from the OS, e.g. `/system/theme.css`).
The SDK's documentation: `docs/index.html` in the iotdash folder (`iotdash --version` shows where).
