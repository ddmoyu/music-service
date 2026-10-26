import { MusicServiceClient } from './client.mjs';

// Call from your UI with a user-supplied token kept in memory.
// This module never embeds tokens or starts downloads automatically.
export function connect(baseUrl, token) {
  return new MusicServiceClient(baseUrl, token);
}

export async function search(client, keywords, sources, signal) {
  const result = await client.search({ q: keywords, sources, limit: 20 }, { signal });
  return { tracks: result.data.items, partial: result.meta.partial, sources: result.meta.source_results };
}

export async function play(client, track, audioElement, signal) {
  audioElement.pause();
  audioElement.removeAttribute('src');
  audioElement.load();
  const { data: asset } = await client.resolve({
    track: track.ref, context_ref: track.context_ref,
    quality: 'standard', quality_policy: 'strict', allow_preview: false,
  }, { signal });
  const { data: access } = await client.mediaAccess({
    target: { kind: 'asset', id: asset.asset_id }, mode: 'proxy', auth: 'ticket', ttl_seconds: 60,
  }, { signal });
  audioElement.src = access.url; // Resource-scoped URL; do not append the service token.
  await audioElement.play(); // Usually called from a user gesture.
  return { asset, access }; // Refresh before expiry; show actual_track/actual_quality.
}

export function download(client, track, destinationId, submissionKey, signal) {
  // Generate crypto.randomUUID() once per user action and keep it when retrying
  // an uncertain HTTP submission. A successful 202 means accepted, not completed.
  return client.createDownload({
    track: track.ref, context_ref: track.context_ref, destination_id: destinationId,
    quality: 'standard', quality_policy: 'strict', allow_preview: false,
    metadata_policy: 'best_effort', duplicate_policy: 'reuse',
  }, submissionKey, { signal });
}

export function watch(client, onTasks, signal, onError = () => {}) {
  const tasks = new Map();
  return client.watchDownloads({
    signal, onError,
    onSync: async (_ready, connectionSignal) => {
      // Reload all pages after subscription registration and every reconnect.
      // The map belongs to this view; each sync replaces the prior instance.
      tasks.clear();
      let cursor;
      let pages = 0;
      do {
        if (++pages > 100) throw new Error('Limit reached: use a filtered task view');
        const { data } = await client.request('/api/v1/downloads', {
          query: { limit: 100, cursor }, signal: connectionSignal,
        });
        for (const task of data.items) tasks.set(task.task_id, task);
        cursor = data.page.next_cursor;
      } while (cursor);
      onTasks([...tasks.values()]);
    },
    onEvent: update => {
      const previous = tasks.get(update.task_id);
      if (!previous || update.version > previous.version) {
        if (!previous && tasks.size >= 10000) throw new Error('Task view capacity exceeded');
        tasks.set(update.task_id, { ...previous, ...update });
        // SSE is a compact projection. Fetch a terminal task to obtain result.file_id/error.
        onTasks([...tasks.values()]);
      }
    },
  });
}
