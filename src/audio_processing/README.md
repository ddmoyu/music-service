# audio_processing

AudioProcessor 的实现（端口见 `application/ports/processing`）。下载执行在传输完成后按 ResolvedAudio 的处理计划（processor_id＋版本）从 ProcessorCatalog 取处理器，处理器只读工作文件，返回产物与处理报告；验证失败的文件不得发布。

| 文件 | 内容 |
| --- | --- |
| `format/byte_reader.*` | 64 KiB 窗口的随机读取，文件不整段读入内存 |
| `format/flac.*` | 原生 FLAC：可选前置 ID3v2、fLaC 标记、元数据块链（STREAMINFO 在首位）与首个音频帧同步码；时长取自 STREAMINFO 总样本数；音频字节少于 STREAMINFO 最小帧长所需判为截断 |
| `format/flac_frames.*` | 本项目 C++ 逐帧解码、头 CRC8／帧 CRC16、样本序号及总数、PCM MD5；读取窗口 64 KiB，只保留一个块的样本，支持中途取消与截止时间 |
| `kuwo_flac.*` | `audio.kuwo-flac` v1：完整校验后定位末帧，清理已识别的酷我附加尾部到工作副本；标签写入后再次完整校验，返回最终文件的事实和哈希 |
| `ncm.*` | `audio.ncm` v1：原生 NCM 解密、检查加密传输长度、提取元数据并补充平台缺失字段，调用基线处理器验证明文；保留输入，恢复时重建未发布的明文工作副本 |
| `migu_z3d.*` | `audio.migu-z3d` v1：Android Z3D／旧 MG3D 解密为 WAV；逐曲密钥只在内存中传递，64 KiB 流式处理，失败删除本次明文副本；无外部密钥时仅尝试有界的旧 MG3D 零填充恢复 |
| `format/wav.*` | PCM WAV：RIFF／chunk 边界、fmt 格式与采样对齐、实际帧数及数据 SHA-256；支持 PCM fmt 16／18 字节及 extensible PCM，不支持 RF64、浮点或压缩 WAV |
| `format/mp4.*` | MP4／M4A：顶层盒须恰好铺满文件（ftyp 在首、一个 moov、至少一个 mdat），moov 内须有 soun 轨；时长取自 mvhd，编码取自 stsd 样本项 |
| `tags/tag_writer.*` | TagLib 写入：MP3 只写 ID3v2.4（已有 ID3v1／APE 等保持原样），FLAC 为 Vorbis 注释与图片块，MP4 为 iTunes 原子，WAV 为 RIFF 标签；只改给出的项（标题、歌手列表、专辑、歌词、封面），其他帧与字段保留；保存后重新打开逐项读回 |
| `format/mp3.*` | MPEG 音频帧逐帧遍历：跳过前置 ID3v2 与末尾 ID3v1／APEv2／Lyrics3v2，首帧须与后续帧连成链，统计帧数、实测时长与码率，识别截断、损坏和 Xing／VBRI 声明帧数不足 |
| `verifying_processor.*` | 基线处理器 `audio.verify` v2：按字节识别容器（拒绝 HTML／JSON／文本与未支持容器）、核对传输声明大小、MP3／FLAC／MP4／PCM WAV 校验、按平台声明时长判定整曲／试听；metadata_policy 不为 skip 时把可信元数据写入输入旁的副本 `tagged.<ext>`，副本的音频载荷（帧数／样本数、音频字节、时长；WAV 另含数据 SHA-256）须与输入相同，否则丢弃；记录阶段与警告。ProcessorCatalog 按 id、版本与解密步骤查找 |

Ogg 与 ADTS 可识别但返回 `unsupported_container`。PCM WAV 通过结构校验后可写入 ID3 标签，并复核 PCM 载荷 SHA-256 不变。未知处理计划或解密标识返回 CAPABILITY_UNSUPPORTED，不跳过、不发布。基线 `audio.verify` 保持 FLAC 结构校验，酷我黑胶专用计划才执行完整 FLAC 解码与 PCM 校验。

`audio.verify` v2 在检查标签和音频容器前识别 `CTENFDAM`，将 NCM 委托给原生处理器；解密出的 MP3／FLAC 再经过基线验证，报告记录实际执行的 `audio.ncm`。在线下载使用既有独立 `processing` 线程池；固定线程数及有界队列限制 CPU 并发，取消令牌和截止时间贯穿解密、校验及发布前检查。64 KiB 流式读写，无外部解密程序、转码子进程或每任务新建线程。

原生 FLAC 解码依据 [RFC 9639](https://www.rfc-editor.org/rfc/rfc9639.html)。只在总样本数已知且 STREAMINFO 含非零 MD5 时确认末帧；一致性校验失败时拒绝文件。酷我尾部需匹配 `f000ff0f` 前缀和 `0e55fff0` 后缀且长度在 8～4096 字节之间；长度由验证后的末帧位置决定。`KUWO_TRAILER_REMOVED` 表示仅附加尾部被移除，不是跳过损坏音频。
