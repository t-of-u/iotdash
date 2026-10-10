// Copy the SDK's engine for the browser (iotdash/web/dist, made by web/build.sh) into
// public/iotdash/, where the simulator page loads it. --if-missing: only when it is not there yet.
import { cpSync, existsSync, mkdirSync, rmSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const src = resolve(process.env.IOTDASH_WEB || resolve(here, '../../../web/dist'));
const out = resolve(here, '../public/iotdash');
if (process.argv.includes('--if-missing') && existsSync(resolve(out, 'iotdash.wasm'))) process.exit(0);
if (!existsSync(resolve(src, 'iotdash.wasm'))) {
  console.warn(`no engine in ${src}: run web/build.sh in the SDK (the simulator page will not work)`);
  process.exit(0);
}
rmSync(out, { recursive: true, force: true });
mkdirSync(out, { recursive: true });
for (const f of ['iotdash.js', 'iotdash.wasm', 'iotdash-web.js', 'system']) cpSync(resolve(src, f), resolve(out, f), { recursive: true });
console.log(`engine: ${src} -> public/iotdash`);
