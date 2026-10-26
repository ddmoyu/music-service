export interface TrackRef { source: string; id: string }
export interface Track {
  ref: TrackRef; title: string; artists: { name: string; ref?: TrackRef | null }[];
  context_ref: string | null; duration_ms: number | null; [key: string]: unknown;
}
export interface Asset {
  asset_id: string; original_track: TrackRef; actual_track: TrackRef;
  actual_quality: string | null; expires_at: string; [key: string]: unknown;
}
export interface MediaAccess {
  url: string; auth: 'url' | 'bearer'; delivery: 'proxy' | 'direct';
  expires_at: string | null; [key: string]: unknown;
}
export interface RequestOptions {
  signal?: AbortSignal;
  method?: string;
  query?: Record<string, string | number | boolean | null | undefined>;
  body?: unknown;
  idempotencyKey?: string;
  etag?: string;
}
export interface Envelope<T = unknown> {
  status: number;
  data: T;
  meta: { request_id: string; partial: boolean; source_results?: unknown[]; [key: string]: unknown };
  error: null;
  etag: string | null;
}
export interface NotModified { status: 304; data: null; etag: string | null }
export interface Info {
  version: string;
  api_version: string;
  instance_id: string;
  media_base_url: string | null;
  events_base_url: string | null;
  features: Record<string, boolean>;
}
export interface TaskUpdate {
  task_id: string; version: number; status: string;
  progress: Record<string, unknown>; updated_at: string;
}
export class ApiError extends Error {
  status: number; code: string; details: unknown[]; retryable: boolean;
  retryAfterMs: number; requestId: string | null;
}
export class SseParser {
  constructor(maxFrameChars?: number);
  push(text: string): Generator<{ event: string; data: string }>;
}
export class MusicServiceClient {
  constructor(baseUrl: string, token: string, options?: { timeoutMs?: number; fetchImpl?: typeof fetch });
  request<T = unknown>(path: string, options?: RequestOptions): Promise<Envelope<T> | NotModified>;
  info(options?: Pick<RequestOptions, 'signal'>): Promise<Envelope<Info>>;
  search(query: RequestOptions['query'], options?: Pick<RequestOptions, 'signal'>): Promise<Envelope<{ items: Track[] }>>;
  resolve(body: { track: TrackRef; context_ref?: string; quality?: string; quality_policy?: string;
    allow_preview?: boolean }, options?: Pick<RequestOptions, 'signal'>): Promise<Envelope<Asset>>;
  mediaAccess(body: { target: { kind: 'asset' | 'file'; id: string };
    mode?: 'auto' | 'proxy' | 'direct'; auth?: 'ticket' | 'bearer'; ttl_seconds?: number },
    options?: Pick<RequestOptions, 'signal'>): Promise<Envelope<MediaAccess>>;
  createDownload(body: Record<string, unknown>, idempotencyKey: string,
    options?: Pick<RequestOptions, 'signal'>): Promise<Envelope<{ task_id: string; status: string; reused: boolean }>>;
  watchDownloads(options: {
    signal?: AbortSignal;
    onEvent: (event: TaskUpdate) => void | Promise<void>;
    onSync: (event: { instance_id: string; sync_required: boolean }, signal: AbortSignal) => void | Promise<void>;
    onError?: (error: Error, retryInMs: number) => void;
    maxRetries?: number; idleMs?: number;
  }): Promise<void>;
}
