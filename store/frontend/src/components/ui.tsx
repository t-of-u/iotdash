import { useEffect, useState, type ReactNode } from 'react';
import type { Version } from '../lib/api';

export function Modal({ title, sub, onClose, children, wide }: { title: string; sub?: ReactNode; onClose: () => void; children: ReactNode; wide?: boolean }) {
  useEffect(() => {
    const k = (e: KeyboardEvent) => e.key === 'Escape' && onClose();
    window.addEventListener('keydown', k);
    return () => window.removeEventListener('keydown', k);
  }, [onClose]);
  return (
    <div className="modal-bg" onMouseDown={(e) => e.target === e.currentTarget && onClose()}>
      <div className={`modal${wide ? ' lg' : ''}`} role="dialog" aria-modal="true">
        <h2>{title}</h2>
        {sub ? <div className="sub">{sub}</div> : <div style={{ height: 14 }} />}
        {children}
      </div>
    </div>
  );
}

export function Confirm({ title, message, ok = 'Delete', onOk, onClose }: { title: string; message: ReactNode; ok?: string; onOk: () => Promise<unknown>; onClose: () => void }) {
  return (
    <Modal title={title} onClose={onClose}>
      <div className="sub" style={{ marginTop: -6 }}>{message}</div>
      <ActionButtons ok={ok} danger onOk={onOk} onClose={onClose} />
    </Modal>
  );
}

export function ActionButtons({ ok, onOk, onClose, danger, disabled }: { ok: string; onOk: () => Promise<unknown>; onClose: () => void; danger?: boolean; disabled?: boolean }) {
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  return (
    <>
      {error && <div className="error-box" style={{ marginTop: 12 }}>{error}</div>}
      <div className="modal-actions">
        <button className="btn ghost" onClick={onClose} disabled={busy}>Cancel</button>
        <button className={`btn${danger ? ' danger' : ''}`} disabled={busy || disabled} onClick={async () => {
          setBusy(true);
          setError('');
          try {
            await onOk();
            onClose();
          } catch (e) {
            setError((e as Error).message);
            setBusy(false);
          }
        }}>{busy ? <span className="spinner sm" /> : ok}</button>
      </div>
    </>
  );
}

export function AppIcon({ src, name, large }: { src?: string | null; name: string; large?: boolean }) {
  const [bad, setBad] = useState(false);
  if (src && !bad) return <img className={`app-icon${large ? ' lg' : ''}`} src={src} alt="" onError={() => setBad(true)} />;
  return <div className={`app-icon${large ? ' lg' : ''}`}>{(name || '?').slice(0, 1).toUpperCase()}</div>;
}

export function StatusChip({ v }: { v: Version }) {
  if (v.status === 'ready') return v.published ? <span className="chip ok"><span className="dot" />Published</span> : <span className="chip">Draft</span>;
  if (v.status === 'failed') return <span className="chip bad">Failed</span>;
  return <span className="chip warn">Uploading</span>;
}

export const Icon = {
  plus: <svg viewBox="0 0 24 24"><path d="M12 5v14M5 12h14" /></svg>,
  upload: <svg viewBox="0 0 24 24"><path d="M12 16V4M7 9l5-5 5 5M4 16v3a1 1 0 0 0 1 1h14a1 1 0 0 0 1-1v-3" /></svg>,
  download: <svg viewBox="0 0 24 24"><path d="M12 4v12M7 11l5 5 5-5M4 20h16" /></svg>,
  play: <svg viewBox="0 0 24 24"><path d="M7 4.5v15l12-7.5z" /></svg>,
  edit: <svg viewBox="0 0 24 24"><path d="M4 20h4L19 9l-4-4L4 16z" /><path d="M13 7l4 4" /></svg>,
  trash: <svg viewBox="0 0 24 24"><path d="M4 7h16M10 11v6M14 11v6M6 7l1 12a2 2 0 0 0 2 2h6a2 2 0 0 0 2-2l1-12M9 7V4h6v3" /></svg>,
  back: <svg viewBox="0 0 24 24"><path d="M15 5l-7 7 7 7" /></svg>,
  home: <svg viewBox="0 0 24 24"><path d="M4 11l8-7 8 7v8a1 1 0 0 1-1 1h-4v-6H9v6H5a1 1 0 0 1-1-1z" /></svg>,
  refresh: <svg viewBox="0 0 24 24"><path d="M20 11a8 8 0 1 0-2.3 5.7" /><path d="M20 4v7h-7" /></svg>,
  wifi: <svg className="ic" viewBox="0 0 24 24"><path d="M2.5 9a14 14 0 0 1 19 0" /><path d="M5.5 12.5a9.5 9.5 0 0 1 13 0" /><path d="M8.6 16a5 5 0 0 1 6.8 0" /><path d="M12 19.5h.01" /></svg>,
  globe: <svg className="ic" viewBox="0 0 24 24"><circle cx="12" cy="12" r="9" /><path d="M3 12h18M12 3a14 14 0 0 1 0 18M12 3a14 14 0 0 0 0 18" /></svg>,
  bt: <svg className="ic" viewBox="0 0 24 24"><path d="M7 7l10 10-5 4.5V2.5L17 7 7 17" /></svg>,
  scale: <svg className="ic" viewBox="0 0 24 24"><rect x="3" y="13" width="18" height="7" rx="2" /><path d="M6 13l1.5-4h9L18 13M10 16.5h4" /></svg>,
  dot: <svg className="ic" viewBox="0 0 24 24"><circle cx="12" cy="12" r="3" /></svg>,
};
