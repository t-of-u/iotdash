// Settings (a system app): writes the registry, shows the card and the installed apps, and
// does the factory reset (formats the card).
const $ = (s) => document.querySelector(s);
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;'}[c]));

// ---- tabs (one page; the last tab is remembered in the system memory.json)
function showTab(name) {
  document.querySelectorAll('.tab').forEach((t) => t.classList.toggle('on', t.dataset.tab === name));
  document.querySelectorAll('.panel').forEach((p) => (p.hidden = p.id !== name));
  localStorage.setItem('settingsTab', name);
  refresh(name);
}

function refresh(name) {
  if (name !== 'connection') Connection.hide();
  if (name === 'sdcard') refreshCard();
  else if (name === 'system') refreshSystem();
  else if (name === 'connection') Connection.show();
}
const currentTab = () => document.querySelector('.tab.on').dataset.tab;
document.querySelector('.tabs').addEventListener('click', (e) => {
  const t = e.target.closest('.tab');
  if (t) showTab(t.dataset.tab);
});

// ---- general: switches bound to registry keys
function refreshSwitches() {
  document.querySelectorAll('.switch').forEach((s) => {
    s.classList.toggle('on', !!registry.get(s.dataset.key, s.dataset.default === 'true'));
  });
  // what the home screen clock looks like with the current choice
  const d = new Date();
  const h = d.getHours(), m = String(d.getMinutes()).padStart(2, '0');
  $('#clockSample').textContent = registry.get('clock24', true)
    ? `Home screen shows ${String(h).padStart(2, '0')}:${m}`
    : `Home screen shows ${h % 12 || 12}:${m} ${h < 12 ? 'AM' : 'PM'}`;
}
document.querySelectorAll('.switch').forEach((s) => {
  s.addEventListener('click', () => {
    const on = !s.classList.contains('on');
    s.classList.toggle('on', on);
    registry.set(s.dataset.key, on);  // dark mode applies to the whole system at once
  });
});

// ---- system
const fmtBytes = (n) => {
  if (!n) return '0 B';
  const u = ['B', 'KB', 'MB', 'GB', 'TB'];
  const i = Math.min(u.length - 1, Math.floor(Math.log(n) / Math.log(1024)));
  return (n / 1024 ** i).toFixed(i > 1 ? 1 : 0) + ' ' + u[i];
};

function storageInfo() {
  const sd = os.sd.info();
  if (!sd.mounted) return {sd, name: 'No storage', kind: 'Insert an SD card'};
  if (sd.type === 'FLASH') return {sd, name: 'Internal flash', kind: 'No SD card: apps live in the board\'s flash'};
  return {sd, name: 'SD card', kind: [sd.type, sd.name].filter(Boolean).join(' · ')};
}

function refreshCard() {
  const {sd, name, kind} = storageInfo();
  $('#sdName').textContent = name;
  $('#sdKind').textContent = kind;
  const used = sd.mounted ? sd.total - sd.free : 0;
  $('#sdUsed').textContent = fmtBytes(used);
  $('#sdTotal').textContent = sd.mounted ? 'of ' + fmtBytes(sd.total) : '';
  $('#sdFree').textContent = fmtBytes(sd.mounted ? sd.free : 0);
  $('#sdBar').style.width = (sd.total ? Math.max(2, (100 * used) / sd.total) : 0).toFixed(1) + '%';
  const apps = os.apps().filter((a) => !a.system);
  $('#apps').innerHTML = apps.length
    ? apps.map((a) => `<div class="row">
        <img class="app-icon" src="${esc(a.icon)}">
        <div class="text"><div class="name">${esc(a.title)}</div><div class="hint">${esc(a.id)}${a.version ? ' · v' + esc(a.version) : ''}</div></div>
        <div class="btn subtle-red" data-remove="${esc(a.id)}">Remove</div>
      </div>`).join('')
    : '<div class="empty">No apps installed yet</div>';
}

function refreshSystem() {
  $('#osVersion').textContent = os.version;
  $('#osStorage').textContent = storageInfo().name;
  $('#osApps').textContent = String(os.apps().filter((a) => !a.system).length);
  const reg = registry.all();
  const keys = Object.keys(reg).sort();
  $('#registry').innerHTML = keys.length
    ? keys.map((k) => `<div class="row kv"><div class="key">${esc(k)}</div><div class="pill">${
      k.startsWith('secure.') ? '••••••' : esc(JSON.stringify(reg[k]))}</div></div>`).join('')
    : '<div class="empty">Empty: every setting uses its default</div>';
}

$('#apps').addEventListener('click', async (e) => {
  const b = e.target.closest('[data-remove]');
  if (!b) return;
  const id = b.dataset.remove;
  if (!(await os.confirm('Remove app', `Remove "${id}" and its data from the card?`, {ok: 'Remove'}))) return;
  try {
    os.uninstall(id);
  } catch (err) {
    await os.alert('Cannot remove', err.message);
  }
  refreshCard();
});

$('#resetRegistry').addEventListener('click', async () => {
  if (!(await os.confirm('Reset registry', 'Every setting goes back to its default.', {ok: 'Reset'}))) return;
  registry.clear();
  refreshSwitches();
  refreshSystem();
});

$('#format').addEventListener('click', async () => {
  const ok = await os.confirm('Factory reset',
    'The SD card is formatted: every app, its data and all settings are erased. ' +
    'The system stays, and apps can be installed again from a computer.', {ok: 'Format'});
  if (!ok) return;
  const btn = $('#format');
  btn.textContent = 'Formatting…';
  // let that frame show: formatting blocks the system for a few seconds
  await new Promise((r) => setTimeout(r, 60));
  try {
    os.sd.format();
    await os.alert('Factory reset done', 'The SD card holds only the system now.');
  } catch (err) {
    await os.alert('Format failed', err.message);
  }
  btn.textContent = 'Format';
  refreshCard();
});

registry.onChange(() => {
  refreshSwitches();
  if (currentTab() !== 'connection') refresh(currentTab());  // that page follows os.net events
});
os.on('apps', () => refresh(currentTab()));  // installed or removed over USB

refreshSwitches();
showTab(localStorage.getItem('settingsTab') || 'general');
