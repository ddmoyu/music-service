#!/usr/bin/env python3
"""酷我黑胶在线闭环；只调用原生服务，清空外部音频工具 PATH 后验证产物、复用与重启。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import time
import urllib.parse
import uuid

from kuwo_e2e import OwnService, Http, check, wait_task, ticket_for, get_url
from kuwo_artists_e2e import embedded_cover


def flac_facts(data):
    check(data.startswith(b'fLaC') and data[4] & 127 == 0, 'published bytes are native FLAC')
    bits = int.from_bytes(data[18:26], 'big')
    rate = bits >> 44
    at = 4
    while True:
        kind, size = data[at], int.from_bytes(data[at + 1:at + 4], 'big')
        at += 4 + size
        check(at <= len(data), 'metadata stays within the published file')
        if kind & 128:
            break
    return {'sample_rate_hz': rate, 'bits_per_sample': ((bits >> 36) & 31) + 1,
            'channels': ((bits >> 41) & 7) + 1, 'samples': bits & ((1 << 36) - 1),
            'pcm_md5': data[26:42].hex(), 'audio_sha256': hashlib.sha256(data[at:]).hexdigest(),
            'audio_bytes': len(data) - at, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}


def run(service, destination, report):
    config_path = Path(service.config)
    config = json.loads(config_path.read_text(encoding='utf-8'))
    config['downloads'] = {'output': {'embed_cover': True, 'embed_lyrics': False,
        'export_cover': True, 'export_lyrics': False, 'sidecars_required': True}}
    config_path.write_text(json.dumps(config, ensure_ascii=False), encoding='utf-8')
    service.restart()
    api = Http(service.api, service.token)
    providers = api.json('GET', '/api/v1/providers')[2]['items']
    kuwo = next(x for x in providers if x['source'] == 'kuwo')
    check('hires' in kuwo['quality_hints'], 'Kuwo advertises the implemented hires request')
    report['implementation'] = kuwo['implementation']
    query = urllib.parse.urlencode({'sources': 'kuwo', 'q': '亲密爱人 黑胶版', 'limit': 10})
    items = api.json('GET', '/api/v1/search/tracks?' + query)[2]['items']
    check(any(x['ref']['id'] == '492445332' for x in items), 'native search finds the exact vinyl edition')
    detail = api.json('GET', '/api/v1/tracks/detail?source=kuwo&id=492445332')[2]
    check(detail['album']['ref']['id'] == '46894897', 'native detail retains the expected album')
    request = {'track': {'source': 'kuwo', 'id': '492445332'}, 'quality': 'hires',
               'quality_policy': 'strict', 'metadata_policy': 'required'}
    before = time.monotonic()
    created = api.json('POST', '/api/v1/downloads', request,
                      {'Idempotency-Key': 'kuwo-vinyl-' + uuid.uuid4().hex}, expect=(200, 202))[2]
    task = wait_task(api, created['task_id'], {'completed', 'failed', 'canceled'}, timeout=480)
    # 错误只输出业务码，不输出可能含上游地址的完整任务。
    check(task['status'] == 'completed', 'native vinyl download completes: ' + str((task.get('error') or {}).get('code')))
    check(task['actual_quality'] == 'hires', 'decoded quality passes strict hires policy')
    check(task['result']['processing']['processor'] == 'audio.kuwo-flac', 'native full FLAC processor was used')
    check('KUWO_TRAILER_REMOVED' in task['result']['warnings'], 'verified trailer removal is reported')
    for field in ('title', 'artist', 'album', 'track_number', 'cover'):
        check(task['result']['metadata_fields'][field] == 'written', field + ' metadata is written')
    file_id = task['result']['file_id']
    status, _, data = get_url(ticket_for(api, file_id))
    check(status == 200, 'published file reads completely')
    facts = flac_facts(data)
    check(facts['bits_per_sample'] == 24 and facts['sample_rate_hz'] == 96000 and facts['channels'] == 2,
          'published audio has the expected 24 bit 96 kHz stereo format')
    check(facts['pcm_md5'] == '874082e3f1a569fe2f568d5a659c1423', 'PCM identity matches the previously verified sample')
    check(not data.endswith(bytes.fromhex('0e55fff0')), 'the proprietary tail is absent')
    path = destination / '梅艳芳 - 亲密爱人（黑胶版）-原生黑胶.flac'
    path.write_bytes(data)
    url = ticket_for(api, file_id)
    status, _, cover = get_url(url + ('&' if '?' in url else '?') + 'part=cover')
    check(status == 200 and cover == embedded_cover(data, 'lossless'), 'exported cover equals embedded cover')
    (destination / '梅艳芳 - 亲密爱人（黑胶版）-封面.jpg').write_bytes(cover)
    status, headers, last = get_url(ticket_for(api, file_id), {'Range': 'bytes=' + str(len(data) - 64) + '-'})
    check(status == 206 and last == data[-64:], 'range reads end at the cleaned file size')
    reused = api.json('POST', '/api/v1/downloads', request,
                     {'Idempotency-Key': 'kuwo-vinyl-reuse-' + uuid.uuid4().hex}, expect=(200, 202))[2]
    check(reused['task_id'] == task['task_id'], 'completed native result is reused')
    service.restart()
    api = Http(service.api, service.token)
    status, _, again = get_url(ticket_for(api, file_id))
    check(status == 200 and hashlib.sha256(again).hexdigest() == facts['sha256'], 'published file survives service restart')
    report.update(status='passed', track='492445332', title=detail['title'], album=detail['album']['title'],
        facts=facts, processing=task['result']['processing'], warnings=task['result']['warnings'],
        metadata_fields=task['result']['metadata_fields'], source_bytes=task['progress']['total_bytes'],
        file=str(path.resolve()), cover_bytes=len(cover), reuse=True, restart_readback=True, range_readback=True,
        seconds=round(time.monotonic() - before, 2))


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--port', type=int, default=18972)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    report = {'status': 'failed', 'authentication': 'anonymous'}
    path_before = os.environ.get('PATH', '')
    # 服务使用绝对路径启动；Windows 系统目录外的 Python/Node/FFmpeg 均不可通过 PATH 调用。
    os.environ['PATH'] = str(Path(os.environ.get('SystemRoot', 'C:/Windows')) / 'System32') if os.name == 'nt' else ''
    service = None
    try:
        check(not any(shutil.which(x) for x in ('ffmpeg', 'ffprobe', 'flac', 'python', 'node')), 'external media and script tools are absent from service PATH')
        report['external_tools_absent_from_service_path'] = True
        service = OwnService(str(Path(args.binary).resolve()), args.port)
        run(service, args.output_dir, report)
    finally:
        if service:
            service.close()
        os.environ['PATH'] = path_before
        (args.output_dir / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
