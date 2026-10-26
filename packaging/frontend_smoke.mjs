// Start only a fresh temporary service instance. No upstream/network music calls.
import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import { mkdtemp, mkdir, writeFile, readFile, rm } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { MusicServiceClient } from '../examples/frontend/client.mjs';

const binary = path.resolve(process.argv[2] || '');
if (!process.argv[2]) throw new Error('Usage: node packaging/frontend_smoke.mjs BINARY');
const temporary = await mkdtemp(path.join(os.tmpdir(), 'ms-frontend-'));
const environment = Object.fromEntries(Object.entries(process.env).filter(([key]) => !key.startsWith('MUSIC_SERVICE_')));
const origin = 'http://127.0.0.1:5173';
const config = path.join(temporary, 'config.json');
let child;
let exited;
try {
  await mkdir(path.join(temporary, 'downloads'));
  await writeFile(config, JSON.stringify({
    config_version: 1, data_dir: path.join(temporary, 'data'),
    control: { host: '127.0.0.1', port: 0 },
    media: { enabled: true, host: '127.0.0.1', port: 0 },
    events: { enabled: true, host: '127.0.0.1', port: 0 },
    cors: { allowed_origins: [origin] },
    features: { file_access: true, media_proxy: true },
    destinations: [{ id: 'dst_default', name: '临时下载', root: path.join(temporary, 'downloads'), principals: ['prn_local'] }],
  }));
  const initialized = spawnSync(binary, ['--config', config, 'init'], {
    env: environment, windowsHide: true, encoding: 'utf8', timeout: 20000, maxBuffer: 65536,
  });
  assert.equal(initialized.status, 0, initialized.stderr);
  const token = (await readFile(path.join(temporary, 'data/secrets/bootstrap-token.txt'), 'utf8')).trim();
  child = spawn(binary, ['--config', config, 'serve'], {
    env: environment, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'],
  });
  exited = new Promise(resolve => child.once('close', resolve));
  let diagnostics = '';
  child.stderr.on('data', chunk => { diagnostics = (diagnostics + chunk).slice(-8192); });
  const ready = await new Promise((resolve, reject) => {
    let pending = '';
    const timer = setTimeout(() => reject(new Error('Service startup timed out: ' + diagnostics)), 20000);
    child.once('error', error => { clearTimeout(timer); reject(error); });
    child.once('exit', code => { clearTimeout(timer); reject(new Error('Service exited ' + code + ': ' + diagnostics)); });
    child.stdout.on('data', chunk => {
      pending += chunk.toString('utf8');
      if (pending.length > 65536) { clearTimeout(timer); reject(new Error('Startup output exceeds limit')); return; }
      for (;;) {
        const end = pending.indexOf('\n');
        if (end < 0) break;
        const line = pending.slice(0, end);
        pending = pending.slice(end + 1);
        try {
          const record = JSON.parse(line);
          if (record.event === 'ready') { clearTimeout(timer); resolve(record); }
        } catch (error) { clearTimeout(timer); reject(error); }
      }
    });
  });
  const client = new MusicServiceClient(ready.api_base_url, token);
  const info = await client.info();
  assert.equal(info.data.api_version, 'v1');
  assert.equal(info.data.events_base_url, ready.events_base_url);
  assert.equal(info.data.media_base_url, ready.media_base_url);
  assert.equal(info.data.features.progress_sse, true);
  for (const route of ['/api/v1/providers', '/api/v1/destinations', '/api/v1/downloads']) {
    assert.ok(Array.isArray((await client.request(route)).data.items));
  }
  await assert.rejects(new MusicServiceClient(ready.api_base_url, 'invalid').info(), e => e.status === 401);
  await assert.rejects(client.search({ q: 'test', sources: 'not_an_installed_provider' }), e => e.status === 400);
  for (const [base, route] of [[ready.api_base_url, '/api/v1/info'],
                              [ready.events_base_url, '/events/v1/downloads'],
                              [ready.media_base_url, '/media/v1/files/fil_missing']]) {
    const allowed = await fetch(base + route, {
      method: 'OPTIONS', headers: { Origin: origin, 'Access-Control-Request-Method': 'GET',
        'Access-Control-Request-Headers': 'authorization' }, signal: AbortSignal.timeout(5000),
    });
    assert.equal(allowed.status, 204);
    assert.equal(allowed.headers.get('Access-Control-Allow-Origin'), origin);
    const denied = await fetch(base + route, {
      method: 'OPTIONS', headers: { Origin: 'https://unlisted.example' }, signal: AbortSignal.timeout(5000),
    });
    assert.equal(denied.status, 403);
  }
  const stop = new AbortController();
  const timeout = setTimeout(() => stop.abort(new Error('SSE test timed out')), 5000);
  let synced = false;
  try {
    await client.watchDownloads({
      signal: stop.signal,
      onEvent() {},
      onSync: async () => {
        assert.equal((await client.request('/api/v1/downloads')).data.items.length, 0);
        synced = true;
        stop.abort();
      },
    });
  } finally { clearTimeout(timeout); }
  assert.equal(synced, true);
  assert.equal((await client.info()).status, 200);
  console.log('Frontend integration passed: dynamic addresses, Bearer, API, three CORS listeners, SSE and cancellation');
} finally {
  if (child && child.exitCode === null) {
    child.kill();
    const force = setTimeout(() => child.kill('SIGKILL'), 5000);
    await exited;
    clearTimeout(force);
  }
  // Only remove the exact fresh directory created by mkdtemp above.
  assert.equal(path.dirname(temporary), path.resolve(os.tmpdir()));
  assert.ok(path.basename(temporary).startsWith('ms-frontend-'));
  await rm(temporary, { recursive: true, force: true });
}
