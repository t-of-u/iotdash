// History from data/history.txt, newest first. A page-scope background process keeps the
// "x min ago" labels current; it stops by itself when the page is left.
const list = document.getElementById('list');
const esc = (s) => String(s).replace(/[&<>]/g, (c) => ({'&': '&amp;', '<': '&lt;', '>': '&gt;'}[c]));

function ago(t) {
  const s = Math.max(0, Math.round((Date.now() - t) / 1000));
  if (s < 60) return 'just now';
  if (s < 3600) return Math.floor(s / 60) + ' min ago';
  if (s < 86400) return Math.floor(s / 3600) + ' h ago';
  return new Date(t).toLocaleDateString();
}

function render() {
  const items = Calc.readHistory().reverse();
  list.innerHTML = items.length
    ? items.map((h) => `<div class="item"><div class="calc">${esc(h.text)}</div><div class="when">${ago(h.time)}</div></div>`).join('')
    : '<div class="empty">No calculations yet.</div>';
  // written by the app-scope "stats" process of the main page
  let stats = null;
  try {
    stats = fs.readJSON('stats.json');
  } catch (_) {}
  document.getElementById('stats').textContent = stats ? `${stats.count} total` : '';
}

document.getElementById('back').addEventListener('click', () => navigate(-1));
document.getElementById('clear').addEventListener('click', async () => {
  if (!(await os.confirm('Clear history', 'Delete every saved calculation?', {ok: 'Clear'}))) return;
  fs.remove('history.txt');
  render();
});

setBackgroundProcess('refresh', 'PAGE', () => render(), 15000);
