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


NCM 受控样本由 `generate_ncm.ps1` 使用 .NET AES-128-ECB 独立编码，生产解密使用原生 Mbed TLS。`synthetic-mp3.ncm` 包装 `ncm-plain.mp3`（128 个合成结构帧，不是录音），`synthetic-flac.ncm` 包装上述正弦波 FLAC 并在封面帧中保留 8192 字节填充，覆盖新版布局。两个文件都含合成标签、歌词和本地 PNG；不含真实账号信息。解密后的完整字节分别与明文对照比较。运行脚本会重建这三个测试文件，不用于用户音乐。

明文 SHA-256：MP3 `23905ef97bb833dd2b50b7e506c6b9bf4c93aaeb9a80ba9f8190ff902109c0b6`；FLAC `446085753426182f7f756a429e7d6a394a174db142d7adb374a36eb6ef3fbab7`。
