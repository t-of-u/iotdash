const $ = (s) => document.querySelector(s);
let count = Number(localStorage.getItem('count') || 0);
const countEl = $('#count');
function render() {
  countEl.textContent = count;
  countEl.classList.add('bump');
  setTimeout(() => countEl.classList.remove('bump'), 250);
  localStorage.setItem('count', count);
}
$('#inc').addEventListener('click', () => { count++; render(); });
$('#dec').addEventListener('click', () => { count--; render(); });
render();

$('#toggle').addEventListener('change', (e) => document.body.classList.toggle('dark', e.target.checked));
$('#name').addEventListener('input', (e) => { $('#hello').textContent = 'สวัสดี ' + e.target.value; });
$('#level').addEventListener('input', (e) => { $('#prog').value = e.target.value; });

function tick() {
  const d = new Date(1730000000000 + performance.now());
  $('#clock').textContent = d.toISOString().slice(11, 19);
}
setInterval(tick, 1000); tick();

fetch('data.json').then((r) => r.json()).then((data) => {
  $('#status').textContent = data.sensors.length + ' รายการ';
  $('#list').innerHTML = data.sensors.map((s) =>
    `<li class="${s.alert ? 'alert' : ''}"><span>${s.name}</span><b>${s.value} ${s.unit}</b></li>`).join('');
}).catch((e) => { $('#status').textContent = 'error: ' + e.message; });

const c = $('#chart'), g = c.getContext('2d');
const pts = [12, 18, 15, 22, 30, 26, 34, 31, 40, 37, 45];
function draw(shift) {
  g.clearRect(0, 0, c.width, c.height);
  const grad = g.createLinearGradient(0, 0, 0, c.height);
  grad.addColorStop(0, 'rgba(37,99,235,.35)'); grad.addColorStop(1, 'rgba(37,99,235,0)');
  const xs = (i) => 6 + i * (c.width - 12) / (pts.length - 1), ys = (v) => c.height - 8 - v * 2;
  g.beginPath(); g.moveTo(xs(0), ys(pts[0]));
  pts.forEach((v, i) => i && g.lineTo(xs(i), ys(v + Math.sin(i + shift) * 3)));
  g.lineTo(xs(pts.length - 1), c.height); g.lineTo(xs(0), c.height); g.closePath();
  g.fillStyle = grad; g.fill();
  g.beginPath(); pts.forEach((v, i) => (i ? g.lineTo : g.moveTo).call(g, xs(i), ys(v + Math.sin(i + shift) * 3)));
  g.strokeStyle = '#2563eb'; g.lineWidth = 2.5; g.lineJoin = 'round'; g.stroke();
  g.fillStyle = '#0f172a'; g.font = '600 12px Inter'; g.fillText('kWh', 8, 16);
}
let t = 0;
draw(0);
console.log('app ready', typeof Path2D, typeof customElements);
