// Apps: the store's catalog (iotstore API, docs in store/docs/API.md) next to the apps on
// the card. Installs go through os.store: the package downloads straight onto the card, file by
// file with checksums, and replaces the app's files (its data stays).
//
// registry  store.url        the store's address (set here)
// data      store/catalog.json   the last catalog (shown while offline)
//           store/icons/<app>-<version_id>.img   icons (IMG1, from the store)
'use strict';
const $ = (s) => document.querySelector(s);
const esc = (s) => String(s == null ? '' : s).replace(/[&<>"]/g, (c) => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;'}[c]));
const DEFAULT_ICON = '/system/assets/default-icon.img';
const ICONS = 'store/icons';
// install.json "source": the store, `iotdash app install` over USB, or copied onto the card by hand
const SOURCE = {store: 'Store', usb: 'USB', '': 'Card'};

let base = '';         // the store's address, no trailing slash
let catalog = [];      // store apps: {app_id, name, description, latest: {...}, ...}
let catalogAt = 0;     // when it was fetched (wall clock, ms; saved with it); 0 = never
let fetchedAt = -1;    // performance.now() of this session's fetch (the wall clock may jump when it syncs)
let offline = false;   // the catalog shown is the saved one
let loading = false;
let local = new Map(); // app id -> os.apps() entry (apps on the card)
const jobs = {};       // app id -> {got, total, version}
let tab = 'store';
let detailId = '';

// ------------------------------------------------------------------ helpers
const vparts = (v) => String(v || '').split(/[.\-+]/).map((p) => (/^\d+$/.test(p) ? +p : p));
function vcmp(a, b) {
  const x = vparts(a), y = vparts(b);
  for (let i = 0; i < Math.max(x.length, y.length); i++) {
    const p = x[i] === undefined ? 0 : x[i], q = y[i] === undefined ? 0 : y[i];
    if (p === q) continue;
    if (typeof p === 'number' && typeof q === 'number') return p - q;
    return String(p) < String(q) ? -1 : 1;
  }
  return 0;
}
function size(n) {
  if (!n) return '';
  if (n < 1024) return n + ' B';
  if (n < 1024 * 1024) return Math.round(n / 1024) + ' KB';
  return (n / 1048576).toFixed(n < 10 * 1048576 ? 1 : 0) + ' MB';
}
function day(iso) {
  const d = new Date(iso);
  if (isNaN(d.getTime())) return '';
  const p = (x) => (x < 10 ? '0' : '') + x;
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`;
}
function ago(ms) {
  if (fetchedAt >= 0) ms = Date.now() - (performance.now() - fetchedAt);
  else if (ms < 1.6e12) return 'a while ago';  // saved before the clock was set
  const s = Math.max(0, Math.round((Date.now() - ms) / 1000));
  if (s < 60) return 'just now';
  if (s < 3600) return Math.round(s / 60) + ' min ago';
  if (s < 86400) return Math.round(s / 3600) + ' h ago';
  return Math.round(s / 86400) + ' d ago';
}
let toastTimer = 0;
function toast(text, bad) {
  const t = $('#toast');
  t.textContent = text;
  t.classList.toggle('bad', !!bad);
  t.classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => t.classList.remove('show'), 2600);
}
function readJSON(p, d) {
  try {
    return fs.readJSON(p);
  } catch (e) {
    return d;
  }
}

// the store API answers {success, data} or {success: false, message}
async function api(path, opts) {
  if (!base) throw new Error('no store address');
  const r = await fetch(base + path, opts);
  let j = null;
  try {
    j = await r.json();
  } catch (e) {}
  if (!r.ok || !j || j.success === false) throw new Error((j && j.message) || `the store answered ${r.status}`);
  return j.data;
}
const post = (path, body) => api(path, {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(body || {})});

// ------------------------------------------------------------------ icons
const iconFile = (a) => `${ICONS}/${a.app_id}-${a.latest.version_id}.img`;
function iconSrc(a) {
  const l = local.get(a.app_id);
  if (fs.exists(iconFile(a))) return 'sd:/system/data/' + iconFile(a);
  return l ? l.icon : DEFAULT_ICON;
}
let iconsBusy = false;
async function fetchIcons() {
  if (iconsBusy) return;
  iconsBusy = true;
  for (const a of catalog) {
    if (!a.icon_img_url || fs.exists(iconFile(a))) continue;
    try {
      await os.store.download(a.icon_img_url, '/system/data/' + iconFile(a));
    } catch (e) {
      continue;
    }
    try {  // the app's older icons
      for (const f of fs.list(ICONS)) {
        const n = typeof f === 'string' ? f : f.name;
        if (n.startsWith(a.app_id + '-') && `${ICONS}/${n}` !== iconFile(a)) fs.remove(`${ICONS}/${n}`);
      }
    } catch (e) {}
    for (const img of document.querySelectorAll(`[data-icon="${a.app_id}"]`)) img.src = iconSrc(a);
  }
  iconsBusy = false;
}

// ------------------------------------------------------------------ data
function readLocal() {
  local = new Map();
  for (const a of os.apps()) if (!a.system) local.set(a.id, a);
}

async function loadCatalog() {
  if (!base || loading) return;
  loading = true;
  $('#refresh').classList.add('busy');
  renderStatus();
  try {
    const d = await api(`/api/store/apps?os=${encodeURIComponent(os.version)}`);
    catalog = d.items || [];
    catalogAt = Date.now();
    fetchedAt = performance.now();
    offline = false;
    try {
      fs.writeJSON('store/catalog.json', {base, at: catalogAt, items: catalog});
    } catch (e) {}
  } catch (e) {
    const saved = readJSON('store/catalog.json', null);
    if (saved && saved.base === base) {
      catalog = saved.items || [];
      catalogAt = saved.at || 0;
    }
    fetchedAt = -1;
    offline = true;
    $('#banner').textContent = `Cannot reach the store (${e.message}).` + (catalogAt ? ` Showing the list from ${ago(catalogAt)}.` : '');
  }
  loading = false;
  $('#refresh').classList.remove('busy');
  render();
  fetchIcons();
}

// ------------------------------------------------------------------ store list
function storeAction(a) {
  const j = jobs[a.app_id];
  if (j) return progress(a.app_id, j);
  const l = local.get(a.app_id);
  if (!l) return `<div class="btn primary" data-install="${esc(a.app_id)}">Install</div>`;
  const newer = vcmp(a.latest.version, l.version) > 0 || (l.source === 'store' && l.versionId && l.versionId !== a.latest.version_id && vcmp(a.latest.version, l.version) >= 0);
  if (newer) return `<div class="btn primary" data-install="${esc(a.app_id)}">Update</div>`;
  return `<div class="btn" data-open="${esc(a.app_id)}">Open</div>`;
}
function progress(id, j) {
  const pct = j.total ? Math.min(100, Math.round(j.got * 100 / j.total)) : 0;
  return `<div class="prog"><div class="prog-top"><span>${j.total ? size(j.got) : 'Starting…'}</span><b>${j.total ? pct + '%' : ''}</b></div>` +
    `<div class="bar"><div class="bar-fill" style="width: ${pct}%"></div></div></div>` +
    `<div class="stop" data-cancel="${esc(id)}"><svg width="14" height="14" viewBox="0 0 24 24"><path d="M6 6l12 12M18 6 6 18"/></svg></div>`;
}
function storeRow(a) {
  const l = local.get(a.app_id);
  let state = '';
  if (l) state = `<span class="badge ${l.source === 'store' ? 'store' : 'usb'}">Installed ${esc(l.version)}${l.source === 'store' ? '' : ' · ' + SOURCE[l.source || '']}</span>`;
  else if (a.latest.created_date && Date.now() - Date.parse(a.latest.created_date) < 7 * 86400e3) state = '<span class="badge new">New</span>';
  return `<div class="row app" data-detail="${esc(a.app_id)}">
    <img class="app-icon" data-icon="${esc(a.app_id)}" src="${esc(iconSrc(a))}">
    <div class="text">
      <div class="name">${esc(a.name || a.latest.title || a.app_id)}</div>
      <div class="desc">${esc(a.latest.description || a.description || a.app_id)}</div>
      <div class="meta">${state}<span>${esc(a.latest.version)} · ${size(a.latest.size)} · ${a.installs || 0} installs</span></div>
    </div>
    <div class="act" data-act="${esc(a.app_id)}">${storeAction(a)}</div>
  </div>`;
}

// ------------------------------------------------------------------ installed list
function installedRow(l) {
  const a = catalog.find((x) => x.app_id === l.id);
  const j = jobs[l.id];
  const src = `<span class="badge ${l.source === 'store' ? 'store' : 'usb'}">${SOURCE[l.source || '']}</span>`;
  let hint = l.installed ? `installed ${day(l.installed * 1000)}` : '';
  let act = '';
  if (j) act = progress(l.id, j);
  else {
    if (a && vcmp(a.latest.version, l.version) > 0) {
      hint = `${esc(a.latest.version)} in the store`;
      act += `<div class="btn primary" data-install="${esc(l.id)}">Update</div>`;
    } else if (a && l.source !== 'store') {
      act += `<div class="btn subtle" data-install="${esc(l.id)}">Use store's</div>`;
    }
    act += `<div class="btn" data-open="${esc(l.id)}">Open</div>`;
    act += `<div class="icon-btn" data-remove="${esc(l.id)}"><svg width="18" height="18" viewBox="0 0 24 24"><path d="M4 7h16M10 11v6M14 11v6M6 7l1 12a2 2 0 0 0 2 2h6a2 2 0 0 0 2-2l1-12M9 7V4h6v3"/></svg></div>`;
  }
  return `<div class="row app"${a ? ` data-detail="${esc(l.id)}"` : ''}>
    <img class="app-icon" src="${esc(l.icon)}">
    <div class="text">
      <div class="name">${esc(l.title)}</div>
      <div class="meta">${src}<span>${esc(l.version || '?')}${hint ? ' · ' + esc(hint) : ''}</span></div>
    </div>
    <div class="act" data-act="${esc(l.id)}">${act}</div>
  </div>`;
}

// ------------------------------------------------------------------ rendering
function renderStatus() {
  $('#addrValue').textContent = base ? base.replace(/^https?:\/\//, '') : 'not set';
  const net = os.net.state();
  let s = '';
  if (!base) s = 'No store address yet';
  else if (loading) s = 'Checking the store…';
  else if (catalogAt && !offline) s = `Updated ${ago(catalogAt)}`;
  else if (net.internet === false || !(net.wifi && net.wifi.state === 'connected')) s = 'Offline';
  else if (catalogAt) s = `Saved list from ${ago(catalogAt)}`;
  $('#status').textContent = s;
}
function render() {
  readLocal();
  renderStatus();
  $('#banner').hidden = !(offline && base);
  $('#storeCount').textContent = catalog.length ? String(catalog.length) : '';
  $('#installedCount').textContent = local.size ? String(local.size) : '';
  if (!base) {
    $('#storeList').innerHTML = '<div class="empty">Set the store\'s address to see its apps.<div class="btn primary" id="setAddr">Set address</div></div>';
  } else if (!catalog.length) {
    $('#storeList').innerHTML = `<div class="empty">${loading ? 'Loading…' : offline ? 'The store cannot be reached.' : 'No apps for OS ' + esc(os.version) + ' yet.'}</div>`;
  } else {
    $('#storeList').innerHTML = catalog.map(storeRow).join('');
  }
  const apps = [...local.values()].sort((a, b) => a.title.localeCompare(b.title));
  $('#installedList').innerHTML = apps.length ? apps.map(installedRow).join('') : '<div class="empty">No apps on the card</div>';
  if (detailId) renderDetail();
}
// one app's action buttons (progress events: no full re-render)
function renderAction(id) {
  const a = catalog.find((x) => x.app_id === id);
  for (const el of document.querySelectorAll(`[data-act="${id}"]`)) {
    if (el.closest('#storeList') && a) el.innerHTML = storeAction(a);
    else if (el.closest('#installedList') && jobs[id]) el.innerHTML = progress(id, jobs[id]);
  }
}

// ------------------------------------------------------------------ actions
async function install(id, versionId) {
  if (jobs[id]) return;
  const a = catalog.find((x) => x.app_id === id);
  jobs[id] = {got: 0, total: 0};
  renderAction(id);
  if (detailId === id) renderDetail();
  try {
    const t = await post(`/api/store/apps/${encodeURIComponent(id)}/install`, versionId ? {version_id: versionId, os: os.version} : {os: os.version});
    if (!jobs[id]) return;  // cancelled meanwhile
    jobs[id] = {got: 0, total: t.size || 0, version: t.version};
    renderAction(id);
    const r = await os.store.install({id, url: t.url, version: t.version, version_id: t.version_id, store: base, size: t.size || 0});
    toast(`${(a && a.name) || id} ${r.version} installed`);
  } catch (e) {
    if (e.message !== 'cancelled') toast(`${(a && a.name) || id}: ${e.message}`, true);
  }
  delete jobs[id];
  render();
}

async function remove(id) {
  const l = local.get(id);
  if (!(await os.confirm('Remove app', `Remove "${l ? l.title : id}" and its data from the card?`, {ok: 'Remove'}))) return;
  try {
    os.uninstall(id);
    toast(`${l ? l.title : id} removed`);
  } catch (e) {
    await os.alert('Cannot remove', e.message);
  }
  render();
}

document.addEventListener('click', (e) => {
  const t = e.target;
  let b;
  if ((b = t.closest('[data-install]'))) return install(b.dataset.install, b.dataset.version || '');
  if ((b = t.closest('[data-cancel]'))) {
    const id = b.dataset.cancel;
    os.store.cancel(id);
    delete jobs[id];
    return render();
  }
  if ((b = t.closest('[data-open]'))) {
    try {
      os.launch(b.dataset.open);
    } catch (err) {
      toast(err.message, true);
    }
    return;
  }
  if ((b = t.closest('[data-remove]'))) return remove(b.dataset.remove);
  if (t.closest('#setAddr')) return openAddr();
  if ((b = t.closest('[data-detail]')) && !t.closest('.act')) return openDetail(b.dataset.detail);
});

for (const el of document.querySelectorAll('.tab')) {
  el.addEventListener('click', () => {
    tab = el.dataset.tab;
    for (const x of document.querySelectorAll('.tab')) x.classList.toggle('on', x === el);
    $('#store').hidden = tab !== 'store';
    $('#installed').hidden = tab !== 'installed';
  });
}
$('#refresh').addEventListener('click', () => loadCatalog());

// ------------------------------------------------------------------ an app's versions
let detail = null;  // GET /api/store/apps/{id}
async function openDetail(id) {
  detailId = id;
  detail = null;
  $('#detail').hidden = false;
  renderDetail();
  try {
    detail = await api(`/api/store/apps/${encodeURIComponent(id)}?os=${encodeURIComponent(os.version)}`);
  } catch (e) {
    detail = {error: e.message};
  }
  if (detailId === id) renderDetail();
}
function renderDetail() {
  const a = catalog.find((x) => x.app_id === detailId) || {app_id: detailId, latest: {}};
  const l = local.get(detailId);
  $('#dIcon').src = a.latest.version_id ? iconSrc(a) : l ? l.icon : DEFAULT_ICON;
  $('#dTitle').textContent = a.name || (l && l.title) || detailId;
  $('#dSub').textContent = [a.owner ? 'by ' + a.owner : '', `${a.installs || 0} installs`,
    l ? `installed ${l.version}${l.source === 'store' ? '' : ` (${SOURCE[l.source || '']})`}` : 'not installed'].filter(Boolean).join(' · ');
  $('#dDesc').textContent = a.description || '';
  if (!detail) return void ($('#dVersions').innerHTML = '<div class="empty">Loading versions…</div>');
  if (detail.error) return void ($('#dVersions').innerHTML = `<div class="empty">${esc(detail.error)}</div>`);
  const j = jobs[detailId];
  $('#dVersions').innerHTML = (detail.versions || []).map((v) => {
    const mine = l && (l.versionId ? l.versionId === v.version_id : l.version === v.version);
    let act;
    if (j) act = '';
    else if (mine) act = '<div class="btn subtle dis">Installed</div>';
    else if (!v.compatible) act = '<div class="btn subtle dis">Needs OS ' + esc(v.os_min) + '</div>';
    else act = `<div class="btn${l ? '' : ' primary'}" data-install="${esc(detailId)}" data-version="${esc(v.version_id)}">${
      !l ? 'Install' : vcmp(v.version, l.version) > 0 ? 'Update' : vcmp(v.version, l.version) < 0 ? 'Go back' : 'Reinstall'}</div>`;
    return `<div class="ver"><div class="text">
      <div class="ver-name">${esc(v.version)}${v.version_id === a.latest.version_id ? '<span class="badge new">Latest</span>' : ''}</div>
      <div class="ver-note">${day(v.created_date)} · ${size(v.size)} · OS ${esc(v.os_min || '?')} or newer · ${v.installs || 0} installs</div>
      ${v.description ? `<div class="ver-desc">${esc(v.description)}</div>` : ''}
    </div><div class="act">${act}</div></div>`;
  }).join('') + (j ? `<div class="ver"><div class="text"><div class="ver-note">Installing ${esc(j.version || '')}…</div></div><div class="act">${progress(detailId, j)}</div></div>` : '');
}
$('#dClose').addEventListener('click', () => {
  detailId = '';
  $('#detail').hidden = true;
});
$('#detail').addEventListener('click', (e) => {
  if (e.target === $('#detail')) $('#dClose').click();
});

// ------------------------------------------------------------------ the store's address
function openAddr() {
  $('#addrInput').value = base;
  $('#addrError').textContent = '';
  $('#addrSheet').hidden = false;
  setTimeout(() => $('#addrInput').focus(), 50);
}
function closeAddr() {
  $('#addrInput').blur();
  $('#addrSheet').hidden = true;
}
function saveAddr() {
  let v = $('#addrInput').value.trim().replace(/\/+$/, '');
  if (v && !/^https?:\/\//.test(v)) v = 'http://' + v;
  if (v && !/^https?:\/\/[^\s/]+(\/\S*)?$/.test(v)) return void ($('#addrError').textContent = 'That is not a web address');
  base = v;
  registry.set('store.url', v);
  catalog = [];
  catalogAt = 0;
  closeAddr();
  render();
  loadCatalog();
}
$('#addrBtn').addEventListener('click', openAddr);
$('#addrCancel').addEventListener('click', closeAddr);
$('#addrOk').addEventListener('click', saveAddr);
$('#addrInput').addEventListener('keydown', (e) => {
  if (e.key === 'Enter') saveAddr();
});
$('#addrInput').addEventListener('focus', () => $('#addrField').classList.add('focus'));
$('#addrInput').addEventListener('blur', () => $('#addrField').classList.remove('focus'));

// ------------------------------------------------------------------ events
os.store.on((e) => {
  if (!e || !jobs[e.id]) return;
  if (e.state === 'downloading') {
    jobs[e.id].got = e.got;
    if (e.total) jobs[e.id].total = e.total;
    renderAction(e.id);
    if (detailId === e.id) renderDetail();
  }
});
os.on('apps', () => render());
os.net.on(() => {
  renderStatus();
  if (base && os.net.online() && (offline || !catalogAt)) loadCatalog();
});

// ------------------------------------------------------------------ start
base = String(registry.get('store.url', '') || '');
const saved = readJSON('store/catalog.json', null);
if (saved && saved.base === base) {
  catalog = saved.items || [];
  catalogAt = saved.at || 0;
}
render();
if (base) loadCatalog();
setBackgroundProcess('status', 'PAGE', () => renderStatus(), 30000);
