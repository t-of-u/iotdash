// The app's first page. Pages are plain HTML/CSS/JS; the OS adds its API on top (os, fs, registry,
// navigate, setBackgroundProcess, ...: see the SDK's docs/reference/js-api.html).
const $ = (s) => document.querySelector(s);

// localStorage is the app's memory.json on the card: it survives restarts and updates
let count = +(localStorage.getItem('count') || 0);
const show = () => { $('#count').textContent = String(count); localStorage.setItem('count', String(count)); };
$('#plus').addEventListener('click', () => { count++; show(); });
$('#minus').addEventListener('click', () => { count--; show(); });
show();

$('#app').textContent = `${os.page().title} ${os.page().version}`;
$('#os').textContent = os.version;
$('#about').addEventListener('click', () => navigate('pages/about.html'));

// Wi-Fi and internet as the OS sees them (the board joins saved networks by itself)
function net() {
  const s = os.net.state();
  const up = s.wifi && s.wifi.state === 'connected';
  $('#net').classList.toggle('on', up && s.internet === true);
  $('#netText').textContent = !up ? 'Offline' : s.internet === false ? `${s.wifi.ssid} · no internet` : s.wifi.ssid;
}
os.net.on(net);
net();

// a process of this page: every second while it is open
setBackgroundProcess('clock', 'PAGE', () => {
  const d = new Date();
  $('#clock').textContent = d.toTimeString().slice(0, 8);
}, 1000);
