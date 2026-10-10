// Settings → Connection: Wi-Fi and Bluetooth, the way a tablet does it. The OS keeps the
// saved networks (registry, passwords hidden from apps) and remembered Bluetooth devices;
// this page only shows them and asks for changes (os.wifi / os.bt).
const Connection = (() => {
  const $ = (s) => document.querySelector(s);
  const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;'}[c]));

  let view = localStorage.getItem('connView') || 'wifi';
  let visible = false;

  // ---------------------------------------------------------------- shared bits
  let toastTimer = 0;
  function toast(text, bad) {
    const t = $('#toast');
    t.textContent = text;
    t.classList.toggle('bad', !!bad);
    t.classList.add('show');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => t.classList.remove('show'), 3000);
  }
  const spinner = '<span class="spin"></span>';
  // signal bars: 1..4 from the RSSI
  function bars(rssi) {
    const n = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -78 ? 2 : 1;
    const arc = (i, d) => `<path d="${d}"${n > i ? '' : ' stroke-opacity=".22"'}/>`;
    return '<svg class="sig" width="24" height="24" viewBox="0 0 24 24">' +
      arc(3, 'M2.5 9a14 14 0 0 1 19 0') + arc(2, 'M5.5 12.5a9.5 9.5 0 0 1 13 0') +
      arc(1, 'M8.6 16a5 5 0 0 1 6.8 0') + '<path d="M12 19.5h.01"/></svg>';
  }
  const lock = '<svg class="lock" width="15" height="15" viewBox="0 0 24 24"><rect x="5" y="11" width="14" height="10" rx="2"/><path d="M8 11V7a4 4 0 0 1 8 0v4"/></svg>';
  const btIcon = '<svg class="bt" width="22" height="22" viewBox="0 0 24 24"><path d="M7 7l10 10-5 4.5V2.5L17 7 7 17"/></svg>';

  function show(v) {
    view = v;
    localStorage.setItem('connView', v);
    document.querySelectorAll('#connSeg .seg-btn').forEach((b) => b.classList.toggle('on', b.dataset.view === v));
    $('#wifiView').hidden = v !== 'wifi';
    $('#btView').hidden = v !== 'bt';
    refresh();
  }
  $('#connSeg').addEventListener('click', (e) => {
    const b = e.target.closest('.seg-btn');
    if (b) show(b.dataset.view);
  });

  // ================================================================ Wi-Fi
  let aps = [];          // last scan
  let saved = [];        // [{ssid}]
  let scanning = false;
  let joining = '';      // network being joined from this page
  let lastScan = 0;

  async function wifiScan() {
    const st = os.wifi.state();
    if (scanning || !st.power) return;
    scanning = true;
    renderWifi();
    try {
      aps = await os.wifi.scan();
    } catch (e) {
      if (visible) toast(e.message, true);
    }
    scanning = false;
    lastScan = Date.now();
    renderWifi();
  }

  async function loadSaved() {
    try {
      saved = await os.wifi.saved();
    } catch (e) {
      saved = [];
    }
    renderWifi();
  }

  function wifiStatusLine(st, online) {
    if (st.state === 'connected') {
      const net = online === true ? '<span class="ok-text">Connected</span>'
        : online === false ? '<span class="err-text">Connected, no internet</span>' : 'Connected · checking internet…';
      return net + (st.ip ? ` · ${esc(st.ip)}` : '');
    }
    if (st.state === 'connecting') return 'Connecting…';
    return '';
  }

  function renderWifi() {
    if (!visible || view !== 'wifi') return;
    const st = os.wifi.state() || {};
    const online = os.net.online();
    const on = !!st.power;
    $('#wifiSwitch').classList.toggle('on', on);
    $('#wifiHint').textContent = !on ? 'Off'
      : st.state === 'connected' ? `Connected to ${st.ssid}`
      : st.state === 'connecting' ? `Connecting to ${st.ssid}…`
      : st.state === 'failed' ? `Couldn't connect to ${st.ssid}: ${st.error || 'failed'}`
      : 'Not connected';
    // the network in use (or being joined)
    const cur = on && (st.state === 'connected' || st.state === 'connecting') ? st.ssid : '';
    $('#wifiCurrent').innerHTML = cur
      ? `<div class="row net current">
          ${st.state === 'connecting' ? spinner : bars(st.rssi || -50)}
          <div class="text"><div class="name">${esc(cur)}</div><div class="hint">${wifiStatusLine(st, online)}</div></div>
          ${st.state === 'connected' ? '<div class="btn subtle" data-wifi-off="1">Disconnect</div>' : ''}
          <div class="btn subtle-red" data-forget="${esc(cur)}">Forget</div>
        </div>`
      : '';
    $('#wifiNets').hidden = !on;
    $('#wifiScan').innerHTML = scanning ? spinner + 'Searching' : 'Scan';
    if (!on) return;
    const savedSet = new Set(saved.map((n) => n.ssid));
    const list = aps.filter((a) => a.ssid !== cur)
      .sort((a, b) => (savedSet.has(b.ssid) - savedSet.has(a.ssid)) || b.rssi - a.rssi);
    if (!list.length) {
      $('#wifiList').innerHTML = `<div class="empty">${scanning ? 'Looking for networks…' : 'No networks found'}</div>`;
      return;
    }
    $('#wifiList').innerHTML = list.map((a) => {
      const isSaved = savedSet.has(a.ssid);
      const failed = st.state === 'failed' && st.ssid === a.ssid;
      const hint = failed ? `<span class="err-text">${esc(st.error || 'Couldn\'t connect')}</span>`
        : isSaved ? 'Saved' : a.secure ? 'Secured' : 'Open';
      return `<div class="row net" data-ssid="${esc(a.ssid)}" data-secure="${a.secure ? 1 : 0}">
        ${bars(a.rssi)}
        <div class="text"><div class="name">${esc(a.ssid)}${a.secure ? lock : ''}</div><div class="hint">${hint}</div></div>
        ${isSaved ? `<div class="btn subtle-red small" data-forget="${esc(a.ssid)}">Forget</div>` : ''}
      </div>`;
    }).join('');
  }

  async function join(ssid, pass) {
    joining = ssid;
    try {
      await os.wifi.connect(ssid, pass);  // answers at once; the result comes as a 'net' event
    } catch (e) {
      joining = '';
      toast(e.message, true);
    }
    renderWifi();
  }

  $('#wifiList').addEventListener('click', (e) => {
    const f = e.target.closest('[data-forget]');
    if (f) return forget(f.dataset.forget);
    const row = e.target.closest('.row.net');
    if (!row) return;
    const ssid = row.dataset.ssid;
    if (saved.some((n) => n.ssid === ssid)) join(ssid);  // the OS has its password
    else if (row.dataset.secure === '1') askPassword(ssid);
    else join(ssid, '');
  });
  $('#wifiCurrent').addEventListener('click', async (e) => {
    const f = e.target.closest('[data-forget]');
    if (f) return forget(f.dataset.forget);
    if (e.target.closest('[data-wifi-off]')) {
      await os.wifi.disconnect().catch((err) => toast(err.message, true));
      renderWifi();
    }
  });

  async function forget(ssid) {
    const ok = await os.confirm('Forget network', `Forget "${ssid}"? Its password is removed from this device.`, {ok: 'Forget'});
    if (!ok) return;
    await os.wifi.forget(ssid).catch((err) => toast(err.message, true));
    await loadSaved();
  }

  $('#wifiSwitch').addEventListener('click', async () => {
    const on = !$('#wifiSwitch').classList.contains('on');
    $('#wifiSwitch').classList.toggle('on', on);
    try {
      await os.wifi.power(on);
      if (on) setTimeout(wifiScan, 300);
      else aps = [];
    } catch (e) {
      toast(e.message, true);
    }
    renderWifi();
  });
  $('#wifiScan').addEventListener('click', wifiScan);

  // ---- password sheet
  let sheetSsid = '';
  function askPassword(ssid, error) {
    sheetSsid = ssid;
    $('#pwTitle').textContent = `Connect to "${ssid}"`;
    $('#pwError').textContent = error || '';
    $('#pwInput').value = '';
    $('#pwInput').type = 'password';
    $('#pwEye').classList.remove('on');
    $('#pwSheet').hidden = false;
    updateOk();
    setTimeout(() => $('#pwInput').focus(), 50);
  }
  function closeSheet() {
    $('#pwInput').blur();
    $('#pwSheet').hidden = true;
    sheetSsid = '';
  }
  function updateOk() {
    $('#pwOk').classList.toggle('dis', $('#pwInput').value.length < 8);  // WPA: 8 or more
  }
  $('#pwInput').addEventListener('input', updateOk);
  $('#pwInput').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') submit();
  });
  $('#pwInput').addEventListener('focus', () => $('#pwField').classList.add('focus'));
  $('#pwInput').addEventListener('blur', () => $('#pwField').classList.remove('focus'));
  $('#pwEye').addEventListener('click', () => {
    const shown = $('#pwInput').type === 'text';
    $('#pwInput').type = shown ? 'password' : 'text';
    $('#pwEye').classList.toggle('on', !shown);
  });
  $('#pwCancel').addEventListener('click', closeSheet);
  $('#pwSheet').addEventListener('click', (e) => {
    if (e.target.id === 'pwSheet') closeSheet();  // a tap outside the card
  });
  function submit() {
    const pass = $('#pwInput').value;
    if (pass.length < 8) return;
    const ssid = sheetSsid;
    closeSheet();
    join(ssid, pass);
  }
  $('#pwOk').addEventListener('click', submit);

  // ================================================================ Bluetooth
  let mine = [];          // remembered [{id, name, type}]
  let near = new Map();   // id -> device, this scan
  let btScanning = false;
  let busy = {};          // id -> 'connecting' | 'disconnecting'
  let showUnnamed = false;

  async function loadMine() {
    try {
      mine = await os.bt.devices();
    } catch (e) {
      mine = [];
    }
    renderBt();
  }

  async function btScan() {
    if (btScanning || !registry.get('bt.enabled', true)) return;  // powers up on first use
    btScanning = true;
    near.clear();
    renderBt();
    try {
      const list = await os.bt.scan({ms: 8000});
      for (const d of list) near.set(d.id, d);
    } catch (e) {
      if (visible) toast(e.message, true);
    }
    btScanning = false;
    renderBt();
  }

  function renderBt() {
    if (!visible || view !== 'bt') return;
    const st = os.bt.state() || {};
    const enabled = registry.get('bt.enabled', true);
    const on = !!st.power;
    $('#btSwitch').classList.toggle('on', enabled);
    const conn = new Set((st.connected || []).map((c) => c.id));
    $('#btHint').textContent = !enabled ? 'Off'
      : !on ? 'On · starts when a device is used'
      : conn.size ? `Connected to ${(st.connected || []).map((c) => c.name || c.id).join(', ')}` : 'On';
    $('#btLists').hidden = !enabled;
    if (!enabled) return;
    // remembered devices (connected ones first)
    const mineIds = new Set(mine.map((d) => d.id));
    const rows = [...mine].sort((a, b) => conn.has(b.id) - conn.has(a.id));
    $('#btMine').innerHTML = rows.length ? rows.map((d) => {
      const c = conn.has(d.id), b = busy[d.id];
      const hint = b ? (b === 'connecting' ? 'Connecting…' : 'Disconnecting…') : c ? '<span class="ok-text">Connected</span>' : 'Not connected';
      return `<div class="row dev" data-id="${esc(d.id)}">
        ${b ? spinner : btIcon}
        <div class="text"><div class="name">${esc(d.name || 'Unknown device')}</div><div class="hint">${hint} · ${esc(d.id)}</div></div>
        ${b ? '' : c ? `<div class="btn subtle" data-disc="${esc(d.id)}">Disconnect</div>` : `<div class="btn primary" data-conn="${esc(d.id)}">Connect</div>`}
        <div class="btn subtle-red small" data-btforget="${esc(d.id)}">Forget</div>
      </div>`;
    }).join('') : '<div class="empty">No devices yet: connect one from the list below</div>';
    // nearby devices that are not remembered
    $('#btScan').innerHTML = btScanning ? spinner + 'Searching' : 'Scan';
    const list = [...near.values()].filter((d) => !mineIds.has(d.id)).sort((a, b) => b.rssi - a.rssi);
    const named = list.filter((d) => d.name), unnamed = list.filter((d) => !d.name);
    const shown = showUnnamed ? named.concat(unnamed) : named;
    let html = shown.map((d) => {
      const b = busy[d.id];
      return `<div class="row dev near" data-id="${esc(d.id)}" data-name="${esc(d.name || '')}">
        ${b ? spinner : bars(d.rssi)}
        <div class="text"><div class="name">${esc(d.name || 'Unknown device')}</div><div class="hint">${b ? 'Connecting…' : `${d.rssi} dBm · ${esc(d.id)}`}</div></div>
      </div>`;
    }).join('');
    if (unnamed.length && !showUnnamed)
      html += `<div class="row more" id="btMore"><div class="text"><div class="hint">Show ${unnamed.length} device${unnamed.length > 1 ? 's' : ''} without a name</div></div></div>`;
    $('#btNear').innerHTML = html || `<div class="empty">${btScanning ? 'Looking for devices…' : on ? 'No devices found: tap Scan' : 'Tap Scan to look for devices'}</div>`;
  }

  async function btConnect(id, name) {
    busy[id] = 'connecting';
    renderBt();
    try {
      const d = await os.bt.connect(id);
      toast(`Connected to ${d.name || name || id}`);
    } catch (e) {
      toast(`Couldn't connect: ${e.message}`, true);
    }
    delete busy[id];
    await loadMine();
  }

  $('#btMine').addEventListener('click', async (e) => {
    const c = e.target.closest('[data-conn]');
    if (c) return btConnect(c.dataset.conn);
    const d = e.target.closest('[data-disc]');
    if (d) {
      busy[d.dataset.disc] = 'disconnecting';
      renderBt();
      await os.bt.disconnect(d.dataset.disc).catch((err) => toast(err.message, true));
      delete busy[d.dataset.disc];
      return renderBt();
    }
    const f = e.target.closest('[data-btforget]');
    if (f) {
      const dev = mine.find((x) => x.id === f.dataset.btforget) || {};
      const ok = await os.confirm('Forget device', `Forget "${dev.name || f.dataset.btforget}"? It disconnects and leaves this list.`, {ok: 'Forget'});
      if (!ok) return;
      await os.bt.forget(f.dataset.btforget).catch((err) => toast(err.message, true));
      await loadMine();
    }
  });
  $('#btNear').addEventListener('click', (e) => {
    if (e.target.closest('#btMore')) {
      showUnnamed = true;
      return renderBt();
    }
    const row = e.target.closest('.row.near');
    if (row && !busy[row.dataset.id]) btConnect(row.dataset.id, row.dataset.name);
  });
  $('#btSwitch').addEventListener('click', async () => {
    const on = !registry.get('bt.enabled', true);
    $('#btSwitch').classList.toggle('on', on);
    try {
      await os.bt.power(on);
      if (on) btScan();
      else near.clear();
    } catch (e) {
      toast(e.message, true);
    }
    renderBt();
  });
  $('#btScan').addEventListener('click', btScan);

  // ================================================================ events
  os.net.on(() => {
    const st = os.wifi.state() || {};
    if (joining && st.ssid === joining) {
      if (st.state === 'connected') {
        toast(`Connected to ${joining}`);
        joining = '';
        loadSaved();
      } else if (st.state === 'failed') {
        const ssid = joining;
        joining = '';
        if (st.error === 'wrong password') askPassword(ssid, 'Wrong password, try again');
        else toast(`Couldn't connect to ${ssid}: ${st.error || 'failed'}`, true);
      }
    }
    renderWifi();
    renderBt();
  });
  os.bt.on((e) => {
    if (e.type === 'found' && btScanning) {
      near.set(e.device.id, e.device);
      renderBt();
    } else if (e.type === 'state' || e.type === 'disconnected') {
      renderBt();
    }
  });
  // keep the Wi-Fi list fresh while it is on screen
  setBackgroundProcess('wifi-rescan', 'PAGE', () => {
    if (visible && view === 'wifi' && Date.now() - lastScan > 20000) wifiScan();
  }, 5000);

  function refresh() {
    visible = true;
    if (view === 'wifi') {
      loadSaved();
      if (Date.now() - lastScan > 8000) wifiScan();
      renderWifi();
    } else {
      loadMine();
      if (registry.get('bt.enabled', true) && !btScanning && !near.size) btScan();
      renderBt();
    }
  }
  function hide() {
    visible = false;
  }
  return {refresh, hide, show: (v) => show(v || view)};
})();
