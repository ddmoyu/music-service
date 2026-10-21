# audio_processing

AudioProcessor 的实现（端口见 `application/ports/processing`）。下载执行在传输完成后按 ResolvedAudio 的处理计划（processor_id＋版本）从 ProcessorCatalog 取处理器，处理器只读工作文件，返回产物与处理报告；验证失败的文件不得发布。

| 文件 | 内容 |
| --- | --- |
| `format/byte_reader.*` | 64 KiB 窗口的随机读取，文件不整段读入内存 |
| `format/flac.*` | 原生 FLAC：可选前置 ID3v2、fLaC 标记、元数据块链（STREAMINFO 在首位）与首个音频帧同步码；时长取自 STREAMINFO 总样本数；音频字节少于 STREAMINFO 最小帧长所需判为截断 |
| `format/mp4.*` | MP4／M4A：顶层盒须恰好铺满文件（ftyp 在首、一个 moov、至少一个 mdat），moov 内须有 soun 轨；时长取自 mvhd，编码取自 stsd 样本项 |
| `tags/tag_writer.*` | TagLib 写入：MP3 只写 ID3v2.4（已有 ID3v1／APE 等保持原样），FLAC 为 Vorbis 注释与图片块，MP4 为 iTunes 原子；只改给出的项（标题、歌手列表、专辑、歌词、封面），其他帧与字段保留；保存后重新打开逐项读回 |
| `format/mp3.*` | MPEG 音频帧逐帧遍历：跳过前置 ID3v2 与末尾 ID3v1／APEv2／Lyrics3v2，首帧须与后续帧连成链，统计帧数、实测时长与码率，识别截断、损坏和 Xing／VBRI 声明帧数不足 |
| `verifying_processor.*` | 基线处理器 `audio.verify` v2：按字节识别容器（拒绝 HTML／JSON／文本与未支持容器）、核对传输声明大小、MP3／FLAC／MP4 结构校验、按平台声明时长判定整曲／试听；metadata_policy 不为 skip 时把可信元数据写入输入旁的副本 `tagged.<ext>`，副本的音频载荷（帧数／样本数、音频字节、时长）须与输入相同，否则丢弃；记录阶段与警告。ProcessorCatalog 按 id、版本与解密步骤查找 |

Ogg、WAV 与 ADTS 可识别但返回 `unsupported_container`。解密只有端口（`application/ports/processing/decryptor.hpp`），没有内置实现：处理计划声明解密而没有对应处理器时返回 CAPABILITY_UNSUPPORTED，不跳过、不发布。不做完整解码证明。
