# 音频样本

固定夹具的历史来源为 ffmpeg 8.1.1，内容为 440 Hz 正弦波，无版权素材，已去掉编码器信息。用于 `tests/unit/audio_processing/tagging_test.cpp` 的 FLAC／MP4 结构验证与标签写入。项目直接使用已保存的样本；开发、测试和维护流程均不要求或调用该外部工具。

| 文件 | 大小 | 内容 |
| --- | --- | --- |
| `sine-3s.flac` | 19,604 字节 | FLAC，8000 Hz，单声道，24000 样本（3.000 秒） |
| `sine-3s.m4a` | 9,407 字节 | 品牌 M4A，AAC，8000 Hz，单声道；顶层盒 ftyp、moov、free、mdat |
| `cover.png` | 67 字节 | 与 tagging_test 的内存样本一致，1×1 透明 PNG；用于封面正文结构检查和 Runner 标签读回，无远程来源 |


NCM 受控样本由 `generate_ncm.ps1` 使用 .NET AES-128-ECB 独立编码，生产解密使用原生 Mbed TLS。`synthetic-mp3.ncm` 包装 `ncm-plain.mp3`（128 个合成结构帧，不是录音），`synthetic-flac.ncm` 包装上述正弦波 FLAC 并在封面帧中保留 8192 字节填充，覆盖新版布局。两个文件都含合成标签、歌词和本地 PNG；不含真实账号信息。解密后的完整字节分别与明文对照比较。运行脚本会重建这三个测试文件，不用于用户音乐。

明文 SHA-256：MP3 `23905ef97bb833dd2b50b7e506c6b9bf4c93aaeb9a80ba9f8190ff902109c0b6`；FLAC `446085753426182f7f756a429e7d6a394a174db142d7adb374a36eb6ef3fbab7`。

## 原生 Z3D／MG3D 合成样本

Z3D／MG3D 使用 `z3d_fixture.hpp` 在测试内生成合成双声道 PCM WAV，覆盖普通与 extensible fmt、零填充和跨窗口数据。加密夹具由公开的固定输入输出向量构造，不调用生产密钥派生函数；没有真实音乐或账号密钥。来源、版本及验证范围见 [Z3D 研究记录](../../../docs/research/migu-z3d.md)。

## 原生 FLAC 完整校验样本

`verify-*.flac` 是历史上用独立编码器制作的固定样本，来源为确定性的合成波形、噪声和常量 PCM。`flac-verification.json` 保存编码前 PCM 的 MD5、样本数、声道与规格，以及成品 SHA-256。覆盖 16／24／32 bit、44.1／48／96 kHz、单声道／双声道和不同子帧；没有真实音乐和账号数据。

运行 `python tests/fixtures/audio/verify_flac_fixtures.py`，仅用 Python 标准库重算原始合成 PCM 摘要和固定文件 SHA-256；不重编码、不改写样本和清单。完整帧解码、CRC／MD5、截断及规格验证由现有 C++ 测试执行。外部编码器生成入口已移除；不得重新引入外部工具维护流程。
