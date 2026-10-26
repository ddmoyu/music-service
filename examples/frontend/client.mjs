// No runtime dependencies. Works in modern browsers and Node.js 22+.
export class ApiError extends Error {
  constructor(status, error, retryAfterMs = 0, requestId = null) {
    super(error?.message || 'Request failed');
    this.name = 'ApiError';
    this.status = status;
    this.code = error?.code || 'INVALID_RESPONSE';
    this.details = error?.details || [];
    this.retryable = error?.retryable ?? false;
    this.retryAfterMs = retryAfterMs;
    this.requestId = requestId;
  }
}

function baseUrl(value) {
  const url = new URL(value);
  if (!['http:', 'https:'].includes(url.protocol) || url.username || url.password || url.search || url.hash) {
    throw new TypeError('Expected an HTTP base URL without credentials, query or fragment');
  }
  return url.href.replace(/\/+$/, '');
}

function retryAfter(value) {
  if (!value) return 0;
  const ms = /^\d+$/.test(value) ? Number(value) * 1000 : Date.parse(value) - Date.now();
  return Number.isFinite(ms) ? Math.max(0, Math.min(ms, 3_600_000)) : 0;
}

function cancellation(signal) {
  signal?.throwIfAborted();
  const controller = new AbortController();
  const abort = () => controller.abort(signal.reason);
  signal?.addEventListener('abort', abort, { once: true });
  return { controller, dispose: () => signal?.removeEventListener('abort', abort) };
}

async function boundedJson(response, limit = 2 * 1024 * 1024) {
  if (!response.body) throw new ApiError(response.status, { message: 'Missing JSON response' });
  const reader = response.body.getReader();
  const decoder = new TextDecoder('utf-8', { fatal: true });
  let bytes = 0;
  let text = '';
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      bytes += value.byteLength;
      if (bytes > limit) throw new ApiError(response.status, { message: 'JSON response exceeds size limit' });
      text += decoder.decode(value, { stream: true });
    }
    return JSON.parse(text + decoder.decode());
  } finally {
    await reader.cancel().catch(() => {});
    reader.releaseLock();
  }
}

async function responseEnvelope(response) {
  let envelope;
  try {
    envelope = await boundedJson(response);
  } catch (error) {
    if (error?.name === 'AbortError' || error?.name === 'TimeoutError') throw error;
    throw new ApiError(response.status, { message: 'Invalid or oversized JSON response' },
      retryAfter(response.headers.get('Retry-After')), response.headers.get('X-Request-Id'));
  }
  if (!response.ok || envelope?.error) {
    throw new ApiError(response.status, envelope?.error,
      retryAfter(response.headers.get('Retry-After')), envelope?.meta?.request_id);
  }
  if (!envelope || !Object.hasOwn(envelope, 'data') || !envelope.meta || envelope.error !== null) {
    throw new ApiError(response.status, { message: 'Invalid response envelope' });
  }
  // Keep meta.partial/source_results: a 200 can contain failed sources.
  return { ...envelope, status: response.status, etag: response.headers.get('ETag') };
}

export class SseParser {
  constructor(maxFrameChars = 65536) {
    this.maxFrameChars = maxFrameChars;
    this.line = '';
    this.data = [];
    this.event = 'message';
    this.frameChars = 0;
    this.skipLF = false;
  }

  *push(text) {
    for (const char of text) {
      if (this.skipLF) {
        this.skipLF = false;
        if (char === '\n') continue;
      }
      if (char !== '\r' && char !== '\n') {
        this.line += char;
        if (++this.frameChars > this.maxFrameChars) throw new Error('SSE frame exceeds size limit');
        continue;
      }
      this.skipLF = char === '\r';
      const line = this.line;
      this.line = '';
      if (!line) {
        if (this.data.length) yield { event: this.event, data: this.data.join('\n') };
        this.data = [];
        this.event = 'message';
        this.frameChars = 0;
      } else if (!line.startsWith(':')) {
        const colon = line.indexOf(':');
        const field = colon < 0 ? line : line.slice(0, colon);
        let value = colon < 0 ? '' : line.slice(colon + 1);
        if (value.startsWith(' ')) value = value.slice(1);
        if (field === 'event') this.event = value;
        if (field === 'data') this.data.push(value);
      }
    }
  }
}

function sleep(ms, signal) {
  return new Promise((resolve, reject) => {
    signal?.throwIfAborted();
    const stop = () => { clearTimeout(timer); reject(signal.reason); };
    const timer = setTimeout(() => {
      signal?.removeEventListener('abort', stop);
      resolve();
    }, ms);
    signal?.addEventListener('abort', stop, { once: true });
  });
}

export class MusicServiceClient {
  constructor(url, token, { timeoutMs = 15000, fetchImpl = globalThis.fetch } = {}) {
    this.baseUrl = baseUrl(url);
    if (typeof token !== 'string' || !token.trim()) throw new TypeError('A token is required');
    if (!Number.isFinite(timeoutMs) || timeoutMs <= 0) throw new TypeError('timeoutMs must be positive');
    this.token = token;
    this.timeoutMs = timeoutMs;
    this.fetch = fetchImpl;
  }

  async request(path, { method = 'GET', query = {}, body, idempotencyKey, etag, signal } = {}) {
    if (!/^\/api\/v1\/[a-zA-Z0-9_/-]+$/.test(path)) throw new TypeError('Expected an API path');
    const url = new URL(this.baseUrl + path);
    for (const [key, value] of Object.entries(query)) {
      if (value !== undefined && value !== null) url.searchParams.set(key, String(value));
    }
    const { controller, dispose } = cancellation(signal);
    const timer = setTimeout(() => controller.abort(new DOMException('Request timed out', 'TimeoutError')),
      this.timeoutMs);
    try {
      const headers = { Authorization: 'Bearer ' + this.token, Accept: 'application/json' };
      if (body !== undefined) headers['Content-Type'] = 'application/json';
      if (idempotencyKey) headers['Idempotency-Key'] = idempotencyKey;
      if (etag) headers['If-None-Match'] = etag;
      const response = await this.fetch(url, {
        method, headers, body: body === undefined ? undefined : JSON.stringify(body),
        signal: controller.signal, redirect: 'error', credentials: 'omit', cache: 'no-store',
      });
      if (response.status === 304) {
        if (!etag) throw new ApiError(304, { message: 'Unexpected 304 without a conditional request' });
        return { status: 304, etag: response.headers.get('ETag'), data: null };
      }
      return await responseEnvelope(response);
    } finally {
      clearTimeout(timer);
      dispose();
    }
  }

  info(options) { return this.request('/api/v1/info', options); }
  search(query, options = {}) { return this.request('/api/v1/search/tracks', { ...options, query }); }
  resolve(body, options = {}) { return this.request('/api/v1/assets/resolve', { ...options, method: 'POST', body }); }
  mediaAccess(body, options = {}) { return this.request('/api/v1/media-access', { ...options, method: 'POST', body }); }
  createDownload(body, idempotencyKey, options = {}) {
    if (!idempotencyKey) throw new TypeError('Keep one Idempotency-Key for each submission');
    return this.request('/api/v1/downloads', { ...options, method: 'POST', body, idempotencyKey });
  }

  async watchDownloads({ signal, onEvent, onSync, onError = () => {}, maxRetries = 8, idleMs = 45000 }) {
    if (typeof onEvent !== 'function' || typeof onSync !== 'function') {
      throw new TypeError('onEvent and onSync callbacks are required');
    }
    if (!Number.isInteger(maxRetries) || maxRetries < 0 || !Number.isFinite(idleMs) || idleMs <= 0) {
      throw new TypeError('Invalid stream limits');
    }
    let failures = 0;
    while (!signal?.aborted) {
      const { controller, dispose } = cancellation(signal);
      const started = Date.now();
      let timer;
      let reader;
      let delay = 0;
      let callbackFailed = false;
      const arm = ms => {
        clearTimeout(timer);
        timer = setTimeout(() => controller.abort(new DOMException('SSE timed out', 'TimeoutError')), ms);
      };
      try {
        // Rediscover on reconnect: a restarted desktop service may advertise new addresses.
        const { data: info } = await this.info({ signal: controller.signal });
        if (!info.features.progress_sse || !info.events_base_url) {
          throw new ApiError(501, { code: 'FEATURE_UNAVAILABLE', message: 'Use conditional polling' });
        }
        const events = baseUrl(info.events_base_url);
        if (this.baseUrl.startsWith('https:') && !events.startsWith('https:')) {
          throw new ApiError(400, { message: 'Refusing to send the token to an insecure event URL' });
        }
        arm(this.timeoutMs);
        const response = await this.fetch(events + '/events/v1/downloads', {
          headers: { Authorization: 'Bearer ' + this.token, Accept: 'text/event-stream' },
          signal: controller.signal, redirect: 'error', credentials: 'omit', cache: 'no-store',
        });
        if (!response.ok) await responseEnvelope(response);
        if (!response.body || !response.headers.get('Content-Type')?.startsWith('text/event-stream')) {
          throw new ApiError(400, { message: 'Expected an SSE response' });
        }
        reader = response.body.getReader();
        const decoder = new TextDecoder('utf-8', { fatal: true });
        const parser = new SseParser();
        for (;;) {
          controller.signal.throwIfAborted();
          arm(idleMs);
          const { done, value } = await reader.read();
          if (done) break; // An incomplete final SSE event is not dispatched.
          clearTimeout(timer);
          for (const frame of parser.push(decoder.decode(value, { stream: true }))) {
            if (!['stream.ready', 'task.updated'].includes(frame.event)) continue;
            const data = JSON.parse(frame.data);
            try {
              if (frame.event === 'stream.ready') await onSync(data, controller.signal);
              else await onEvent(data);
            } catch (error) {
              callbackFailed = true;
              throw error;
            }
          }
        }
        throw new Error('Event stream disconnected');
      } catch (error) {
        if (signal?.aborted) return;
        if (callbackFailed || (error instanceof ApiError &&
            error.status >= 400 && error.status < 500 && error.status !== 429) ||
            (error instanceof ApiError && error.status === 501)) throw error;
        if (Date.now() - started >= 30000) failures = 0;
        if (++failures > maxRetries) throw error;
        delay = Math.max(error.retryAfterMs || 0,
          Math.min(30000, 1000 * 2 ** (failures - 1)) * (0.8 + Math.random() * 0.4));
        onError(error, delay);
      } finally {
        clearTimeout(timer);
        controller.abort();
        await reader?.cancel().catch(() => {});
        reader?.releaseLock();
        dispose();
      }
      try { await sleep(delay, signal); }
      catch (error) { if (signal?.aborted) return; throw error; }
    }
  }
}
