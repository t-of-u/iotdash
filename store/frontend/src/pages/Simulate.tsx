// One version of an app running on a simulated board: the iotdash OS (WebAssembly, public/iotdash)
// with the package installed, a simulated Wi-Fi network and BLE devices, and a weighing scale to
// play with. Built from the SDK's web/ (iotdash-web.js).
import { useCallback, useEffect, useRef, useState } from 'react';
import { Link, useParams } from 'react-router-dom';
import ScaleGame, { type Reading, type ScaleHandle } from '../components/ScaleGame';
import { AppIcon, Icon } from '../components/ui';
import { projects, type Project, type Version } from '../lib/api';
import { dateTime, size } from '../lib/format';

interface Sim {
  init(): Promise<unknown>; installPackage(pkg: unknown): { id: string; version: string }; boot(): void; launch(id: string): void;
  home(): void; stop(): void; setInternet(on: boolean): void; setWifiReach(on: boolean): void;
  scale(r: { kg: number; stable: boolean; overload: boolean; intervalMs?: number }): void; scalePower(on: boolean): void;
}
interface SimModule {
  IotdashSim: new (o: { canvas: HTMLCanvasElement; base: string; onLog: (s: string) => void; onRadio: (n: string, s: unknown) => void; onTare: () => void }) => Sim;
  parseIdbz(b: ArrayBuffer): { manifest: { id: string; version: string; title?: string } };
  verifyIdbz(p: unknown): Promise<boolean>;
}
interface WifiState { power?: boolean; state?: string; ssid?: string; ip?: string; rssi?: number; error?: string }
interface BtState { type?: string; power?: boolean; scanning?: boolean; connected?: { id: string; name: string }[] }

const ENGINE = '/iotdash/';
const SCALE_ID = 'c8:2e:18:a4:01:5d';
const OTHER_BT = [
  { name: 'Mi Smart Band 7', note: 'battery service' },
  { name: 'JBL Flip 6', note: 'nothing to read' },
];

export default function Simulate() {
  const { pid = '', vid = '' } = useParams();
  const screen = useRef<HTMLCanvasElement>(null);
  const sim = useRef<Sim | null>(null);
  const scaleRef = useRef<ScaleHandle>(null);
  const [p, setP] = useState<Project | null>(null);
  const [v, setV] = useState<Version | null>(null);
  const [phase, setPhase] = useState<'load' | 'boot' | 'run' | 'error'>('load');
  const [status, setStatus] = useState('Loading the package…');
  const [logs, setLogs] = useState<string[]>([]);
  const [wifi, setWifi] = useState<WifiState>({});
  const [bt, setBt] = useState<BtState>({});
  const [reach, setReach] = useState(true);
  const [internet, setInternet] = useState(true);
  const [scaleOn, setScaleOn] = useState(true);
  const [reading, setReading] = useState<Reading | null>(null);
  const [lastNotify, setLastNotify] = useState(0);
  const [run, setRun] = useState(0);
  const appId = useRef('');

  const log = useCallback((s: string) => setLogs((l) => (l.length > 300 ? l.slice(-250) : l).concat(s)), []);

  useEffect(() => {
    let gone = false;
    let s: Sim | null = null;
    setPhase('load');
    setLogs([]);
    (async () => {
      const [proj, ver] = await Promise.all([projects.get(pid), projects.version(pid, vid)]);
      if (gone) return;
      setP(proj);
      setV(ver);
      if (!ver.package_url) throw new Error('This version has no package to run');
      setStatus('Starting the engine…');
      const mod = (await import(/* @vite-ignore */ `${ENGINE}iotdash-web.js`)) as SimModule;
      const res = await fetch(ver.package_url);
      if (!res.ok) throw new Error(`The package cannot be downloaded (${res.status})`);
      const pkg = mod.parseIdbz(await res.arrayBuffer());
      await mod.verifyIdbz(pkg);
      s = new mod.IotdashSim({
        canvas: screen.current!, base: ENGINE, onLog: log,
        onRadio: (name, st) => {
          if (gone) return;
          if (name === 'wifi') setWifi(st as WifiState);
          else if (name === 'bt') {
            const b = st as BtState & { type?: string; id?: string };
            if (b.type === 'state') setBt(b);
            else if (b.type === 'notify' && b.id === SCALE_ID) setLastNotify(Date.now());
          }
        },
        onTare: () => scaleRef.current?.tare(),
      });
      await s.init();
      if (gone) return s.stop();
      sim.current = s;
      appId.current = s.installPackage(pkg).id;
      s.setInternet(internet);
      s.setWifiReach(reach);
      s.scalePower(scaleOn);
      setStatus('Booting iotdash OS…');
      setPhase('boot');
      s.boot();
      setTimeout(() => {
        if (gone) return;
        try {
          s!.launch(appId.current);
          setPhase('run');
        } catch (e) {
          setStatus((e as Error).message);
          setPhase('error');
        }
      }, 1300);
    })().catch((e) => {
      if (gone) return;
      setStatus((e as Error).message);
      setPhase('error');
    });
    return () => {
      gone = true;
      s?.stop();
      sim.current = null;
    };
    // the world switches apply to the running sim below; a restart takes their current values
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [pid, vid, run, log]);

  const onReading = useCallback((r: Reading) => {
    setReading(r);
    sim.current?.scale({ kg: r.kg, stable: r.stable, overload: r.overload, intervalMs: 200 });
  }, []);

  const scaleLinked = !!bt.connected?.some((c) => c.id === SCALE_ID);
  const streaming = scaleLinked && Date.now() - lastNotify < 1500;
  const wifiText = !wifi.power ? 'Wi-Fi off' : wifi.state === 'connected' ? `Connected to ${wifi.ssid} · ${wifi.ip}` : wifi.state === 'connecting' ? `Connecting to ${wifi.ssid}…` : wifi.error || 'Not connected';

  return (
    <>
      <div className="crumbs">
        <Link to="/">Projects</Link><span>/</span>
        {p ? <Link to={`/projects/${pid}`}>{p.name}</Link> : <span>…</span>}<span>/</span>
        <span>Simulate {v?.version || ''}</span>
      </div>
      <div className="head" style={{ marginBottom: 16 }}>
        {p && <AppIcon src={v?.icon_png_url || p.icon_png_url} name={p.name} />}
        <div style={{ flex: 1, minWidth: 0 }}>
          <h1 style={{ fontSize: 22 }}>{p?.name || 'Simulator'} <span className="chip blue" style={{ verticalAlign: 3 }}>{v?.version}</span></h1>
          <div className="sub">{v ? `Uploaded ${dateTime(v.created_date)} · ${size(v.size)} · iotdash OS ${v.os_min}+ (built with ${v.os_built})` : 'Loading…'}</div>
        </div>
        <Link to={`/projects/${pid}`} className="btn ghost">{Icon.back}Back to project</Link>
      </div>

      <div className="sim">
        <div>
          <div className="device">
            <div className="device-screen">
              <canvas ref={screen} width={800} height={480} />
              {phase !== 'run' && (
                <div className="device-overlay">
                  <div>
                    {phase !== 'error' && <div className="spinner" style={{ margin: '0 auto 12px', borderColor: '#334155', borderTopColor: '#60a5fa' }} />}
                    <div style={{ color: phase === 'error' ? '#fca5a5' : undefined }}>{status}</div>
                  </div>
                </div>
              )}
            </div>
            <div className="device-bar">
              <span className={`led${phase === 'run' ? ' on' : phase === 'error' ? '' : ' warn'}`} />
              <span>{phase === 'run' ? 'Running' : phase === 'error' ? 'Stopped' : 'Starting'}</span>
              <span style={{ color: '#475569' }}>·</span>
              <span>800 × 480 touch screen</span>
              <div className="grow" />
              <button className="btn ghost sm" disabled={phase !== 'run'} onClick={() => sim.current?.home()}>{Icon.home}Home</button>
              <button className="btn ghost sm" disabled={phase !== 'run'} onClick={() => { try { sim.current?.launch(appId.current); } catch { /* already open */ } }}>{Icon.play}Open app</button>
              <button className="btn ghost sm" onClick={() => setRun((n) => n + 1)}>{Icon.refresh}Restart</button>
            </div>
          </div>
          <details className="card panel" style={{ marginTop: 16 }}>
            <summary style={{ cursor: 'pointer', fontWeight: 600, fontSize: 14 }}>Console ({logs.length})</summary>
            <pre className="log" style={{ marginTop: 10 }}>{logs.join('\n') || 'Nothing yet'}</pre>
          </details>
        </div>

        <div className="side">
          <div className="card panel">
            <h3>Wi-Fi</h3>
            <div className="prow">
              {Icon.wifi}
              <div className="t"><b>Access point “Home-WiFi”</b><span>{reach ? 'In range (saved on the board)' : 'Out of range'}</span></div>
              <button className={`switch${reach ? ' on' : ''}`} onClick={() => { setReach(!reach); sim.current?.setWifiReach(!reach); }} />
            </div>
            <div className="prow">
              {Icon.globe}
              <div className="t"><b>Internet</b><span>{internet ? 'The network reaches the internet' : 'Network up, no internet'}</span></div>
              <button className={`switch${internet ? ' on' : ''}`} onClick={() => { setInternet(!internet); sim.current?.setInternet(!internet); }} />
            </div>
            <div className="prow">
              {Icon.dot}
              <div className="t"><b>Board</b><span>{wifiText}</span></div>
              <span className={`led${wifi.state === 'connected' ? ' on' : wifi.state === 'connecting' ? ' warn' : ''}`} />
            </div>
            <div className="sub" style={{ fontSize: 12, marginTop: 6 }}>
              Other networks the board can join from Settings: TopuTofu (<code>tofu1234</code>), IoT-Lab (<code>iotlab99</code>), Cafe Free WiFi (open).
              The app's own HTTP requests go out from this browser.
            </div>
          </div>

          <div className="card panel">
            <h3>Bluetooth devices <span className="grow" /><span className={`chip${bt.power ? ' ok' : ''}`}>{bt.power ? (bt.scanning ? 'Scanning' : 'On') : 'Off'}</span></h3>
            <div className="prow">
              {Icon.scale}
              <div className="t"><b>BLE-Scale-01 · weighing scale</b>
                <span>{!scaleOn ? 'Switched off' : scaleLinked ? (streaming ? 'Connected · sending readings' : 'Connected') : 'Advertising, not connected'}</span></div>
              <span className={`led${scaleOn && scaleLinked ? ' on' : ''}`} />
              <button className={`switch${scaleOn ? ' on' : ''}`} onClick={() => { setScaleOn(!scaleOn); sim.current?.scalePower(!scaleOn); }} />
            </div>
            {OTHER_BT.map((d) => (
              <div className="prow" key={d.name}>
                {Icon.bt}
                <div className="t"><b>{d.name}</b><span>Nearby · {d.note}</span></div>
              </div>
            ))}
          </div>

          <div className="card panel">
            <h3>Scale <span className="grow" />
              {reading && scaleOn && <span className={`chip${reading.stable ? ' ok' : ' warn'}`}>{reading.overload ? 'OL' : `${reading.kg.toFixed(2)} kg`}{reading.stable ? ' · stable' : ''}</span>}
            </h3>
            <ScaleGame ref={scaleRef} powered={scaleOn} onReading={onReading} />
            <div className="scale-tools">
              <div className="sub">Drag from the shelf, throw things on, take them off. Drop on the shelf to put away.</div>
              <button className="btn ghost sm" onClick={() => scaleRef.current?.tare()}>Tare</button>
              <button className="btn ghost sm" onClick={() => scaleRef.current?.clear()}>Clear</button>
            </div>
          </div>
        </div>
      </div>
    </>
  );
}
