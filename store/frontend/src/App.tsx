import { createContext, useContext, useEffect, useState } from 'react';
import { Link, Navigate, Route, Routes, useLocation, useNavigate } from 'react-router-dom';
import { auth, setSignedOutHandler, signedIn, storeInfo, type StoreInfo, type User } from './lib/api';
import Login from './pages/Login';
import Projects from './pages/Projects';
import ProjectPage from './pages/Project';
import Simulate from './pages/Simulate';

interface Session { user: User | null; setUser: (u: User | null) => void; store: StoreInfo }
const DEFAULT_STORE: StoreInfo = { name: (import.meta.env.VITE_STORE_NAME as string | undefined) || 'iotstore', signup: 'open' };
const SessionContext = createContext<Session>({ user: null, setUser: () => {}, store: DEFAULT_STORE });
export const useSession = () => useContext(SessionContext);

export default function App() {
  const [user, setUser] = useState<User | null>(null);
  const [ready, setReady] = useState(!signedIn());
  const [store, setStore] = useState<StoreInfo>(DEFAULT_STORE);
  const navigate = useNavigate();

  useEffect(() => {
    storeInfo().then((s) => {
      setStore(s);
      document.title = s.name;
    });
  }, []);

  useEffect(() => {
    setSignedOutHandler(() => {
      setUser(null);
      navigate('/login');
    });
    if (signedIn()) auth.me().then(setUser).catch(() => auth.logout()).finally(() => setReady(true));
  }, [navigate]);

  if (!ready) return <div className="boot"><div className="spinner" /></div>;
  return (
    <SessionContext.Provider value={{ user, setUser, store }}>
      <Routes>
        <Route path="/login" element={user ? <Navigate to="/" replace /> : <Login />} />
        <Route path="/" element={<Signed><Shell><Projects /></Shell></Signed>} />
        <Route path="/projects/:pid" element={<Signed><Shell><ProjectPage /></Shell></Signed>} />
        <Route path="/projects/:pid/versions/:vid/simulate" element={<Signed><Shell wide><Simulate /></Shell></Signed>} />
        <Route path="*" element={<Navigate to="/" replace />} />
      </Routes>
    </SessionContext.Provider>
  );
}

function Signed({ children }: { children: React.ReactNode }) {
  const { user } = useSession();
  const loc = useLocation();
  if (!user) return <Navigate to="/login" replace state={{ from: loc.pathname }} />;
  return <>{children}</>;
}

function Shell({ children, wide }: { children: React.ReactNode; wide?: boolean }) {
  const { user, setUser, store } = useSession();
  const navigate = useNavigate();
  return (
    <div className="shell">
      <header className="topbar">
        <div className={`topbar-in${wide ? ' wide' : ''}`}>
          <Link to="/" className="brand">
            <img src="/favicon.svg" alt="" width={28} height={28} />
            <span>{store.name}</span>
          </Link>
          <nav className="topnav">
            <Link to="/">Projects</Link>
          </nav>
          <div className="grow" />
          {user && (
            <div className="me">
              <div className="avatar">{user.name.slice(0, 1).toUpperCase()}</div>
              <div className="me-text">
                <div className="me-name">{user.name}</div>
                <div className="me-mail">{user.email}{user.role === 'admin' ? ' · admin' : ''}</div>
              </div>
              <button className="btn ghost sm" onClick={() => { auth.logout(); setUser(null); navigate('/login'); }}>Sign out</button>
            </div>
          )}
        </div>
      </header>
      <main className={`page${wide ? ' wide' : ''}`}>{children}</main>
    </div>
  );
}
