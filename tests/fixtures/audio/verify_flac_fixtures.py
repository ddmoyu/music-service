"""仅用 Python 标准库校验固定 FLAC 夹具及原始合成 PCM 摘要，不调用编码器。"""
import hashlib
import json
import math
import random
from pathlib import Path

root = Path(__file__).parent
rng = random.Random(9639)
cases = [
    ('verify-24-96-lpc', 24, 96000, 2, 16384, 12, 'signal'),
    ('verify-16-44-fixed', 16, 44100, 1, 8193, 0, 'signal'),
    ('verify-24-48-verbatim', 24, 48000, 2, 2048, 0, 'noise'),
    ('verify-32-48', 32, 48000, 2, 8192, 5, 'signal'),
    ('verify-24-96-constant', 24, 96000, 2, 128, 5, 'constant'),
]
manifest = []
expected = {item['file']: item for item in json.loads((root / 'flac-verification.json').read_text(encoding='utf-8'))}
for name, depth, rate, channels, count, level, kind in cases:
    pcm = bytearray()
    for i in range(count):
        for ch in range(channels):
            if kind == 'noise':
                value = rng.randrange(-(1 << (depth - 2)), 1 << (depth - 2))
            elif kind == 'constant':
                value = 256 if ch == 0 else -512
            else:
                wave = math.sin(i * 0.013) * 0.55 + math.sin(i * (0.031 + ch * 0.002)) * 0.13
                value = round(wave * ((1 << (depth - 2)) - 1))
            pcm.extend(value.to_bytes(depth // 8, 'little', signed=True))
    out = root / (name + '.flac')
    actual = {'file': out.name, 'bits_per_sample': depth, 'sample_rate_hz': rate,
                     'channels': channels, 'total_samples': count, 'pcm_md5': hashlib.md5(pcm).hexdigest(),
                     'file_sha256': hashlib.sha256(out.read_bytes()).hexdigest()}
    if actual != expected.get(out.name):
        raise SystemExit(f'fixture or PCM mismatch: {out.name}')
    manifest.append(actual)
if len(manifest) != len(expected):
    raise SystemExit('fixture manifest contains unexpected entries')
print(json.dumps(manifest, indent=2))
