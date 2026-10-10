# iotstore frontend

React 19 + Vite + TypeScript, no UI library. One build serves one store: `VITE_API_URL` is its API;
the store's name and whether sign-up is open come from the API (`/api/health`; `VITE_STORE_NAME` is
the name until it answers).

```sh
npm install
npm run dev          # :5310, API at VITE_API_URL (default http://localhost:9310)
VITE_API_URL=https://<your API> npm run build   # what ../scripts/deploy-web.sh does
npm run build        # dist/ (copies the engine first)
npm run engine       # copy the SDK's web/dist into public/iotdash again (after web/build.sh)
```

| page | file |
|---|---|
| sign in / create account | `src/pages/Login.tsx` |
| projects (create, search) | `src/pages/Projects.tsx` |
| one project: edit, delete, versions (upload, notes, publish, download, delete) | `src/pages/Project.tsx` |
| one version on a simulated board | `src/pages/Simulate.tsx` |
| the weighing-scale game | `src/components/ScaleGame.tsx` |

**Uploads** go in two steps (docs/API.md): the API hands out a form (S3 presigned POST, or the
local runner's), the browser posts the file there with progress, then asks the API to check it.
The package's header and manifest are read in the browser first (`src/lib/idbz.ts`), so a wrong
app or a version already uploaded is caught before anything is sent.

**The simulator** loads `public/iotdash/iotdash-web.js` (the SDK's `web/`): the board's engine
and OS in WebAssembly, drawing into an 800×480 canvas that takes mouse/touch and the keyboard.
The version's package is downloaded, verified (SHA-256 per file) and installed on a simulated card,
then the app starts. The side panels drive the simulated world:

- Wi-Fi: the saved access point in or out of range, internet on or off (the OS's internet check
  is answered by the page; the app's own HTTP requests go out from the browser, CORS applies).
- Bluetooth: the BLE scale `BLE-Scale-01` on or off; other nearby devices are listed.
- Scale: drag plastic, glass, paper and metal items from the shelf onto the plate. The plate is a
  damped spring (about 2 Hz, slightly under-damped), so the reading climbs, overshoots, wobbles and
  settles; the display shows `STABLE` after 0.35 s without motion, in 10 g steps. Every 200 ms the
  reading goes to the simulated scale, which sends it over BLE as the real one does
  (`ST,GS,+0001.25kg`). `TARE` / `ZERO` on the scale, or a tare sent by the app, set the net weight.
