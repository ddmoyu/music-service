import test from 'node:test';
import assert from 'node:assert/strict';
import { ApiError, MusicServiceClient, SseParser } from './client.mjs';

const json = (data, extra = {}) => new Response(JSON.stringify({
  data, meta: { request_id: 'test', partial: false, ...extra }, error: null,
}), { headers: { 'Content-Type': 'application/json' } });
const info = () => json({ features: { progress_sse: true }, events_base_url: 'http://127.0.0.1:17634' });
const error = (status, code, headers = {}) => new Response(JSON.stringify({
  data: null, meta: { request_id: 'req_test', partial: false },
  error: { code, message: code, retryable: false, details: [] },
}), { status, headers });

test('request retains partial failures and encodes Unicode search parameters', async () => {
  const client = new MusicServiceClient('http://127.0.0.1:17632', 'test-token', {
    fetchImpl: async (url, options) => {
      assert.equal(url.searchParams.get('q'), '歌曲 & 歌手');
      assert.equal(options.headers.Authorization, 'Bearer test-token');
      assert.equal(options.redirect, 'error');
      return json({ items: [] }, { partial: true, source_results: [{ status: 'failed' }] });
    },
  });
  const result = await client.search({ q: '歌曲 & 歌手' });
  assert.equal(result.meta.partial, true);
  assert.equal(result.meta.source_results[0].status, 'failed');
});

test('download retries keep the caller supplied idempotency key', async () => {
  const keys = [];
  const client = new MusicServiceClient('http://localhost', 'test', {
    fetchImpl: async (_url, options) => {
      keys.push(options.headers['Idempotency-Key']);
      return json({ task_id: 'tsk_test' });
    },
  });
  assert.throws(() => client.createDownload({}, ''), /Idempotency/);
  await client.createDownload({}, 'same-submission-1');
  await client.createDownload({}, 'same-submission-1');
  assert.deepEqual(keys, ['same-submission-1', 'same-submission-1']);
});

test('304 has no JSON body and preserves the ETag', async () => {
  const client = new MusicServiceClient('http://localhost', 'test', {
    fetchImpl: async () => new Response(null, { status: 304, headers: { ETag: 'W/"task-v2"' } }),
  });
  assert.equal((await client.request('/api/v1/downloads/tsk_test', { etag: 'W/"task-v2"' })).status, 304);
  await assert.rejects(client.info(), e => e.status === 304);
});

test('errors preserve status, request ID and Retry-After', async () => {
  const client = new MusicServiceClient('http://localhost', 'test', {
    fetchImpl: async () => error(429, 'CALLER_LIMIT_EXCEEDED', { 'Retry-After': '2' }),
  });
  await assert.rejects(client.info(), e =>
    e instanceof ApiError && e.status === 429 && e.retryAfterMs === 2000 && e.requestId === 'req_test');
});

test('JSON body is bounded and malformed envelopes fail', async () => {
  for (const body of ['x'.repeat(2 * 1024 * 1024 + 1), '{"data":{}}']) {
    const client = new MusicServiceClient('http://localhost', 'test', { fetchImpl: async () => new Response(body) });
    await assert.rejects(client.info(), ApiError);
  }
});

test('already cancelled requests never call fetch', async () => {
  let called = false;
  const client = new MusicServiceClient('http://localhost', 'test', {
    fetchImpl: async () => { called = true; return info(); },
  });
  await assert.rejects(client.info({ signal: AbortSignal.abort() }));
  assert.equal(called, false);
});

test('request timeout aborts the pending transport', async () => {
  const client = new MusicServiceClient('http://localhost', 'test', {
    timeoutMs: 15,
    fetchImpl: async (_url, { signal }) => new Promise((_resolve, reject) => {
      signal.addEventListener('abort', () => reject(signal.reason), { once: true });
    }),
  });
  await assert.rejects(client.info(), e => e.name === 'TimeoutError');
});

test('SSE handles CR, CRLF split across chunks, multiline data and comments', () => {
  const parser = new SseParser();
  const events = [];
  for (const chunk of [': heartbeat\r', '\nevent: task.updated\rdata: {"a":\r', '\ndata: 1}\r\n\r', '\n']) {
    events.push(...parser.push(chunk));
  }
  assert.deepEqual(events, [{ event: 'task.updated', data: '{"a":\n1}' }]);
  assert.deepEqual([...parser.push('data: incomplete')], []);
});

test('SSE prevents unbounded frames', () => {
  assert.throws(() => [...new SseParser(32).push('data:' + 'x'.repeat(40))], /size limit/);
});

test('UTF-8 SSE bytes can split anywhere; sync precedes updates and cancellation cleans up', async () => {
  const signal = new AbortController();
  const order = [];
  const bytes = new TextEncoder().encode(
    'event: stream.ready\ndata: {"instance_id":"实例","sync_required":true}\n\n' +
    'event: task.updated\ndata: {"task_id":"tsk_test","version":2}\n\n');
  let canceled = false;
  const client = new MusicServiceClient('http://localhost', 'test', {
    fetchImpl: async url => {
      if (String(url).endsWith('/api/v1/info')) return info();
      let index = 0;
      return new Response(new ReadableStream({
        pull(controller) { if (index < bytes.length) controller.enqueue(bytes.slice(index, ++index)); },
        cancel() { canceled = true; },
      }), { headers: { 'Content-Type': 'text/event-stream' } });
    },
  });
  await client.watchDownloads({
    signal: signal.signal,
    onSync: async event => { assert.equal(event.instance_id, '实例'); order.push('sync'); },
    onEvent: event => { assert.equal(event.version, 2); order.push('event'); signal.abort(); },
  });
  assert.deepEqual(order, ['sync', 'event']);
  assert.equal(canceled, true);
});

test('401 terminates subscription without retrying', async () => {
  let requests = 0;
  const client = new MusicServiceClient('http://localhost', 'test', {
    fetchImpl: async () => { requests++; return error(401, 'UNAUTHENTICATED'); },
  });
  await assert.rejects(client.watchDownloads({ onEvent() {}, onSync() {} }), e => e.status === 401);
  assert.equal(requests, 1);
});

test('callback failures terminate rather than creating duplicate subscriptions', async () => {
  let retries = 0;
  const client = new MusicServiceClient('http://localhost', 'test', {
    fetchImpl: async url => String(url).endsWith('/api/v1/info') ? info() : new Response(
      'event: stream.ready\ndata: {"instance_id":"x","sync_required":true}\n\n',
      { headers: { 'Content-Type': 'text/event-stream' } }),
  });
  await assert.rejects(client.watchDownloads({
    onEvent() {}, onSync() { throw new Error('view failed'); }, onError() { retries++; },
  }), /view failed/);
  assert.equal(retries, 0);
});
