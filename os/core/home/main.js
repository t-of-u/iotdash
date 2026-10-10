// Home screen: installed apps in pages of 10 (swipe sideways), a clock, page dots.
const PER_PAGE = 10;
const pager = document.getElementById('pager');
const dots = document.getElementById('dots');

const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;'}[c]));

function render() {
  const apps = os.apps();
  const pages = Math.max(1, Math.ceil(apps.length / PER_PAGE));
  let html = '';
  for (let p = 0; p < pages; p++) {
    html += '<div class="page">';
    for (const a of apps.slice(p * PER_PAGE, (p + 1) * PER_PAGE))
      html += `<div class="app" data-id="${esc(a.id)}"><img class="icon" src="${esc(a.icon)}"><div class="label">${esc(a.title)}</div></div>`;
    if (!apps.some((a) => !a.system) && p === pages - 1)
      html += '<div class="empty"><h2>No apps yet</h2><p>Install apps from your computer:<br>' +
        '<code>iotdash run --device</code> or the Apps store</p></div>';
    html += '</div>';
  }
  pager.innerHTML = html;
  dots.innerHTML = pages > 1 ? '<div class="dot"></div>'.repeat(pages) : '';
  // come back to the page the user was on
  const last = Math.min(+localStorage.getItem('homePage') || 0, pages - 1);
  pager.scrollLeft = last * 800;
  markDot(last);
}

function markDot(i) {
  dots.querySelectorAll('.dot').forEach((d, k) => d.classList.toggle('on', k === i));
}

pager.addEventListener('scroll', () => {
  const i = Math.round(pager.scrollLeft / 800);
  markDot(i);
  localStorage.setItem('homePage', String(i));
});

pager.addEventListener('click', (e) => {
  const app = e.target.closest('.app');
  if (!app) return;
  try {
    os.launch(app.dataset.id);
  } catch (err) {
    os.alert('Cannot open', err.message);
  }
});

// ---- clock: a page-scoped background process (stops when an app opens)
const DAYS = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
const MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
const pad = (n) => String(n).padStart(2, '0');
function tickClock() {
  const d = new Date();
  const h24 = registry.get('clock24', true);
  let h = d.getHours();
  let suffix = '';
  if (!h24) {
    suffix = h < 12 ? ' AM' : ' PM';
    h = h % 12 || 12;
  }
  document.getElementById('time').textContent = (h24 ? pad(h) : h) + ':' + pad(d.getMinutes()) + suffix;
  document.getElementById('date').textContent = `${DAYS[d.getDay()]} ${d.getDate()} ${MONTHS[d.getMonth()]}`;
  return 1000 - (Date.now() % 1000) + 5;  // next run just after the minute digit can change
}
setBackgroundProcess('clock', 'PAGE', tickClock);

registry.onChange((e) => {
  if (e.key === 'clock24' || e.key === null) tickClock();
});

os.on('apps', render);  // installed or removed over USB

render();
