import { useEffect, useState } from 'react';
import { Link, useNavigate } from 'react-router-dom';
import { ActionButtons, AppIcon, Icon, Modal } from '../components/ui';
import { projects, type Project } from '../lib/api';
import { APP_ID, ago } from '../lib/format';

export default function Projects() {
  const [items, setItems] = useState<Project[] | null>(null);
  const [error, setError] = useState('');
  const [creating, setCreating] = useState(false);
  const [q, setQ] = useState('');

  const load = () => projects.list().then(setItems).catch((e) => setError(e.message));
  useEffect(() => { load(); }, []);

  const shown = (items || []).filter((p) => !q || `${p.name} ${p.app_id} ${p.description}`.toLowerCase().includes(q.toLowerCase()));
  return (
    <>
      <div className="head">
        <div style={{ flex: 1 }}>
          <h1>Projects</h1>
          <div className="sub">Every app you publish to iotdash boards, with all its versions.</div>
        </div>
        {!!items?.length && <input className="input" placeholder="Search" value={q} onChange={(e) => setQ(e.target.value)} style={{ width: 200 }} />}
        <button className="btn" onClick={() => setCreating(true)}>{Icon.plus}New project</button>
      </div>
      {error && <div className="error-box">{error}</div>}
      {!items && !error && <div className="empty"><div className="spinner" style={{ margin: '0 auto' }} /></div>}
      {items && !items.length && (
        <div className="card empty">
          <h3>No projects yet</h3>
          <div>Create a project for an app, then upload its package (<code>iotdash pack</code> in the app's folder).</div>
          <button className="btn" style={{ marginTop: 16 }} onClick={() => setCreating(true)}>{Icon.plus}New project</button>
        </div>
      )}
      <div className="grid">
        {shown.map((p) => (
          <Link key={p.id} to={`/projects/${p.id}`} className="card pcard">
            <div className="pcard-top">
              <AppIcon src={p.icon_png_url} name={p.name} />
              <div style={{ minWidth: 0 }}>
                <div className="pcard-name">{p.name}</div>
                <div className="pcard-id">{p.app_id}</div>
              </div>
            </div>
            <div className="pcard-desc">{p.description || 'No description'}</div>
            <div className="pcard-meta">
              {p.latest_version ? <span className="chip ok">{p.latest_version.version}</span> : <span className="chip">No release</span>}
              {p.visibility === 'private' && <span className="chip">Private</span>}
              <span>{p.versions_count} version{p.versions_count === 1 ? '' : 's'}</span>
              <span className="sep">·</span>
              <span>{p.installs} installs</span>
              <span className="sep">·</span>
              <span>{ago(p.updated_date)}</span>
            </div>
          </Link>
        ))}
      </div>
      {creating && <ProjectForm onClose={() => setCreating(false)} />}
    </>
  );
}

export function ProjectForm({ project, onClose, onSaved }: { project?: Project; onClose: () => void; onSaved?: (p: Project) => void }) {
  const [appId, setAppId] = useState(project?.app_id || '');
  const [name, setName] = useState(project?.name || '');
  const [description, setDescription] = useState(project?.description || '');
  const [visibility, setVisibility] = useState(project?.visibility || 'public');
  const navigate = useNavigate();
  const idOk = !!project || APP_ID.test(appId);
  return (
    <Modal title={project ? 'Edit project' : 'New project'} sub={project ? project.app_id : 'One project per app; versions are uploaded into it.'} onClose={onClose}>
      {!project && (
        <div className="field">
          <label htmlFor="appid">App id</label>
          <input id="appid" className={`input mono${appId && !idOk ? ' bad' : ''}`} value={appId} placeholder="my-app" autoFocus
            onChange={(e) => setAppId(e.target.value.toLowerCase())} />
          <span className={appId && !idOk ? 'err' : 'hint'}>The id in the package (folder name under os/apps): a–z, 0–9 and -, 2–40 characters. Fixed once created.</span>
        </div>
      )}
      <div className="field">
        <label htmlFor="pname">Name</label>
        <input id="pname" className="input" value={name} onChange={(e) => setName(e.target.value)} placeholder="My App" autoFocus={!!project} />
      </div>
      <div className="field">
        <label htmlFor="pdesc">Description</label>
        <textarea id="pdesc" className="input" value={description} onChange={(e) => setDescription(e.target.value)} placeholder="What the app does" />
      </div>
      <div className="field">
        <label>Visibility</label>
        <div className="seg">
          <button type="button" className={visibility === 'public' ? 'on' : ''} onClick={() => setVisibility('public')}>Public</button>
          <button type="button" className={visibility === 'private' ? 'on' : ''} onClick={() => setVisibility('private')}>Private</button>
        </div>
        <span className="hint">Private projects stay out of the boards' store.</span>
      </div>
      <ActionButtons ok={project ? 'Save' : 'Create project'} disabled={!idOk || !name.trim()} onClose={onClose} onOk={async () => {
        const body = { name: name.trim(), description: description.trim(), visibility };
        if (project) onSaved?.(await projects.update(project.id, body));
        else navigate(`/projects/${(await projects.create({ app_id: appId, ...body })).id}`);
      }} />
    </Modal>
  );
}
