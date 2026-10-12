// iotdash OS script API, loaded into every page (apps and system pages) after the Web API
// bootstrap. N holds the firmware's native functions; page code never sees it.
//
//   navigate("scientific/v1/main.html")   open pages/scientific/v1/main.html (pushes history)
//   navigate(-1)                           back to the previous page
//   setBackgroundProcess(name, "APP" | "PAGE", fn, intervalMs = 1000)
//   rmBackgroundProcess(name)    clearBackgroundProcess("ALL" | "APP" | "PAGE")
//   open(path, "r" | "w" | "a" | "r+" | "w+" | "a+" [+ "b"])   files in the app's data/ folder
//   fs.*            readFile, writeFile, appendFile, list, mkdir, remove, rename, exists, stat
//   localStorage    the app's memory.json
//   registry.get(key, default)   (registry.set/remove/clear: system apps only)
//   os.*            page(), apps(), launch(id), home(), back(), confirm(), alert(), on(), emit(),
//                   processes(), theme(), sd.info(), sd.format() (system), uninstall(id) (system)
//   os.net / os.wifi / os.bt   connectivity: internet state, WiFi networks, Bluetooth LE devices
//   os.store        install apps from the store (system apps only)
//   os.kiosk        kiosk mode: one app as the board's only screen (start: system apps only)
'use strict';
const G = globalThis;

// ------------------------------------------------------------------ navigation
G.navigate = (to) => {
  if (typeof to === 'number') return to < 0 ? N.os('navigate', -1) : false;
  return N.os('navigate', String(to));
};
// history.back() and <a data-back> behave like the system back key
G.history.back = () => N.os('navigate', -1);
G.history.go = (n) => { if (+n < 0) N.os('navigate', -1); };

// ------------------------------------------------------------------ background processes
// fn(info) runs every intervalMs (first run right away). Return a number for a different
// delay, false to stop, or a promise of either. "APP" processes keep running while the app
// moves between pages; "PAGE" ones stop when the page is left.
G.setBackgroundProcess = (name, scope, fn, intervalMs) =>
  N.bg('set', String(name), String(scope), fn, intervalMs === undefined ? 1000 : +intervalMs);
G.rmBackgroundProcess = (name) => N.bg('rm', String(name));
G.clearBackgroundProcess = (scope) => N.bg('clear', String(scope === undefined ? 'ALL' : scope));

// ------------------------------------------------------------------ files (the app's data/ folder)
const isBytes = (d) => d instanceof ArrayBuffer || ArrayBuffer.isView(d);
const bytes = (d) => (ArrayBuffer.isView(d) ? new Uint8Array(d.buffer, d.byteOffset, d.byteLength) : d);

class OsFile {
  constructor(id, path, mode) {
    this._id = id;
    this.path = path;
    this.mode = mode;
    this.closed = false;
  }
  read(n) { return N.fs('read', this._id, n); }  // string, or ArrayBuffer in "b" modes
  readLine() { return N.fs('readLine', this._id); }  // null at the end
  readLines() {
    const out = [];
    for (let l = this.readLine(); l !== null; l = this.readLine()) out.push(l);
    return out;
  }
  write(data) { return N.fs('write', this._id, isBytes(data) ? bytes(data) : String(data)); }
  writeLine(s) { return this.write(String(s) + '\n'); }
  seek(pos, whence) { return N.fs('seek', this._id, +pos || 0, whence === 'end' ? 2 : whence === 'current' ? 1 : +whence || 0); }
  tell() { return N.fs('tell', this._id); }
  get size() { return N.fs('size', this._id); }
  flush() { N.fs('flush', this._id); }
  close() {
    if (this.closed) return;
    this.closed = true;
    N.fs('close', this._id);
  }
}

G.open = (path, mode) => {
  path = String(path);
  mode = mode === undefined ? 'r' : String(mode);
  return new OsFile(N.fs('open', path, mode), path, mode);
};

G.fs = {
  open: G.open,
  readFile: (p, enc) => N.fs('readFile', String(p), enc === 'binary' || enc === 'bytes'),
  readJSON: (p) => JSON.parse(N.fs('readFile', String(p), false)),
  writeFile: (p, data) => N.fs('writeFile', String(p), isBytes(data) ? bytes(data) : String(data), false),
  appendFile: (p, data) => N.fs('writeFile', String(p), isBytes(data) ? bytes(data) : String(data), true),
  writeJSON: (p, v) => N.fs('writeFile', String(p), JSON.stringify(v), false),
  exists: (p) => N.fs('exists', String(p)),
  stat: (p) => N.fs('stat', String(p)),
  list: (p) => N.fs('list', String(p === undefined ? '' : p)),
  mkdir: (p) => N.fs('mkdir', String(p)),
  remove: (p, recursive) => N.fs('remove', String(p), !!recursive),
  rename: (a, b) => N.fs('rename', String(a), String(b)),
};

// ------------------------------------------------------------------ registry
G.registry = {
  get(key, def) {
    const v = N.reg('get', String(key));
    return v === null ? (def === undefined ? null : def) : JSON.parse(v);
  },
  keys: () => N.reg('keys'),
  all: () => JSON.parse(N.reg('all')),
  set: (key, value) => N.reg('set', String(key), JSON.stringify(value === undefined ? null : value)),
  remove: (key) => N.reg('remove', String(key)),
  clear: () => N.reg('clear'),
  onChange: (fn) => G.os.on('registry', fn),
};

// ------------------------------------------------------------------ the OS
G.os = {
  version: N.os('version'),
  get app() { return N.os('page').id; },
  page: () => N.os('page'),  // {id, title, page, depth, system}
  apps: () => N.os('apps'),
  launch: (id) => N.os('launch', String(id)),
  home: (ask) => N.os('home', ask === undefined ? true : !!ask),
  back: () => N.os('back'),
  // System dialog; resolves true for OK. opts: {ok: 'Delete', cancel: 'Keep'} (cancel: '' = no cancel button)
  confirm(title, message, opts) {
    opts = opts || {};
    return new Promise((resolve) => N.os('confirm', String(title || ''), String(message || ''), String(opts.ok || 'OK'),
      resolve, opts.cancel === undefined ? 'Cancel' : String(opts.cancel)));
  },
  alert(title, message) { return G.os.confirm(title, message, {cancel: ''}).then(() => undefined); },
  // OS events: 'registry' ({key, value}) and anything apps emit to themselves
  on(name, fn) {
    const id = N.bus('on', String(name), fn);
    return () => N.bus('off', id);
  },
  emit(name, data) { N.bus('emit', String(name), data === undefined ? undefined : JSON.stringify(data)); },
  processes: () => N.os('processes'),
  get privileged() { return N.os('privileged'); },
  theme: () => (G.registry.get('darkMode', false) ? 'dark' : 'light'),
  sd: {
    info: () => N.os('sdInfo'),
    format: () => N.os('sdFormat'),
  },
  uninstall: (id) => N.os('uninstall', String(id)),
};
G.alert = (m) => { G.os.alert('', String(m)); };

// ------------------------------------------------------------------ connectivity
// Requests answer as promises (rejected with the radio's error); state() is synchronous: what
// the OS last heard from the radios. Changing connections is for system pages (Settings).
const net = (op, args) => new Promise((resolve, reject) =>
  N.net(op, JSON.stringify(args || {}), (r) => (r && r.err ? reject(new Error(r.err)) : resolve(r ? r.v : undefined))));
const toHex = (d) => {
  let b;
  if (typeof d === 'string') b = new TextEncoder().encode(d);
  else if (d instanceof ArrayBuffer) b = new Uint8Array(d);
  else if (ArrayBuffer.isView(d)) b = new Uint8Array(d.buffer, d.byteOffset, d.byteLength);
  else b = Uint8Array.from(d || []);
  let s = '';
  for (const x of b) s += (x < 16 ? '0' : '') + x.toString(16);
  return s;
};
const fromHex = (h) => {
  const b = new Uint8Array((h || '').length >> 1);
  for (let i = 0; i < b.length; i++) b[i] = parseInt(h.substr(i * 2, 2), 16);
  return b;
};

G.os.net = {
  state: () => JSON.parse(N.net('state')),  // {wifi, internet, bt}
  online: () => JSON.parse(N.net('state')).internet,  // true | false | null (not known yet)
  check: () => net('net.check'),  // check the internet now: Promise<boolean>
  on: (fn) => G.os.on('net', fn),  // any change of wifi / internet / bt power
};
G.os.wifi = {
  // {power, state: 'off'|'idle'|'connecting'|'connected'|'failed', ssid, ip, rssi, error}
  state: () => G.os.net.state().wifi,
  scan: () => net('wifi.scan'),  // [{ssid, rssi, secure, channel}], strongest first
  saved: () => net('wifi.saved'),  // [{ssid}], most recent first
  connect: (ssid, pass) => net('wifi.connect', pass === undefined ? {ssid: String(ssid)} : {ssid: String(ssid), pass: String(pass)}),
  disconnect: () => net('wifi.disconnect'),
  forget: (ssid) => net('wifi.forget', {ssid: String(ssid)}),
  power: (on) => net('wifi.power', {on: !!on}),
};
G.os.bt = {
  // {power, scanning, connected: [{id, name}]}
  state: () => G.os.net.state().bt,
  // devices seen during the scan: [{id, name, rssi, type, services}]; 'found' events meanwhile
  scan: (opts) => net('bt.scan', {ms: (opts && opts.ms) || 8000}),
  stop: () => net('bt.stop'),
  devices: () => net('bt.devices'),  // remembered: [{id, name, type}], most recent first
  // {id, name, type, mtu, services: [{uuid, chars: [{uuid, props: ['read', 'write', 'notify', ...]}]}]}
  connect: (id) => net('bt.connect', {id: String(id)}),
  disconnect: (id) => net('bt.disconnect', {id: String(id)}),
  forget: (id) => net('bt.forget', {id: String(id)}),
  power: (on) => net('bt.power', {on: !!on}),
  read: (id, service, char) => net('bt.read', {id, service, char}).then((r) => fromHex(r && r.data)),
  // data: string (UTF-8), bytes or an array of numbers; response false = write without response
  write: (id, service, char, data, response) =>
    net('bt.write', {id, service, char, data: toHex(data), response: response !== false}),
  subscribe: (id, service, char) => net('bt.notify', {id, service, char, on: true}),
  unsubscribe: (id, service, char) => net('bt.notify', {id, service, char, on: false}),
  // events: {type: 'notify', id, service, char, bytes, text} | {type: 'disconnected', id, reason}
  //         | {type: 'found', device} | {type: 'state', power, scanning, connected}
  on: (fn) => G.os.on('bt', (e) => {
    if (e && typeof e.data === 'string') {
      e.bytes = fromHex(e.data);
      e.text = String.fromCharCode.apply(null, e.bytes);
    }
    fn(e);
  }),
};

// ------------------------------------------------------------------ apps from the store
// The package (.idbz) is downloaded straight onto the card and checked file by file; the app's
// data stays across updates. os.apps() tells each app's source: 'store' | 'usb' | 'system' | ''.
G.os.store = {
  // {id, url, version?, version_id?, store?, size?} -> Promise<{id, version}>
  install: (o) => net('store.install', o),
  cancel: (id) => net('store.cancel', {id: String(id)}),
  // a file the store app keeps (icons): path under /system/data/store/ -> Promise<{path, size}>
  download: (url, path) => net('store.download', {url: String(url), path: String(path)}),
  // {id, state: 'downloading' | 'installed' | 'failed', got, total, error}
  on: (fn) => G.os.on('install', fn),
};

// ------------------------------------------------------------------ kiosk mode
// One installed app becomes the board's only screen: it opens at start-up, and leaving it (Home,
// Back on its first page, os.home()) asks for the password given here; the right password turns
// kiosk mode off. The OS keeps only a salted hash of the password.
G.os.kiosk = {
  status: () => N.os('kiosk'),  // {app} while on, else null
  start: (id, password) => N.os('kioskStart', String(id), String(password)),  // opens the app
};
