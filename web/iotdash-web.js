// iotdash in a web page: the board's OS and an app package (.idbz) running in a <canvas>.
//
//   import { IotdashSim, parseIdbz } from './iotdash-web.js';
//   const sim = new IotdashSim({ canvas, base: '/iotdash/' });   // base: where web/dist is served
//   await sim.init();
//   const app = sim.installPackage(await (await fetch(url)).arrayBuffer());
//   sim.boot();
//   sim.launch(app.id);
//   sim.setInternet(false); sim.setWifiReach(true);
//   sim.scale({ kg: 1.25, stable: true });   // what the simulated BLE scale shows
//
// The engine runs in WebAssembly (iotdash.js / iotdash.wasm from web/build.sh); this file draws
// its frames, feeds it touch and keyboard input, carries its HTTP requests over fetch() and
// drives the simulated world (sim/fake_radio.h: Wi-Fi access points, a BLE weighing scale).

export const W = 800, H = 480;

// ---- IDBZ packages (iotdash/tools/idbz.py): nothing compressed, files are slices
export function parseIdbz(buffer) {
  const bytes = new Uint8Array(buffer);
  const dv = new DataView(buffer);
  const magic = String.fromCharCode(...bytes.subarray(0, 4));
  if (magic !== 'IDBZ') throw new Error('not an IDBZ package');
  const version = dv.getUint16(4, true), count = dv.getUint16(6, true);
  if (version !== 1) throw new Error(`IDBZ format ${version} is not supported`);
  const moff = dv.getUint32(8, true), mlen = dv.getUint32(12, true), total = dv.getUint32(16, true);
  if (total !== bytes.length) throw new Error(`package size ${bytes.length}, header says ${total}`);
  const manifest = JSON.parse(new TextDecoder().decode(bytes.subarray(moff, moff + mlen)));
  const files = {}, hashes = {};
  for (let i = 0; i < count; i++) {
    const base = 64 + 64 * i;
    let end = base;
    while (end < base + 24 && bytes[end]) end++;
    const name = new TextDecoder().decode(bytes.subarray(base, end));
    const off = dv.getUint32(base + 24, true), size = dv.getUint32(base + 28, true);
    files[name] = bytes.subarray(off, off + size);
    hashes[name] = bytes.subarray(base + 32, base + 64);
  }
  for (const n of ['app.json', 'app.idb', 'icon.img'])
    if (!files[n]) throw new Error(`the package has no ${n}`);
  return { manifest, files, hashes };
}

// SHA-256 in plain JS: pages served over plain HTTP have no crypto.subtle (secure contexts only)
const K256 = new Uint32Array([
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
  0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
  0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
  0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
  0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2]);
const rotr = (x, n) => (x >>> n) | (x << (32 - n));
export function sha256(bytes) {
  const len = bytes.length, total = ((len + 9 + 63) >> 6) << 6;
  const buf = new Uint8Array(total);
  buf.set(bytes);
  buf[len] = 0x80;
  const dv = new DataView(buf.buffer);
  dv.setUint32(total - 8, Math.floor(len / 0x20000000));  // the length in bits, big-endian
  dv.setUint32(total - 4, (len << 3) >>> 0);
  const H = new Uint32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
  const w = new Uint32Array(64);
  for (let off = 0; off < total; off += 64) {
    for (let i = 0; i < 16; i++) w[i] = dv.getUint32(off + i * 4);
    for (let i = 16; i < 64; i++) {
      const a = w[i - 15], b = w[i - 2];
      w[i] = w[i - 16] + (rotr(a, 7) ^ rotr(a, 18) ^ (a >>> 3)) + w[i - 7] + (rotr(b, 17) ^ rotr(b, 19) ^ (b >>> 10));
    }
    let [a, b, c, d, e, f, g, h] = H;
    for (let i = 0; i < 64; i++) {
      const t1 = (h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i]) | 0;
      const t2 = ((rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c))) | 0;
      h = g; g = f; f = e; e = (d + t1) | 0; d = c; c = b; b = a; a = (t1 + t2) | 0;
    }
    H[0] += a; H[1] += b; H[2] += c; H[3] += d; H[4] += e; H[5] += f; H[6] += g; H[7] += h;
  }
  const out = new Uint8Array(32);
  const odv = new DataView(out.buffer);
  for (let i = 0; i < 8; i++) odv.setUint32(i * 4, H[i]);
  return out;
}

// checks every file against the SHA-256 in the package table
export async function verifyIdbz(pkg) {
  const subtle = globalThis.crypto && globalThis.crypto.subtle;
  for (const [name, data] of Object.entries(pkg.files)) {
    const h = subtle ? new Uint8Array(await subtle.digest('SHA-256', data)) : sha256(data);
    if (!h.every((b, i) => b === pkg.hashes[name][i])) throw new Error(`${name}: checksum mismatch`);
  }
  return true;
}

// ---- the simulator
const KEYS = new Set(['Backspace', 'Enter', 'Tab', 'Escape', 'Delete', 'ArrowLeft', 'ArrowRight', 'ArrowUp',
  'ArrowDown', 'Home', 'End', 'PageUp', 'PageDown']);
// TransitionKind (lib/rt/engine.h): none, slide left/right/up/down, fade
const T_LEFT = 1, T_RIGHT = 2, T_UP = 3, T_DOWN = 4, T_FADE = 5;

export class IotdashSim {
  constructor({ canvas, base = './', version = '', onLog, onRadio, onTare, fetch: fetchFn } = {}) {
    this.canvas = canvas;
    this.base = base.endsWith('/') ? base : base + '/';
    this.v = version ? '?v=' + encodeURIComponent(version) : '';  // on every engine file: a new build is a new URL
    this.onLog = onLog || ((s) => console.log('[iotdash]', s));
    this.onRadio = onRadio || (() => {});
    this.onTare = onTare || (() => {});
    this.fetchFn = fetchFn || ((...a) => fetch(...a));
    this.ctx = canvas.getContext('2d');
    this.frame = this.ctx.createImageData(W, H);
    this.pixels = new Uint32Array(this.frame.data.buffer);
    this.prev = document.createElement('canvas');  // the frame before a transition
    this.prev.width = W;
    this.prev.height = H;
    this.cur = document.createElement('canvas');
    this.cur.width = W;
    this.cur.height = H;
    this.anim = null;
    this.running = false;
    this.booted = false;
    this.state = { wifi: null, bt: null };
    this._timer = 0;
  }

  async init() {
    const { default: createIotdash } = await import(/* @vite-ignore */ this.base + 'iotdash.js' + this.v);
    this.M = await createIotdash({
      locateFile: (p) => this.base + p + this.v,
      print: (s) => this.onLog(s),
      printErr: (s) => this.onLog(s),
      iotLog: (s) => this.onLog(s),
      iotFetch: (...a) => this._fetch(...a),
      iotDownload: (id, url) => this._download(id, url),
      iotPresent: (kind, ms, ax, ay, aw, ah) => this._present(kind, ms, { x: ax, y: ay, w: aw, h: ah }),
      iotRadio: (name, json) => {
        try {
          this.state[name] = JSON.parse(json);
        } catch (e) {
          return;
        }
        this.onRadio(name, this.state[name]);
      },
      iotTare: () => this.onTare(),
    });
    const FS = this.M.FS;
    for (const d of ['/card', '/card/system', '/card/system/assets', '/card/system/data', '/card/applications'])
      if (!FS.analyzePath(d).exists) FS.mkdir(d);
    const get = async (p) => new Uint8Array(await (await fetch(this.base + 'system/' + p + this.v)).arrayBuffer());
    const [boot, os, osJson, icon] = await Promise.all([get('boot.idb'), get('os.idb'), get('os.json'),
      get('assets/default-icon.img')]);
    FS.writeFile('/boot.idb', boot);
    FS.writeFile('/card/system/os.idb', os);
    FS.writeFile('/card/system/os.json', osJson);
    FS.writeFile('/card/system/assets/default-icon.img', icon);
    // a simulated network the OS joins by itself, its internet probe answered by the page
    this.setRegistry({
      'net.probeUrl': 'sim:probe',
      'secure.wifi': [{ ssid: 'Home-WiFi', pass: 'home1234' }],
    });
    this._input();
    return this;
  }

  // registry values before boot (the board keeps them in flash)
  setRegistry(values) {
    const FS = this.M.FS;
    let reg = {};
    try {
      reg = JSON.parse(new TextDecoder().decode(FS.readFile('/card.registry.json')));
    } catch (e) { /* none yet */ }
    for (const [k, v] of Object.entries(values)) reg[k] = v;  // the file holds JSON values (Os::registryLoad)
    FS.writeFile('/card.registry.json', JSON.stringify(reg));
  }

  // install a package on the simulated card; returns its manifest
  installPackage(buffer, source = 'store') {
    const pkg = buffer instanceof ArrayBuffer ? parseIdbz(buffer) : buffer;
    const id = pkg.manifest.id;
    if (!/^[a-z0-9][a-z0-9-]{0,63}$/.test(id)) throw new Error(`bad app id ${id}`);
    const FS = this.M.FS;
    const dir = `/card/applications/${id}`;
    if (!FS.analyzePath(dir).exists) FS.mkdir(dir);
    if (!FS.analyzePath(dir + '/data').exists) FS.mkdir(dir + '/data');
    for (const n of ['app.json', 'app.idb', 'icon.img']) FS.writeFile(`${dir}/${n}`, pkg.files[n]);
    FS.writeFile(`${dir}/install.json`, JSON.stringify({ source, version: pkg.manifest.version,
      installed: Math.floor(Date.now() / 1000) }));
    if (this.booted) this.M.ccall('iot_apps_changed', null, ['string'], [id]);
    return pkg.manifest;
  }

  boot() {
    if (this.booted) return;
    if (!this.M.ccall('iot_boot', 'number', ['string'], ['/boot.idb'])) throw new Error('the OS did not start');
    this.booted = true;
    this.running = true;
    this._tick();
  }

  launch(id) {
    const err = this.M.ccall('iot_launch', 'string', ['string'], [id]);
    if (err) throw new Error(err);
    this.wake();
  }
  home() {
    this.M.ccall('iot_home', null, [], []);
    this.wake();
  }
  eval(js) {
    return this.M.ccall('iot_eval', 'string', ['string'], [js]);
  }

  // the simulated world
  setInternet(on) {
    this.M.ccall('iot_internet', null, ['number'], [on ? 1 : 0]);
  }
  setWifiReach(on) {
    this.M.ccall('iot_wifi_reach', null, ['number'], [on ? 1 : 0]);
    this.wake();
  }
  scale({ kg = 0, stable = true, overload = false, intervalMs = 200 } = {}) {
    this.M.ccall('iot_scale', null, ['number', 'number', 'number', 'number'], [kg, stable ? 1 : 0, overload ? 1 : 0,
      intervalMs]);
  }
  scalePower(on) {
    this.M.ccall('iot_scale_power', null, ['number'], [on ? 1 : 0]);
    this.wake();
  }

  stop() {
    this.running = false;
    clearTimeout(this._timer);
    cancelAnimationFrame(this._raf);
    this._detach && this._detach();
  }

  // ---- the loop: an engine step every animation frame while it is busy, less when idle
  wake() {
    if (!this.running) return;
    clearTimeout(this._timer);
    cancelAnimationFrame(this._raf);
    this._raf = requestAnimationFrame(() => this._tick());
  }
  _tick() {
    if (!this.running) return;
    let idle = 16;
    try {
      idle = this.M._iot_update();
    } catch (e) {
      this.onLog('engine stopped: ' + e);
      this.running = false;
      return;
    }
    if (this.anim) this._drawAnim();
    if (this.anim || idle <= 20) this._raf = requestAnimationFrame(() => this._tick());
    else this._timer = setTimeout(() => this._tick(), Math.min(idle, 100));
  }

  // ---- frames
  _readFrame() {
    const ptr = this.M._iot_frame() >> 1;
    const px = this.M.HEAPU16.subarray(ptr, ptr + W * H);
    const out = this.pixels;
    for (let i = 0; i < px.length; i++) {
      const c = px[i];
      const r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
      out[i] = 0xff000000 | (((b << 3) | (b >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((r << 3) | (r >> 2));
    }
  }
  _present(kind, ms, area) {
    if (kind && ms > 0) {
      // keep what is on the canvas, animate to the new frame
      this.prev.getContext('2d').drawImage(this.canvas, 0, 0);
      this._readFrame();
      this.cur.getContext('2d').putImageData(this.frame, 0, 0);
      this.anim = { kind, ms, area, t0: performance.now() };
      this._drawAnim();
      return;
    }
    this._readFrame();
    if (this.anim) {  // a frame inside a transition: becomes its target
      this.cur.getContext('2d').putImageData(this.frame, 0, 0);
      return;
    }
    this.ctx.putImageData(this.frame, 0, 0);
  }
  _drawAnim() {
    const a = this.anim;
    const t = Math.min(1, (performance.now() - a.t0) / a.ms);
    const e = 1 - Math.pow(1 - t, 3);  // ease-out cubic, like the board
    const c = this.ctx, { x, y, w, h } = a.area;
    c.drawImage(this.cur, 0, 0);
    c.save();
    c.beginPath();
    c.rect(x, y, w, h);
    c.clip();
    if (a.kind === T_FADE) {
      c.globalAlpha = 1 - e;
      c.drawImage(this.prev, 0, 0);
    } else if (a.kind === T_UP || a.kind === T_DOWN) {
      const off = Math.round(h * e) * (a.kind === T_UP ? -1 : 1);
      c.drawImage(this.prev, 0, off);
      c.drawImage(this.cur, 0, off + (a.kind === T_UP ? h : -h));
    } else if (a.kind === T_LEFT || a.kind === T_RIGHT) {
      const off = Math.round(w * e) * (a.kind === T_LEFT ? -1 : 1);
      c.drawImage(this.prev, off, 0);
      c.drawImage(this.cur, off + (a.kind === T_LEFT ? w : -w), 0);
    }
    c.restore();
    if (t >= 1) {
      this.anim = null;
      c.drawImage(this.cur, 0, 0);
    }
  }

  // ---- HTTP for the engine (a page's fetch / the OS)
  _fetch(id, method, url, headers, body, timeoutMs) {
    const ctl = new AbortController();
    const timer = setTimeout(() => ctl.abort(), timeoutMs || 15000);
    const done = (status, statusText, hdrs, buf, finalUrl, error) => {
      const M = this.M;
      const len = buf ? buf.length : 0;
      const ptr = len ? M._malloc(len) : 0;
      if (len) M.HEAPU8.set(buf, ptr);
      M.ccall('iot_fetch_done', null, ['number', 'number', 'string', 'string', 'number', 'number', 'string', 'string'],
        [id, status, statusText || '', JSON.stringify(hdrs || {}), ptr, len, finalUrl || url, error || '']);
      if (ptr) M._free(ptr);
      this.wake();
    };
    const opts = { method, headers, signal: ctl.signal, redirect: 'follow' };
    if (body && method !== 'GET' && method !== 'HEAD') opts.body = body;
    this.fetchFn(url, opts)
      .then(async (r) => {
        const buf = new Uint8Array(await r.arrayBuffer());
        const hdrs = {};
        r.headers.forEach((v, k) => (hdrs[k] = v));
        done(r.status, r.statusText, hdrs, buf, r.url, '');
      })
      .catch((e) => done(0, '', {}, null, url, ctl.signal.aborted ? 'timed out' : String(e.message || e)))
      .finally(() => clearTimeout(timer));
  }

  // a download (the OS installing an app): streamed to the engine piece by piece
  async _download(id, url) {
    const M = this.M;
    const end = (err, status) => {
      M.ccall('iot_download_done', null, ['number', 'string', 'number'], [id, err, status]);
      this.wake();
    };
    const ctl = new AbortController();
    let status = 0;
    try {
      const r = await this.fetchFn(url, { signal: ctl.signal, redirect: 'follow' });
      status = r.status;
      if (!r.ok || !r.body) return end('', status);
      const total = +(r.headers.get('content-length') || 0);
      const reader = r.body.getReader();
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        for (let off = 0; off < value.length; off += 65536) {
          const piece = value.subarray(off, off + 65536);
          const ptr = M._malloc(piece.length);
          M.HEAPU8.set(piece, ptr);
          const go = M._iot_download_data(id, ptr, piece.length, total);
          M._free(ptr);
          if (!go) {
            ctl.abort();
            return end('stopped', status);
          }
        }
        this.wake();
      }
      end('', status);
    } catch (e) {
      end(String(e.message || e), status);
    }
  }

  // ---- touch (mouse/pen/finger on the canvas) and keyboard
  _input() {
    const cv = this.canvas;
    cv.tabIndex = cv.tabIndex >= 0 ? cv.tabIndex : 0;
    cv.style.touchAction = 'none';
    const at = (ev) => {
      const r = cv.getBoundingClientRect();
      return [(ev.clientX - r.left) * W / r.width, (ev.clientY - r.top) * H / r.height];
    };
    let down = false;
    const send = (phase, ev) => {
      if (!this.booted) return;
      const [x, y] = at(ev);
      this.M._iot_pointer(phase, x, y);
      this.wake();
    };
    const onDown = (ev) => {
      cv.focus();
      cv.setPointerCapture(ev.pointerId);
      down = true;
      send(0, ev);
      ev.preventDefault();
    };
    const onMove = (ev) => {
      if (down) send(1, ev);
    };
    const onUp = (ev) => {
      if (!down) return;
      down = false;
      send(ev.type === 'pointercancel' ? 3 : 2, ev);
    };
    const onKey = (ev) => {
      if (!this.booted || ev.metaKey || ev.ctrlKey || ev.altKey) return;
      if (KEYS.has(ev.key)) this.M.ccall('iot_key', null, ['string'], [ev.key]);
      else if (ev.key.length === 1 || /^[฀-๿]$/.test(ev.key)) this.M.ccall('iot_text', null, ['string'], [ev.key]);
      else return;
      ev.preventDefault();
      this.wake();
    };
    cv.addEventListener('pointerdown', onDown);
    cv.addEventListener('pointermove', onMove);
    cv.addEventListener('pointerup', onUp);
    cv.addEventListener('pointercancel', onUp);
    cv.addEventListener('keydown', onKey);
    this._detach = () => {
      cv.removeEventListener('pointerdown', onDown);
      cv.removeEventListener('pointermove', onMove);
      cv.removeEventListener('pointerup', onUp);
      cv.removeEventListener('pointercancel', onUp);
      cv.removeEventListener('keydown', onKey);
    };
  }
}
