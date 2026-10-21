# audio_processing

AudioProcessor 的实现（端口见 `application/ports/processing`）。下载执行在传输完成后按 ResolvedAudio 的处理计划（processor_id＋版本）从 ProcessorCatalog 取处理器，处理器只读工作文件，返回产物与处理报告；验证失败的文件不得发布。

| 文件 | 内容 |
| --- | --- |
| `format/byte_reader.*` | 64 KiB 窗口的随机读取，文件不整段读入内存 |
| `format/mp3.*` | MPEG 音频帧逐帧遍历：跳过前置 ID3v2 与末尾 ID3v1／APEv2／Lyrics3v2，首帧须与后续帧连成链，统计帧数、实测时长与码率，识别截断、损坏和 Xing／VBRI 声明帧数不足 |
| `verifying_processor.*` | 基线处理器 `audio.verify` v1：按字节识别容器（拒绝 HTML／JSON／文本与未支持容器）、核对传输声明大小、MP3 结构校验、按平台声明时长判定整曲／试听、记录阶段与警告；ProcessorCatalog 按 id 与版本查找 |

当前只校验 MP3（酷我标准品质的实际容器）；FLAC、M4A、Ogg、WAV 与 ADTS 可识别但返回 `unsupported_container`，相应验证、标签写入和解密由 T30 及具体平台任务加入。不做完整解码证明。
