// A platform scale on a canvas: drag materials from the shelf onto it. The plate is a damped spring,
// so the reading climbs, overshoots a little, wobbles and settles as on a real load cell; the
// display says STABLE once it has stopped moving. Every 200 ms the reading goes to the simulated
// BLE scale (onReading), which the app on the board reads like the real one.
import { forwardRef, useEffect, useImperativeHandle, useRef } from 'react';

export interface Reading { kg: number; stable: boolean; overload: boolean; gross: number }
export interface ScaleHandle { tare(): void; zero(): void; clear(): void }

const W = 380, H = 340;
const FLOOR = H - 20;
const PL = 66, PR = 314;           // the plate
const BASE_L = 78, BASE_R = 302;   // the body under it
const PLATE_T = 9;
const PLATE_Y0 = FLOOR - 70;       // plate top with nothing on it
const SHELF_Y = 88;                // shelf plank top
const SHELF_SCALE = 0.55;
const G = 2300;                    // px/s²
const D = 0.01;                    // display division (kg): the BLE frame has two decimals
const CAPACITY = 60;

type Group = 'Plastic' | 'Glass' | 'Paper' | 'Metal';
interface Mat { kind: string; name: string; group: Group; kg: number; w: number; h: number; draw: (c: CanvasRenderingContext2D, w: number, h: number) => void }
interface Body { id: number; mat: Mat; x: number; y: number; vx: number; vy: number; kg: number; resting: boolean; support: 'floor' | 'plate' | Body | null; bounced: boolean }

// ---------------------------------------------------------------- materials (drawn around 0,0)
function rr(c: CanvasRenderingContext2D, x: number, y: number, w: number, h: number, r: number) {
  c.beginPath();
  c.roundRect(x, y, w, h, r);
}
function shine(c: CanvasRenderingContext2D, x: number, y: number, h: number, a = 0.5) {
  c.strokeStyle = `rgba(255,255,255,${a})`;
  c.lineWidth = 2;
  c.lineCap = 'round';
  c.beginPath();
  c.moveTo(x, y);
  c.lineTo(x, y + h);
  c.stroke();
}
function bottle(c: CanvasRenderingContext2D, w: number, h: number, neckW: number, neckH: number) {
  const top = -h / 2, sh = top + neckH, bot = h / 2;
  c.beginPath();
  c.moveTo(-neckW / 2, top + h * 0.06);
  c.lineTo(-neckW / 2, sh - h * 0.04);
  c.bezierCurveTo(-neckW / 2, sh + h * 0.06, -w / 2, sh + h * 0.04, -w / 2, sh + h * 0.14);
  c.lineTo(-w / 2, bot - 4);
  c.quadraticCurveTo(-w / 2, bot, -w / 2 + 4, bot);
  c.lineTo(w / 2 - 4, bot);
  c.quadraticCurveTo(w / 2, bot, w / 2, bot - 4);
  c.lineTo(w / 2, sh + h * 0.14);
  c.bezierCurveTo(w / 2, sh + h * 0.04, neckW / 2, sh + h * 0.06, neckW / 2, sh - h * 0.04);
  c.lineTo(neckW / 2, top + h * 0.06);
  c.closePath();
}
const MATS: Mat[] = [
  { kind: 'pet', name: 'PET bottle 600 ml', group: 'Plastic', kg: 0.022, w: 24, h: 66, draw(c, w, h) {
    bottle(c, w, h, w * 0.46, h * 0.3);
    c.fillStyle = 'rgba(147,197,253,.55)';
    c.fill();
    c.strokeStyle = 'rgba(37,99,235,.75)';
    c.lineWidth = 1.2;
    c.stroke();
    c.fillStyle = '#2563eb';
    rr(c, -w * 0.25, -h / 2, w * 0.5, h * 0.09, 2);
    c.fill();
    c.fillStyle = '#ef4444';
    c.fillRect(-w / 2, h * 0.02, w, h * 0.17);
    c.fillStyle = 'rgba(255,255,255,.85)';
    c.fillRect(-w / 2 + 4, h * 0.08, w - 8, 2);
    shine(c, -w / 2 + 5, -h * 0.12, h * 0.1);
    shine(c, -w / 2 + 5, h * 0.24, h * 0.18);
  } },
  { kind: 'hdpe', name: 'HDPE milk jug', group: 'Plastic', kg: 0.06, w: 40, h: 54, draw(c, w, h) {
    c.fillStyle = '#f8fafc';
    c.strokeStyle = '#cbd5e1';
    c.lineWidth = 1.2;
    rr(c, -w / 2, -h / 2 + h * 0.22, w, h * 0.78, 6);
    c.fill();
    c.stroke();
    c.beginPath();  // handle
    c.moveTo(w * 0.05, -h / 2 + h * 0.24);
    c.quadraticCurveTo(w * 0.55, -h / 2 - 2, w / 2 - 2, -h / 2 + h * 0.45);
    c.lineWidth = 5;
    c.strokeStyle = '#e2e8f0';
    c.stroke();
    c.fillStyle = '#f8fafc';
    rr(c, -w * 0.38, -h / 2 + h * 0.08, w * 0.34, h * 0.18, 3);
    c.fill();
    c.fillStyle = '#16a34a';
    rr(c, -w * 0.36, -h / 2, w * 0.3, h * 0.1, 2);
    c.fill();
    c.fillStyle = '#3b82f6';
    c.fillRect(-w / 2 + 3, h * 0.08, w - 6, h * 0.2);
    c.fillStyle = '#fff';
    c.font = '600 8px Inter, sans-serif';
    c.textAlign = 'center';
    c.fillText('MILK', 0, h * 0.21);
  } },
  { kind: 'glass-bottle', name: 'Glass bottle', group: 'Glass', kg: 0.36, w: 26, h: 78, draw(c, w, h) {
    bottle(c, w, h, w * 0.36, h * 0.36);
    c.fillStyle = 'rgba(21,128,61,.82)';
    c.fill();
    c.strokeStyle = '#14532d';
    c.lineWidth = 1.2;
    c.stroke();
    c.fillStyle = '#ca8a04';
    rr(c, -w * 0.21, -h / 2, w * 0.42, h * 0.06, 1.5);
    c.fill();
    c.fillStyle = '#fef3c7';
    c.fillRect(-w / 2, h * 0.04, w, h * 0.22);
    c.fillStyle = '#b45309';
    c.fillRect(-w / 2 + 4, h * 0.11, w - 8, 3);
    shine(c, -w / 2 + 5, -h * 0.02, h * 0.04, 0.45);
    shine(c, -w / 2 + 5, h * 0.3, h * 0.14, 0.45);
  } },
  { kind: 'jar', name: 'Glass jar', group: 'Glass', kg: 0.22, w: 36, h: 44, draw(c, w, h) {
    c.fillStyle = 'rgba(186,230,253,.45)';
    c.strokeStyle = 'rgba(14,116,144,.7)';
    c.lineWidth = 1.3;
    rr(c, -w / 2, -h / 2 + 7, w, h - 7, 7);
    c.fill();
    c.stroke();
    c.fillStyle = '#d4a017';
    rr(c, -w / 2 + 2, -h / 2, w - 4, 9, 2);
    c.fill();
    c.strokeStyle = 'rgba(0,0,0,.2)';
    c.beginPath();
    for (let x = -w / 2 + 6; x < w / 2 - 3; x += 4) { c.moveTo(x, -h / 2 + 1); c.lineTo(x, -h / 2 + 8); }
    c.stroke();
    shine(c, -w / 2 + 6, -h / 2 + 14, h * 0.5, 0.6);
  } },
  { kind: 'newspaper', name: 'Newspaper bundle', group: 'Paper', kg: 0.8, w: 66, h: 24, draw(c, w, h) {
    for (let i = 0; i < 4; i++) {
      const y = -h / 2 + i * (h / 4);
      c.fillStyle = i % 2 ? '#e5e7eb' : '#f3f4f6';
      c.strokeStyle = '#9ca3af';
      c.lineWidth = 0.8;
      rr(c, -w / 2 + (i % 2 ? 1 : 0), y, w - 1, h / 4, 1.5);
      c.fill();
      c.stroke();
      c.strokeStyle = 'rgba(55,65,81,.35)';
      c.beginPath();
      c.moveTo(-w / 2 + 6, y + h / 8);
      c.lineTo(-w / 2 + 6 + (w - 30) * (0.5 + 0.5 * ((i * 7) % 3) / 2), y + h / 8);
      c.stroke();
    }
    c.strokeStyle = '#a16207';
    c.lineWidth = 2;
    c.beginPath();
    c.moveTo(w * 0.18, -h / 2);
    c.lineTo(w * 0.18, h / 2);
    c.stroke();
  } },
  { kind: 'box', name: 'Cardboard box', group: 'Paper', kg: 0.35, w: 58, h: 42, draw(c, w, h) {
    c.fillStyle = '#c4915a';
    c.strokeStyle = '#8a5a2b';
    c.lineWidth = 1.2;
    rr(c, -w / 2, -h / 2, w, h, 2);
    c.fill();
    c.stroke();
    c.fillStyle = '#e8c895';
    c.fillRect(-5, -h / 2, 10, h);
    c.strokeStyle = 'rgba(138,90,43,.6)';
    c.beginPath();
    c.moveTo(-w / 2, -h / 2 + 8);
    c.lineTo(w / 2, -h / 2 + 8);
    c.stroke();
    c.fillStyle = 'rgba(80,50,20,.55)';
    c.font = '700 7px Inter, sans-serif';
    c.textAlign = 'left';
    c.fillText('↑↑', -w / 2 + 5, h / 2 - 6);
  } },
  { kind: 'alcan', name: 'Aluminium can', group: 'Metal', kg: 0.015, w: 20, h: 36, draw(c, w, h) {
    const g = c.createLinearGradient(-w / 2, 0, w / 2, 0);
    g.addColorStop(0, '#9ca3af');
    g.addColorStop(0.35, '#f3f4f6');
    g.addColorStop(1, '#6b7280');
    c.fillStyle = g;
    rr(c, -w / 2, -h / 2 + 2, w, h - 2, 3);
    c.fill();
    c.fillStyle = '#dc2626';
    c.fillRect(-w / 2, -h / 2 + h * 0.3, w, h * 0.4);
    c.fillStyle = '#fff';
    c.fillRect(-w / 2, -h / 2 + h * 0.47, w, 2);
    c.fillStyle = '#d1d5db';
    rr(c, -w / 2 + 2, -h / 2, w - 4, 4, 2);
    c.fill();
  } },
  { kind: 'tincan', name: 'Steel food can', group: 'Metal', kg: 0.07, w: 28, h: 34, draw(c, w, h) {
    const g = c.createLinearGradient(-w / 2, 0, w / 2, 0);
    g.addColorStop(0, '#78716c');
    g.addColorStop(0.4, '#d6d3d1');
    g.addColorStop(1, '#57534e');
    c.fillStyle = g;
    rr(c, -w / 2, -h / 2, w, h, 2);
    c.fill();
    c.strokeStyle = 'rgba(41,37,36,.35)';
    c.lineWidth = 1;
    c.beginPath();
    for (let y = -h / 2 + 7; y < h / 2 - 4; y += 5) { c.moveTo(-w / 2, y); c.lineTo(w / 2, y); }
    c.stroke();
    c.fillStyle = '#a8a29e';
    c.fillRect(-w / 2 - 1, -h / 2, w + 2, 3);
    c.fillRect(-w / 2 - 1, h / 2 - 3, w + 2, 3);
  } },
  { kind: 'scrap', name: 'Scrap iron bar', group: 'Metal', kg: 2.4, w: 80, h: 16, draw(c, w, h) {
    const g = c.createLinearGradient(0, -h / 2, 0, h / 2);
    g.addColorStop(0, '#71717a');
    g.addColorStop(0.5, '#52525b');
    g.addColorStop(1, '#3f3f46');
    c.fillStyle = g;
    rr(c, -w / 2, -h / 2, w, h, 2);
    c.fill();
    c.fillStyle = 'rgba(146,64,14,.7)';
    for (const [x, y, r] of [[-24, -2, 3.5], [8, 3, 2.5], [28, -3, 3], [-8, 4, 2]]) { c.beginPath(); c.arc(x, y, r, 0, 7); c.fill(); }
    c.fillStyle = '#27272a';
    for (const x of [-32, 32]) { c.beginPath(); c.arc(x, 0, 2.6, 0, 7); c.fill(); }
    shine(c, -w / 2 + 3, -h / 2 + 2, 0, 0);
  } },
];

// shelf slots: the materials laid out at SHELF_SCALE, groups captioned under the plank
const SLOTS = (() => {
  const gap = 13;
  const total = MATS.reduce((s, m) => s + m.w * SHELF_SCALE, 0) + gap * (MATS.length - 1);
  let x = (W - total) / 2;
  return MATS.map((m) => {
    const w = m.w * SHELF_SCALE;
    const s = { mat: m, x: x + w / 2, w, h: m.h * SHELF_SCALE };
    x += w + gap;
    return s;
  });
})();

// ---------------------------------------------------------------- seven segments
const SEGS: Record<string, string> = {
  '0': 'abcdef', '1': 'bc', '2': 'abged', '3': 'abgcd', '4': 'fgbc', '5': 'afgcd', '6': 'afgedc', '7': 'abc', '8': 'abcdefg',
  '9': 'abcdfg', '-': 'g', 'O': 'abcdef', 'L': 'fed', ' ': '',
};
function digit(c: CanvasRenderingContext2D, ch: string, x: number, y: number, w: number, h: number, on: string, off: string) {
  const t = 3.2, half = h / 2;
  const seg: Record<string, [number, number, number, number]> = {
    a: [x + t, y, w - 2 * t, t], d: [x + t, y + h - t, w - 2 * t, t], g: [x + t, y + half - t / 2, w - 2 * t, t],
    f: [x, y + t, t, half - 1.5 * t], b: [x + w - t, y + t, t, half - 1.5 * t],
    e: [x, y + half + t / 2, t, half - 1.5 * t], c: [x + w - t, y + half + t / 2, t, half - 1.5 * t],
  };
  const lit = SEGS[ch] ?? '';
  for (const pass of [false, true]) {  // unlit ghosts, then the lit segments with their glow
    c.shadowBlur = pass ? 6 : 0;
    c.fillStyle = pass ? on : off;
    for (const k of 'abcdefg') {
      if (lit.includes(k) !== pass) continue;
      const [sx, sy, sw, sh] = seg[k];
      rr(c, sx, sy, sw, sh, 1.2);
      c.fill();
    }
  }
  c.shadowBlur = 0;
}

// ---------------------------------------------------------------- the component
export default forwardRef<ScaleHandle, { powered: boolean; onReading: (r: Reading) => void }>(function ScaleGame({ powered, onReading }, ref) {
  const canvas = useRef<HTMLCanvasElement>(null);
  const live = useRef({ powered, onReading });
  live.current = { powered, onReading };
  const world = useRef({
    bodies: [] as Body[], nextId: 1, drag: null as Body | null, grab: { dx: 0, dy: 0, lx: 0, ly: 0, lt: 0 },
    r: 0, v: 0, still: 0, stable: true, noise: 0, tare: 0, shown: 0, hover: null as Mat | null, hx: 0, hy: 0, press: '' as string,
  });

  useImperativeHandle(ref, () => ({
    tare() { const s = world.current; s.tare = s.shown; },
    zero() { world.current.tare = 0; },
    clear() { world.current.bodies = []; world.current.drag = null; },
  }), []);

  useEffect(() => {
    const cv = canvas.current!;
    const c = cv.getContext('2d')!;
    const s = world.current;
    let raf = 0, last = performance.now(), feedAt = 0, dpr = 0;
    const dark = matchMedia('(prefers-color-scheme: dark)');
    if (import.meta.env.DEV) (window as unknown as { __scale: typeof s }).__scale = s;  // tests

    const plateTop = () => PLATE_Y0 + Math.min(7, Math.max(-2, s.r * 0.22));
    const top = (b: Body) => b.y - b.mat.h / 2;
    const overlap = (a: Body, b: Body) => Math.min(a.x + a.mat.w / 2, b.x + b.mat.w / 2) - Math.max(a.x - a.mat.w / 2, b.x - b.mat.w / 2);
    const onPlate = (b: Body): boolean => {
      for (let x: Body | 'floor' | 'plate' | null = b, n = 0; x && n < 50; n++) {
        if (x === 'plate') return true;
        if (x === 'floor') return false;
        if (!x.resting) return false;
        x = x.support;
      }
      return false;
    };
    const load = () => s.bodies.reduce((sum, b) => (b !== s.drag && b.resting && onPlate(b) ? sum + b.kg : sum), 0);

    function supportOk(b: Body) {
      const sp = b.support;
      if (sp === 'floor') return true;
      if (sp === 'plate') return b.x >= PL - 2 && b.x <= PR + 2;
      if (!sp || sp === s.drag || !sp.resting || !s.bodies.includes(sp)) return false;
      return overlap(b, sp) > 0.3 * Math.min(b.mat.w, sp.mat.w);
    }
    function supportTop(b: Body): number {
      const sp = b.support;
      return sp === 'floor' ? FLOOR : sp === 'plate' ? plateTop() : sp ? top(sp) : FLOOR;
    }
    // the highest surface under a falling body (pushing it off an edge it only grazes)
    function surface(b: Body, bottom: number): { y: number; sp: Body['support'] } {
      const left = b.x - b.mat.w / 2, right = b.x + b.mat.w / 2;
      if (bottom <= plateTop() + 1 && right > PL && left < PR && (b.x < PL || b.x > PR)) b.x = b.x < PL ? PL - b.mat.w / 2 - 1 : PR + b.mat.w / 2 + 1;
      let best: { y: number; sp: Body['support'] } = { y: FLOOR, sp: 'floor' };
      if (b.x >= PL && b.x <= PR && plateTop() >= bottom - 1 && plateTop() < best.y) best = { y: plateTop(), sp: 'plate' };
      for (const o of s.bodies) {
        if (o === b || o === s.drag || !o.resting || top(o) < bottom - 1) continue;
        const ov = overlap(b, o);
        if (ov <= 0) continue;
        if (ov < 0.3 * Math.min(b.mat.w, o.mat.w)) {  // grazing: slide off its side
          b.x = b.x < o.x ? o.x - o.mat.w / 2 - b.mat.w / 2 - 1 : o.x + o.mat.w / 2 + b.mat.w / 2 + 1;
          b.x = Math.max(b.mat.w / 2, Math.min(W - b.mat.w / 2, b.x));
          continue;
        }
        if (top(o) < best.y) best = { y: top(o), sp: o };
      }
      return best;
    }

    function step(dt: number) {
      // bodies
      for (const b of s.bodies) {
        if (b === s.drag) continue;
        if (b.resting) {
          if (supportOk(b)) continue;
          b.resting = false;
          b.support = null;
          b.bounced = true;
        }
        b.vy += G * dt;
        b.vx *= Math.pow(0.15, dt);
        b.x = Math.max(b.mat.w / 2, Math.min(W - b.mat.w / 2, b.x + b.vx * dt));
        const bottom = b.y + b.mat.h / 2;
        const sf = surface(b, bottom);
        if (bottom + b.vy * dt >= sf.y) {
          const impact = b.vy;
          b.y = sf.y - b.mat.h / 2;
          const toPlate = sf.sp === 'plate' || (typeof sf.sp === 'object' && sf.sp !== null && onPlate(sf.sp));
          if (toPlate) s.v += b.kg * impact * 0.0042;
          if (impact > 650 && !b.bounced) {
            b.vy = -impact * 0.16;
            b.bounced = true;
          } else {
            b.vy = 0;
            b.vx = 0;
            b.resting = true;
            b.support = sf.sp;
          }
        } else b.y += b.vy * dt;
      }
      // resting bodies ride on what holds them (the plate sinks under load)
      for (let pass = 0; pass < 4; pass++)
        for (const b of s.bodies) if (b.resting && b !== s.drag) b.y = supportTop(b) - b.mat.h / 2;

      // the load cell: a damped spring towards the load, the display's motion detection on top
      const L = load();
      const w0 = 2 * Math.PI * 2.1, zeta = 0.42;
      const sub = 4;
      for (let i = 0; i < sub; i++) {
        const a = w0 * w0 * (L - s.r) - 2 * zeta * w0 * s.v;
        s.v += a * dt / sub;
        s.r += s.v * dt / sub;
      }
      const moving = Math.abs(s.v) > 0.025 || Math.abs(L - s.r) > 0.6 * D;
      s.still = moving ? 0 : s.still + dt;
      s.stable = s.still > 0.35;
      s.noise = s.stable ? 0 : Math.max(-1.5 * D, Math.min(1.5 * D, s.noise * 0.7 + (Math.random() - 0.5) * D * 1.2));
      const gross = s.stable ? Math.round(L / D) * D : Math.round((s.r + s.noise) / D) * D;
      s.shown = Math.abs(gross) < D / 2 ? 0 : gross;
    }

    function readout(): Reading {
      const overload = s.shown > CAPACITY + 9 * D;
      const net = Math.round((s.shown - s.tare) / D) * D;
      return { kg: Math.abs(net) < D / 2 ? 0 : net, stable: s.stable, overload, gross: s.shown };
    }

    // ---- drawing
    function draw() {
      const want = Math.min(3, window.devicePixelRatio || 1);
      if (want !== dpr) {
        dpr = want;
        cv.width = W * dpr;
        cv.height = H * dpr;
      }
      c.setTransform(dpr, 0, 0, dpr, 0, 0);
      const dk = dark.matches;
      // wall, floor
      const bg = c.createLinearGradient(0, 0, 0, H);
      bg.addColorStop(0, dk ? '#162036' : '#f3f6fa');
      bg.addColorStop(1, dk ? '#111a2c' : '#e4eaf2');
      c.fillStyle = bg;
      c.fillRect(0, 0, W, H);
      c.fillStyle = dk ? '#0d1424' : '#cfd8e3';
      c.fillRect(0, FLOOR, W, H - FLOOR);
      c.fillStyle = dk ? '#26334a' : '#b8c4d2';
      c.fillRect(0, FLOOR, W, 2);

      // shelf
      c.fillStyle = dk ? '#3b2f25' : '#c9a37a';
      rr(c, 10, SHELF_Y, W - 20, 7, 2);
      c.fill();
      c.fillStyle = dk ? '#2a2119' : '#a8835b';
      c.fillRect(10, SHELF_Y + 7, W - 20, 2);
      for (const sl of SLOTS) {
        c.save();
        c.translate(sl.x, SHELF_Y - sl.h / 2);
        c.scale(SHELF_SCALE, SHELF_SCALE);
        sl.mat.draw(c, sl.mat.w, sl.mat.h);
        c.restore();
      }
      c.font = '600 9px Inter, sans-serif';
      c.textAlign = 'center';
      c.fillStyle = dk ? '#64748b' : '#94a3b8';
      for (const gname of ['Plastic', 'Glass', 'Paper', 'Metal'] as Group[]) {
        const gs = SLOTS.filter((x) => x.mat.group === gname);
        const cx = (gs[0].x - gs[0].w / 2 + gs[gs.length - 1].x + gs[gs.length - 1].w / 2) / 2;
        c.fillText(gname.toUpperCase(), cx, SHELF_Y + 22);
      }
      if (!s.bodies.length) {
        c.font = '500 12px Inter, sans-serif';
        c.fillStyle = dk ? '#64748b' : '#94a3b8';
        c.fillText('Drag materials onto the scale', W / 2, SHELF_Y + 56);
      }

      // the scale: body with the display, then the plate
      const pt = plateTop();
      c.fillStyle = dk ? '#0b0f17' : '#1f2937';
      rr(c, BASE_L - 6, FLOOR - 4, 14, 4, 1);
      c.fill();
      rr(c, BASE_R - 8, FLOOR - 4, 14, 4, 1);
      c.fill();
      const body = c.createLinearGradient(0, PLATE_Y0 + PLATE_T, 0, FLOOR);
      body.addColorStop(0, '#374151');
      body.addColorStop(1, '#1f2937');
      c.fillStyle = body;
      rr(c, BASE_L, PLATE_Y0 + PLATE_T - 1, BASE_R - BASE_L, FLOOR - 4 - (PLATE_Y0 + PLATE_T - 1), 6);
      c.fill();
      // column between body and plate
      c.fillStyle = '#4b5563';
      c.fillRect((PL + PR) / 2 - 18, pt + PLATE_T, 36, PLATE_Y0 + PLATE_T - (pt + PLATE_T));
      // display
      const lx = (BASE_L + BASE_R) / 2 - 78, ly = PLATE_Y0 + PLATE_T + 7, lw = 156, lh = 40;
      c.fillStyle = '#0a0f0c';
      rr(c, lx - 3, ly - 3, lw + 6, lh + 6, 6);
      c.fill();
      c.fillStyle = live.current.powered ? '#0c1a12' : '#0b0f0d';
      rr(c, lx, ly, lw, lh, 4);
      c.fill();
      if (live.current.powered) {
        const rd = readout();
        const on = '#4ade80', off = 'rgba(74,222,128,.07)';
        const txt = rd.overload ? '  OL' : rd.kg.toFixed(2);
        const cells = txt.replace('.', '').padStart(5, ' ').slice(-5);
        const dot = rd.overload ? -1 : cells.length - 2;
        c.shadowColor = 'rgba(74,222,128,.55)';
        for (let i = 0; i < 5; i++) {
          const dx = lx + 10 + i * 22;
          digit(c, cells[i], dx, ly + 7, 15, 26, on, off);
          if (i === dot - 1) {
            c.fillStyle = on;
            c.fillRect(dx + 17, ly + 30, 3.2, 3.2);
          }
        }
        c.shadowBlur = 0;
        c.font = '700 7px Inter, sans-serif';
        c.textAlign = 'left';
        const ind = (t: string, x: number, y: number, lit: boolean) => { c.fillStyle = lit ? on : off; c.fillText(t, x, y); };
        ind('kg', lx + 124, ly + 31, true);
        ind('STABLE', lx + 122, ly + 11, rd.stable && !rd.overload);
        ind('NET', lx + 122, ly + 20, s.tare !== 0);
      }
      // buttons
      const btn = (label: string, x: number, key: string) => {
        c.fillStyle = s.press === key ? '#6b7280' : '#4b5563';
        rr(c, x, FLOOR - 19, 34, 11, 5);
        c.fill();
        c.fillStyle = '#e5e7eb';
        c.font = '700 6.5px Inter, sans-serif';
        c.textAlign = 'center';
        c.fillText(label, x + 17, FLOOR - 11.3);
      };
      btn('ZERO', (BASE_L + BASE_R) / 2 - 38, 'zero');
      btn('TARE', (BASE_L + BASE_R) / 2 + 4, 'tare');
      c.fillStyle = live.current.powered ? '#22c55e' : '#7f1d1d';
      c.beginPath();
      c.arc(BASE_R - 16, FLOOR - 14, 2.6, 0, 7);
      c.fill();
      // plate
      const plate = c.createLinearGradient(0, pt, 0, pt + PLATE_T);
      plate.addColorStop(0, dk ? '#cbd5e1' : '#e5e7eb');
      plate.addColorStop(0.5, '#9ca3af');
      plate.addColorStop(1, '#6b7280');
      c.fillStyle = plate;
      rr(c, PL, pt, PR - PL, PLATE_T, 3);
      c.fill();
      c.strokeStyle = 'rgba(255,255,255,.55)';
      c.lineWidth = 1;
      c.beginPath();
      c.moveTo(PL + 4, pt + 1.5);
      c.lineTo(PR - 4, pt + 1.5);
      c.stroke();

      // bodies (shadows first)
      for (const b of s.bodies) {
        const ground = b.resting ? null : b.x >= PL && b.x <= PR ? pt : FLOOR;
        if (ground !== null && b.y + b.mat.h / 2 < ground - 2) {
          const d = Math.min(1, (ground - (b.y + b.mat.h / 2)) / 220);
          c.fillStyle = `rgba(15,23,42,${0.18 * (1 - d)})`;
          c.beginPath();
          c.ellipse(b.x, ground, b.mat.w * (0.5 - d * 0.25), 3, 0, 0, 7);
          c.fill();
        }
      }
      for (const b of s.bodies) {
        c.save();
        c.translate(b.x, b.y);
        if (b === s.drag) {
          c.shadowColor = 'rgba(15,23,42,.35)';
          c.shadowBlur = 14;
          c.shadowOffsetY = 6;
        }
        b.mat.draw(c, b.mat.w, b.mat.h);
        c.restore();
      }

      // tooltip
      const tip = s.drag ? null : s.hover;
      if (tip) {
        const label = `${tip.name} · ~${tip.kg < 1 ? Math.round(tip.kg * 1000) + ' g' : tip.kg + ' kg'}`;
        c.font = '600 11px Inter, sans-serif';
        const tw = c.measureText(label).width + 16;
        const tx = Math.max(4, Math.min(W - tw - 4, s.hx - tw / 2)), ty = Math.max(4, s.hy - 34);
        c.fillStyle = 'rgba(15,23,42,.92)';
        rr(c, tx, ty, tw, 22, 6);
        c.fill();
        c.fillStyle = '#fff';
        c.textAlign = 'left';
        c.fillText(label, tx + 8, ty + 15);
      }
    }

    function frame(now: number) {
      const dt = Math.min(0.033, (now - last) / 1000);
      last = now;
      step(dt);
      draw();
      if (now >= feedAt) {
        feedAt = now + 200;
        if (live.current.powered) live.current.onReading(readout());
      }
      raf = requestAnimationFrame(frame);
    }
    raf = requestAnimationFrame(frame);

    // ---- pointer
    const at = (e: PointerEvent) => {
      const r = cv.getBoundingClientRect();
      return [(e.clientX - r.left) * W / r.width, (e.clientY - r.top) * H / r.height];
    };
    const hitBody = (x: number, y: number) => {
      for (let i = s.bodies.length - 1; i >= 0; i--) {
        const b = s.bodies[i];
        if (Math.abs(x - b.x) <= b.mat.w / 2 + 3 && Math.abs(y - b.y) <= b.mat.h / 2 + 3) return b;
      }
      return null;
    };
    const hitSlot = (x: number, y: number) => SLOTS.find((sl) => Math.abs(x - sl.x) <= sl.w / 2 + 6 && y <= SHELF_Y + 4 && y >= SHELF_Y - sl.h - 8);
    const hitButton = (x: number, y: number) => {
      if (y < FLOOR - 22 || y > FLOOR - 5) return '';
      const z = (BASE_L + BASE_R) / 2 - 38, t = (BASE_L + BASE_R) / 2 + 4;
      return x >= z && x <= z + 34 ? 'zero' : x >= t && x <= t + 34 ? 'tare' : '';
    };
    const down = (e: PointerEvent) => {
      const [x, y] = at(e);
      const btn = hitButton(x, y);
      if (btn) {
        s.press = btn;
        if (btn === 'tare') s.tare = s.shown;
        else s.tare = 0;
        setTimeout(() => (s.press = ''), 140);
        return;
      }
      let b = hitBody(x, y);
      if (!b) {
        const sl = hitSlot(x, y);
        if (!sl) return;
        const m = sl.mat;
        b = { id: s.nextId++, mat: m, x, y: y - m.h / 4, vx: 0, vy: 0, kg: +(m.kg * (0.92 + Math.random() * 0.16)).toFixed(3), resting: false, support: null, bounced: false };
        s.bodies.push(b);
      } else {
        s.bodies.splice(s.bodies.indexOf(b), 1);
        s.bodies.push(b);
      }
      s.drag = b;
      b.resting = false;
      b.support = null;
      b.vx = b.vy = 0;
      s.grab = { dx: b.x - x, dy: b.y - y, lx: x, ly: y, lt: performance.now() };
      cv.setPointerCapture(e.pointerId);
      cv.classList.add('dragging');
      e.preventDefault();
    };
    const move = (e: PointerEvent) => {
      const [x, y] = at(e);
      s.hx = x;
      s.hy = y;
      const b = s.drag;
      if (!b) {
        s.hover = hitSlot(x, y)?.mat || null;
        cv.style.cursor = s.hover || hitBody(x, y) || hitButton(x, y) ? 'grab' : 'default';
        return;
      }
      const now = performance.now(), dt = Math.max(1, now - s.grab.lt) / 1000;
      b.vx = 0.6 * b.vx + 0.4 * ((x - s.grab.lx) / dt);
      b.vy = 0.6 * b.vy + 0.4 * ((y - s.grab.ly) / dt);
      s.grab.lx = x;
      s.grab.ly = y;
      s.grab.lt = now;
      b.x = Math.max(b.mat.w / 2, Math.min(W - b.mat.w / 2, x + s.grab.dx));
      b.y = Math.max(b.mat.h / 2, Math.min(FLOOR - b.mat.h / 2, y + s.grab.dy));
    };
    const up = () => {
      const b = s.drag;
      if (!b) return;
      s.drag = null;
      cv.classList.remove('dragging');
      if (b.y < SHELF_Y + 10) {  // back on the shelf: gone
        s.bodies = s.bodies.filter((x) => x !== b);
        return;
      }
      b.vx = Math.max(-900, Math.min(900, b.vx));
      b.vy = Math.max(-600, Math.min(1200, b.vy));
      b.bounced = false;
    };
    const leave = () => { s.hover = null; };
    cv.addEventListener('pointerdown', down);
    cv.addEventListener('pointermove', move);
    cv.addEventListener('pointerup', up);
    cv.addEventListener('pointercancel', up);
    cv.addEventListener('pointerleave', leave);
    return () => {
      cancelAnimationFrame(raf);
      cv.removeEventListener('pointerdown', down);
      cv.removeEventListener('pointermove', move);
      cv.removeEventListener('pointerup', up);
      cv.removeEventListener('pointercancel', up);
      cv.removeEventListener('pointerleave', leave);
    };
  }, []);

  return <div className="scale-wrap"><canvas ref={canvas} width={W} height={H} style={{ aspectRatio: `${W} / ${H}` }} /></div>;
});

export const MATERIALS = MATS.map(({ kind, name, group, kg }) => ({ kind, name, group, kg }));
