import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// VITE_API_URL: the store's API (default: the local runner, store/backend/run_local.sh; `iotdash store dev`)
export default defineConfig({
  plugins: [react()],
  server: { port: 5310, strictPort: true, host: true },
  preview: { port: 5311, host: true },
  build: { target: 'es2022' },
  // this build's id: the simulator's engine files are loaded with ?v=<id>, so a deploy is never hidden by a cache
  define: { __BUILD_ID__: JSON.stringify(Date.now().toString(36)) },
});
