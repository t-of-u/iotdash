import { useCallback, useEffect, useRef, useState } from 'react';
import { Link, useNavigate, useParams } from 'react-router-dom';
import { ActionButtons, AppIcon, Confirm, Icon, Modal, StatusChip } from '../components/ui';
import { ApiError, postForm, projects, type Project, type Version } from '../lib/api';
import { ago, dateTime, size } from '../lib/format';
import { readPackage, type PackageInfo } from '../lib/idbz';
import { ProjectForm } from './Projects';

export default function ProjectPage() {
  const { pid = '' } = useParams();
  const [p, setP] = useState<Project | null>(null);
  const [error, setError] = useState('');
  const [editing, setEditing] = useState(false);
  const [deleting, setDeleting] = useState(false);
  const [uploading, setUploading] = useState(false);
  const [editVersion, setEditVersion] = useState<Version | null>(null);
  const [deleteVersion, setDeleteVersion] = useState<Version | null>(null);
  const navigate = useNavigate();

  const load = useCallback(() => projects.get(pid).then(setP).catch((e) => setError(e.message)), [pid]);
  useEffect(() => { load(); }, [load]);

  if (error) return <div className="error-box">{error}</div>;
  if (!p) return <div className="empty"><div className="spinner" style={{ margin: '0 auto' }} /></div>;
  const versions = p.versions || [];

  async function download(v: Version) {
    const d = await projects.version(p!.id, v.id);
    if (d.package_url) window.location.href = d.package_url;
  }

  return (
    <>
      <div className="crumbs"><Link to="/">Projects</Link><span>/</span><span>{p.name}</span></div>
      <div className="card phead">
        <AppIcon src={p.icon_png_url} name={p.name} large />
        <div className="phead-text">
          <h1>{p.name} {p.visibility === 'private' ? <span className="chip">Private</span> : <span className="chip blue">Public</span>}</h1>
          <div className="pcard-id">{p.app_id}{p.owner ? ` · by ${p.owner.name}` : ''}</div>
          <div className="phead-desc">{p.description || <span style={{ color: 'var(--faint)' }}>No description</span>}</div>
          <div className="stats">
            <div className="stat"><b>{p.latest_version?.version || '—'}</b><span>Latest</span></div>
            <div className="stat"><b>{p.versions_count}</b><span>Versions</span></div>
            <div className="stat"><b>{p.installs}</b><span>Installs</span></div>
            <div className="stat"><b>{ago(p.updated_date)}</b><span>Updated</span></div>
          </div>
        </div>
        <div className="phead-actions">
          <button className="btn ghost" onClick={() => setEditing(true)}>{Icon.edit}Edit</button>
          <button className="btn danger-ghost" onClick={() => setDeleting(true)}>{Icon.trash}Delete</button>
        </div>
      </div>

      <div className="section-head">
        <h2>Versions</h2>
        <button className="btn" onClick={() => setUploading(true)}>{Icon.upload}Upload version</button>
      </div>
      {!versions.length ? (
        <div className="card empty">
          <h3>No versions yet</h3>
          <div>Build a package with <code>iotdash pack</code> in the app's folder and upload the <code>.idbz</code> here,
            or run <code>iotdash publish</code> there.</div>
          <button className="btn" style={{ marginTop: 16 }} onClick={() => setUploading(true)}>{Icon.upload}Upload version</button>
        </div>
      ) : (
        <div className="card vlist">
          <div className="vrow head-row"><div>Version</div><div>Uploaded</div><div>iotdash OS</div><div>Store</div><div /></div>
          {versions.map((v) => (
            <div className="vrow" key={v.id}>
              <div>
                <div className="v-name">{v.version || '—'} <StatusChip v={v} />{p.latest_version?.id === v.id && <span className="chip blue">Latest</span>}</div>
                <div className="v-desc">{v.status === 'failed' ? <span style={{ color: 'var(--danger)' }}>{v.error}</span> : v.description || <span style={{ color: 'var(--faint)' }}>No notes</span>}</div>
              </div>
              <div className="v-small">
                <b>{dateTime(v.created_date)}</b><br />{v.uploaded_by?.name || ''}{v.size ? ` · ${size(v.size)}` : ''}
              </div>
              <div className="v-small">{v.os_min ? <>runs on <b>{v.os_min}</b>+<br />built with {v.os_built}</> : '—'}</div>
              <div className="v-pub">
                <button className={`switch${v.published ? ' on' : ''}`} disabled={v.status !== 'ready'} title={v.published ? 'In the store' : 'Hidden from boards'}
                  onClick={async () => { await projects.updateVersion(p.id, v.id, { published: !v.published }); load(); }} />
                <span>{v.installs} inst.</span>
              </div>
              <div className="v-actions">
                {v.status === 'ready' && <button className="btn soft sm" onClick={() => navigate(`/projects/${p.id}/versions/${v.id}/simulate`)}>{Icon.play}Simulate</button>}
                {v.status === 'ready' && <button className="btn ghost sm icon" title="Download .idbz" onClick={() => download(v)}>{Icon.download}</button>}
                <button className="btn ghost sm icon" title="Edit notes" onClick={() => setEditVersion(v)}>{Icon.edit}</button>
                <button className="btn ghost sm icon" title="Delete" onClick={() => setDeleteVersion(v)}>{Icon.trash}</button>
              </div>
            </div>
          ))}
        </div>
      )}

      {editing && <ProjectForm project={p} onClose={() => setEditing(false)} onSaved={() => load()} />}
      {deleting && <Confirm title={`Delete ${p.name}?`} message="The app leaves the store; boards that have it keep their copy." onClose={() => setDeleting(false)}
        onOk={async () => { await projects.remove(p.id); navigate('/'); }} />}
      {uploading && <Upload project={p} onClose={() => setUploading(false)} onDone={load} />}
      {editVersion && <VersionForm p={p} v={editVersion} onClose={() => setEditVersion(null)} onSaved={load} />}
      {deleteVersion && <Confirm title={`Delete version ${deleteVersion.version || ''}?`} message="Its package is removed from storage. Boards keep what they installed."
        onClose={() => setDeleteVersion(null)} onOk={async () => { await projects.removeVersion(p.id, deleteVersion.id); load(); }} />}
    </>
  );
}

function VersionForm({ p, v, onClose, onSaved }: { p: Project; v: Version; onClose: () => void; onSaved: () => void }) {
  const [description, setDescription] = useState(v.description || '');
  const [published, setPublished] = useState(v.published);
  return (
    <Modal title={`Version ${v.version}`} sub={`Uploaded ${dateTime(v.created_date)} · ${size(v.size)} · sha256 ${v.sha256?.slice(0, 12) || '—'}`} onClose={onClose}>
      <div className="field">
        <label htmlFor="vdesc">Release notes</label>
        <textarea id="vdesc" className="input" value={description} onChange={(e) => setDescription(e.target.value)} autoFocus />
      </div>
      <label className="check"><input type="checkbox" checked={published} disabled={v.status !== 'ready'} onChange={(e) => setPublished(e.target.checked)} />
        Published (boards on iotdash OS {v.os_min || '?'} or newer can install it)</label>
      <ActionButtons ok="Save" onClose={onClose} onOk={async () => { await projects.updateVersion(p.id, v.id, { description, published }); onSaved(); }} />
    </Modal>
  );
}

function Upload({ project, onClose, onDone }: { project: Project; onClose: () => void; onDone: () => void }) {
  const [file, setFile] = useState<File | null>(null);
  const [pkg, setPkg] = useState<PackageInfo | null>(null);
  const [problem, setProblem] = useState('');
  const [notes, setNotes] = useState('');
  const [publish, setPublish] = useState(true);
  const [stage, setStage] = useState<'pick' | 'upload' | 'check' | 'done'>('pick');
  const [progress, setProgress] = useState(0);
  const [error, setError] = useState('');
  const [over, setOver] = useState(false);
  const input = useRef<HTMLInputElement>(null);

  async function pick(f: File | undefined) {
    if (!f) return;
    setFile(f);
    setPkg(null);
    setError('');
    try {
      const info = await readPackage(f);
      setPkg(info);
      const taken = (project.versions || []).some((v) => v.status === 'ready' && v.version === info.manifest.version);
      setProblem(info.manifest.id !== project.app_id ? `This package is the app "${info.manifest.id}", not "${project.app_id}".`
        : taken ? `Version ${info.manifest.version} is already uploaded: raise "version" in app.json.` : '');
      if (!notes && info.manifest.description) setNotes(info.manifest.description);
    } catch (e) {
      setProblem((e as Error).message);
    }
  }

  async function upload() {
    if (!file) return;
    setError('');
    setStage('upload');
    try {
      const start = await projects.startUpload(project.id, file.name, file.size);
      await postForm(start, file, setProgress);
      setStage('check');
      await projects.complete(project.id, start.version_id, { description: notes.trim(), published: publish });
      setStage('done');
      onDone();
      onClose();
    } catch (e) {
      setError(e instanceof ApiError && e.code ? `${e.message} (${e.code})` : (e as Error).message);
      setStage('pick');
      onDone();
    }
  }

  const busy = stage === 'upload' || stage === 'check';
  return (
    <Modal title="Upload a version" sub={<>A package from <code>iotdash pack</code> in the app's folder (<code>build/{project.app_id}-&lt;version&gt;.idbz</code>)</>} onClose={() => !busy && onClose()} wide>
      <input ref={input} type="file" accept=".idbz" hidden onChange={(e) => pick(e.target.files?.[0])} />
      {!pkg && (
        <div className={`drop${over ? ' over' : ''}`} onClick={() => input.current?.click()}
          onDragOver={(e) => { e.preventDefault(); setOver(true); }} onDragLeave={() => setOver(false)}
          onDrop={(e) => { e.preventDefault(); setOver(false); pick(e.dataTransfer.files[0]); }}>
          {Icon.upload}
          <b>Drop the .idbz here</b>
          <span className="sub">or click to choose a file</span>
        </div>
      )}
      {pkg && (
        <dl className="pkg">
          <dt>File</dt><dd>{file?.name} · {size(pkg.size)}</dd>
          <dt>App</dt><dd>{pkg.manifest.title || pkg.manifest.id} <span className="mono">({pkg.manifest.id})</span></dd>
          <dt>Version</dt><dd>{pkg.manifest.version}</dd>
          <dt>iotdash OS</dt><dd>runs on {pkg.manifest.os?.min || '?'} or newer · built with {pkg.manifest.os?.built || '?'}</dd>
          <dt>Built</dt><dd>{pkg.manifest.built ? dateTime(new Date(pkg.manifest.built * 1000).toISOString()) : '—'}</dd>
          <dt>Files</dt><dd>{pkg.files.map((f) => `${f.name} ${size(f.size)}`).join(' · ')}</dd>
        </dl>
      )}
      {problem && <div className="error-box" style={{ marginTop: pkg ? 0 : 12, marginBottom: 12 }}>{problem}</div>}
      {pkg && !problem && (
        <>
          <div className="field">
            <label htmlFor="notes">Release notes</label>
            <textarea id="notes" className="input" value={notes} onChange={(e) => setNotes(e.target.value)} placeholder="What changed in this version" disabled={busy} />
          </div>
          <label className="check"><input type="checkbox" checked={publish} onChange={(e) => setPublish(e.target.checked)} disabled={busy} />
            Publish now (boards see it in the Apps store)</label>
        </>
      )}
      {busy && (
        <div style={{ marginTop: 16 }}>
          <div className="progress"><div style={{ width: `${stage === 'check' ? 100 : Math.round(progress * 100)}%` }} /></div>
          <div className="sub">{stage === 'upload' ? `Uploading… ${Math.round(progress * 100)}%` : 'Checking the package…'}</div>
        </div>
      )}
      {error && <div className="error-box" style={{ marginTop: 12 }}>{error}</div>}
      <div className="modal-actions">
        {pkg && !busy && <button className="btn ghost" onClick={() => { setPkg(null); setFile(null); setProblem(''); }}>Choose another</button>}
        <div className="grow" />
        <button className="btn ghost" onClick={onClose} disabled={busy}>Cancel</button>
        <button className="btn" disabled={!pkg || !!problem || busy} onClick={upload}>{busy ? <span className="spinner sm" /> : <>{Icon.upload}Upload</>}</button>
      </div>
    </Modal>
  );
}
