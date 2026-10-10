// The iotstore API (docs/API.md). Answers are {success, data} or {success: false, message, error_code}.

export const API_URL = (import.meta.env.VITE_API_URL as string | undefined)?.replace(/\/+$/, '') || 'http://localhost:9310';

// the store this web app belongs to (/api/health): its name and whether anyone may sign up
export interface StoreInfo { name: string; signup: 'open' | 'closed'; version?: string }
const FALLBACK_NAME = (import.meta.env.VITE_STORE_NAME as string | undefined) || 'iotstore';
let info: Promise<StoreInfo> | null = null;
export function storeInfo(): Promise<StoreInfo> {
  info ??= fetch(API_URL + '/api/health')
    .then((r) => r.json())
    .then((j) => ({ name: j?.data?.name || FALLBACK_NAME, signup: j?.data?.signup === 'closed' ? 'closed' : 'open', version: j?.data?.version }) as StoreInfo)
    .catch(() => ({ name: FALLBACK_NAME, signup: 'open' }) as StoreInfo);
  return info;
}

export interface User { id: string; email: string; name: string; role: 'developer' | 'admin'; created_date: string }
export interface Version {
  id: string; project_id: string; version: string; title: string; description: string;
  status: 'uploading' | 'ready' | 'failed'; published: boolean; os_built: string | null; os_min: string | null;
  size: number | null; sha256: string | null; manifest: Record<string, unknown> | null; error: string | null;
  installs: number; uploaded_by: { id: string; name: string } | null; created_date: string; published_date: string | null;
  package_url?: string; icon_png_url?: string | null;
}
export interface Project {
  id: string; app_id: string; name: string; description: string; visibility: 'public' | 'private';
  owner: { id: string; name: string } | null; icon_png_url: string | null; latest_version: Version | null;
  versions_count: number; installs: number; created_date: string; updated_date: string;
  versions?: Version[];
}
export interface UploadStart { version_id: string; upload: { url: string; method: string; fields: Record<string, string> }; expires_in: number }

export class ApiError extends Error {
  constructor(message: string, public status: number, public code?: string, public errors?: Record<string, string>) {
    super(message);
  }
}

const TOKENS = 'iotstore.tokens';
interface Tokens { access_token: string; refresh_token: string }
function readTokens(): Tokens | null {
  try {
    return JSON.parse(localStorage.getItem(TOKENS) || 'null');
  } catch {
    return null;
  }
}
export function saveTokens(t: Tokens | null) {
  try {
    if (t) localStorage.setItem(TOKENS, JSON.stringify(t));
    else localStorage.removeItem(TOKENS);
  } catch { /* private mode: signed in for this tab only */ }
  memory = t;
}
let memory: Tokens | null = readTokens();
export const signedIn = () => !!memory;
let onSignedOut: () => void = () => {};
export const setSignedOutHandler = (fn: () => void) => { onSignedOut = fn; };

async function raw(method: string, path: string, body?: unknown, token?: string) {
  const headers: Record<string, string> = { Accept: 'application/json' };
  if (body !== undefined) headers['Content-Type'] = 'application/json';
  if (token) headers.Authorization = `Bearer ${token}`;
  let r: Response;
  try {
    r = await fetch(API_URL + path, { method, headers, body: body === undefined ? undefined : JSON.stringify(body) });
  } catch {
    throw new ApiError(`Cannot reach the store API at ${API_URL}`, 0, 'NETWORK');
  }
  let j: { success?: boolean; data?: unknown; message?: string; error_code?: string; errors?: Record<string, string> } = {};
  try {
    j = await r.json();
  } catch { /* not JSON */ }
  if (!r.ok || j.success === false) throw new ApiError(j.message || `HTTP ${r.status}`, r.status, j.error_code, j.errors);
  return j.data;
}

let refreshing: Promise<boolean> | null = null;
async function refresh(): Promise<boolean> {
  if (!memory?.refresh_token) return false;
  refreshing ??= raw('POST', '/api/auth/refresh', { refresh_token: memory.refresh_token })
    .then((d) => { saveTokens(d as Tokens); return true; })
    .catch(() => false)
    .finally(() => { refreshing = null; });
  return refreshing;
}

export async function api<T = unknown>(method: string, path: string, body?: unknown): Promise<T> {
  try {
    return (await raw(method, path, body, memory?.access_token)) as T;
  } catch (e) {
    if (!(e instanceof ApiError) || e.status !== 401 || !memory) throw e;
    if (await refresh()) return (await raw(method, path, body, memory?.access_token)) as T;
    saveTokens(null);
    onSignedOut();
    throw e;
  }
}

export const auth = {
  async login(email: string, password: string) {
    const d = await raw('POST', '/api/auth/login', { email, password }) as Tokens & { user: User };
    saveTokens(d);
    return d.user;
  },
  async register(email: string, password: string, name: string) {
    const d = await raw('POST', '/api/auth/register', { email, password, name }) as Tokens & { user: User };
    saveTokens(d);
    return d.user;
  },
  me: () => api<{ user: User }>('GET', '/api/auth/me').then((d) => d.user),
  logout: () => saveTokens(null),
};

export const projects = {
  list: () => api<{ items: Project[] }>('GET', '/api/projects').then((d) => d.items),
  get: (id: string) => api<Project>('GET', `/api/projects/${id}`),
  create: (p: { app_id: string; name: string; description?: string; visibility?: string }) => api<Project>('POST', '/api/projects', p),
  update: (id: string, p: Partial<Pick<Project, 'name' | 'description' | 'visibility'>>) => api<Project>('PATCH', `/api/projects/${id}`, p),
  remove: (id: string) => api('DELETE', `/api/projects/${id}`),
  version: (pid: string, vid: string) => api<Version>('GET', `/api/projects/${pid}/versions/${vid}`),
  updateVersion: (pid: string, vid: string, v: { description?: string; published?: boolean }) =>
    api<Version>('PATCH', `/api/projects/${pid}/versions/${vid}`, v),
  removeVersion: (pid: string, vid: string) => api('DELETE', `/api/projects/${pid}/versions/${vid}`),
  startUpload: (pid: string, filename: string, size: number) =>
    api<UploadStart>('POST', `/api/projects/${pid}/versions/upload`, { filename, size }),
  complete: (pid: string, vid: string, v: { description?: string; published?: boolean }) =>
    api<Version>('POST', `/api/projects/${pid}/versions/${vid}/complete`, v),
};

// the file goes straight to storage (S3 presigned POST, or the local runner's form): fields, then the file
export function postForm(start: UploadStart, file: Blob, onProgress: (f: number) => void): Promise<void> {
  return new Promise((resolve, reject) => {
    const form = new FormData();
    for (const [k, v] of Object.entries(start.upload.fields)) form.append(k, v);
    form.append('file', file);
    const x = new XMLHttpRequest();
    x.open('POST', start.upload.url);
    x.upload.onprogress = (e) => e.lengthComputable && onProgress(e.loaded / e.total);
    x.onload = () => (x.status >= 200 && x.status < 300 ? resolve() : reject(new ApiError(
      `Storage refused the file (${x.status})${x.responseText ? ': ' + x.responseText.slice(0, 200) : ''}`, x.status)));
    x.onerror = () => reject(new ApiError('Upload failed: storage cannot be reached (CORS?)', 0));
    x.send(form);
  });
}
