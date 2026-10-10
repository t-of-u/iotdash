import { useState } from 'react';
import { useLocation, useNavigate } from 'react-router-dom';
import { useSession } from '../App';
import { API_URL, ApiError, auth } from '../lib/api';

export default function Login() {
  const [mode, setMode] = useState<'login' | 'register'>('login');
  const [email, setEmail] = useState('');
  const [password, setPassword] = useState('');
  const [name, setName] = useState('');
  const [error, setError] = useState('');
  const [busy, setBusy] = useState(false);
  const { setUser, store } = useSession();
  const navigate = useNavigate();
  const from = (useLocation().state as { from?: string } | null)?.from || '/';

  async function submit(e: React.FormEvent) {
    e.preventDefault();
    setBusy(true);
    setError('');
    try {
      const u = mode === 'login' ? await auth.login(email.trim(), password) : await auth.register(email.trim(), password, name.trim());
      setUser(u);
      navigate(from, { replace: true });
    } catch (err) {
      setError(err instanceof ApiError ? err.message : String(err));
    } finally {
      setBusy(false);
    }
  }

  return (
    <div className="login">
      <div className="login-art">
        <div className="brand"><img src="/favicon.svg" alt="" width={30} height={30} /><span>{store.name}</span></div>
        <div className="login-board">{Array.from({ length: 8 }, (_, i) => <i key={i} />)}</div>
        <div>
          <h1>Apps for iotdash boards</h1>
          <p>Upload app packages, keep every version, try them in the browser with a simulated board, and let boards install them from the Apps app.</p>
        </div>
      </div>
      <div className="login-form">
        <form onSubmit={submit}>
          <h2>{mode === 'login' ? 'Sign in' : 'Create an account'}</h2>
          <div className="sub">{mode === 'login' ? 'Developers sign in to manage their apps.'
            : store.signup === 'closed' ? 'Sign-up is closed on this store: only its first account (the admin) can be made here.'
            : 'The first account becomes the store admin.'}</div>
          {mode === 'register' && (
            <div className="field">
              <label htmlFor="name">Name</label>
              <input id="name" className="input" value={name} onChange={(e) => setName(e.target.value)} required autoComplete="name" />
            </div>
          )}
          <div className="field">
            <label htmlFor="email">E-mail</label>
            <input id="email" className="input" type="email" value={email} onChange={(e) => setEmail(e.target.value)} required autoComplete="email" />
          </div>
          <div className="field">
            <label htmlFor="pw">Password</label>
            <input id="pw" className="input" type="password" value={password} onChange={(e) => setPassword(e.target.value)} required minLength={mode === 'register' ? 8 : 1}
              autoComplete={mode === 'login' ? 'current-password' : 'new-password'} />
            {mode === 'register' && <span className="hint">8 characters or more</span>}
          </div>
          {error && <div className="error-box" style={{ marginBottom: 14 }}>{error}</div>}
          <button className="btn" style={{ width: '100%', height: 42 }} disabled={busy}>
            {busy ? <span className="spinner sm" /> : mode === 'login' ? 'Sign in' : 'Create account'}
          </button>
          <div className="login-switch">
            {mode === 'login' ? (store.signup === 'closed' ? 'New store? ' : 'No account yet? ') : 'Have an account? '}
            <button type="button" onClick={() => { setMode(mode === 'login' ? 'register' : 'login'); setError(''); }}>
              {mode === 'login' ? (store.signup === 'closed' ? 'Create the admin account' : 'Create one') : 'Sign in'}
            </button>
          </div>
          <div className="api-at">API: {API_URL}</div>
        </form>
      </div>
    </div>
  );
}
