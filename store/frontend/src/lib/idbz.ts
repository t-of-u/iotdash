// Reads an .idbz package's header and manifest in the browser (format: iotdash/tools/idbz.py), so an
// upload can be checked before it leaves the computer.

export interface Manifest {
  id: string; title?: string; version: string; description?: string;
  os?: { built?: string; min?: string }; built?: number;
}
export interface PackageInfo { manifest: Manifest; files: { name: string; size: number }[]; size: number }

export async function readPackage(file: Blob): Promise<PackageInfo> {
  const head = new Uint8Array(await file.slice(0, 64).arrayBuffer());
  if (head.length < 64 || String.fromCharCode(...head.subarray(0, 4)) !== 'IDBZ') throw new Error('Not an iotdash app package (.idbz)');
  const dv = new DataView(head.buffer);
  const format = dv.getUint16(4, true), count = dv.getUint16(6, true);
  const moff = dv.getUint32(8, true), mlen = dv.getUint32(12, true), total = dv.getUint32(16, true);
  if (format !== 1) throw new Error(`Package format ${format} is not supported`);
  if (total !== file.size) throw new Error(`The file is ${file.size} bytes, its header says ${total}: incomplete?`);
  const table = new Uint8Array(await file.slice(64, 64 + 64 * count).arrayBuffer());
  const tv = new DataView(table.buffer);
  const files = [];
  for (let i = 0; i < count; i++) {
    let end = i * 64;
    while (end < i * 64 + 24 && table[end]) end++;
    files.push({ name: new TextDecoder().decode(table.subarray(i * 64, end)), size: tv.getUint32(i * 64 + 28, true) });
  }
  const manifest = JSON.parse(await file.slice(moff, moff + mlen).text()) as Manifest;
  for (const n of ['app.json', 'icon.img', 'app.idb'])
    if (!files.some((f) => f.name === n)) throw new Error(`The package has no ${n}`);
  if (!manifest.id || !manifest.version) throw new Error('The package manifest has no id or version');
  return { manifest, files, size: file.size };
}
