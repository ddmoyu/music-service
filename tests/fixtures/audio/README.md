# 音频样本

由 ffmpeg 8.1.1 生成，内容为 440 Hz 正弦波，无版权素材；`-map_metadata -1` 与 bitexact 标志去掉编码器信息，便于复现。用于 `tests/unit/audio_processing/tagging_test.cpp` 的 FLAC／MP4 结构验证与标签写入。

```sh
ffmpeg -f lavfi -i "sine=frequency=440:sample_rate=8000:duration=3" -ac 1 -map_metadata -1 -fflags +bitexact -flags:a +bitexact -c:a flac sine-3s.flac
ffmpeg -f lavfi -i "sine=frequency=440:sample_rate=8000:duration=3" -ac 1 -map_metadata -1 -fflags +bitexact -flags:a +bitexact -c:a aac -b:a 24k -movflags +faststart sine-3s.m4a
```

| 文件 | 大小 | 内容 |
| --- | --- | --- |
| `sine-3s.flac` | 19,604 字节 | FLAC，8000 Hz，单声道，24000 样本（3.000 秒） |
| `sine-3s.m4a` | 9,407 字节 | 品牌 M4A，AAC，8000 Hz，单声道；顶层盒 ftyp、moov、free、mdat |
| `cover.png` | 67 字节 | 与 tagging_test 的内存样本一致，1×1 透明 PNG；用于封面正文结构检查和 Runner 标签读回，无远程来源 |
