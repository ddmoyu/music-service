# Test-only, deterministic NCM encoder using .NET AES, independent of the C++ decoder.
# Input FLAC and PNG are the existing synthetic fixtures. MP3 frames are structural
# test data, not a recording. No production code or external program is used.
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
function Bytes([string]$text) { return [Text.Encoding]::UTF8.GetBytes($text) }
function Encrypt([byte[]]$data, [string]$key) {
    $aes = [Security.Cryptography.Aes]::Create()
    try {
        $aes.Mode = [Security.Cryptography.CipherMode]::ECB
        $aes.Padding = [Security.Cryptography.PaddingMode]::PKCS7
        $aes.Key = Bytes $key
        $enc = $aes.CreateEncryptor()
        try { return $enc.TransformFinalBlock($data, 0, $data.Length) } finally { $enc.Dispose() }
    } finally { $aes.Dispose() }
}
$streamKey = Bytes 'deterministic-independent-test-key'
$encryptedKey = Encrypt ((Bytes 'neteasecloudmusic') + $streamKey) 'hzHRAmso5kInbaxW'
for ($i=0; $i -lt $encryptedKey.Length; $i++) { $encryptedKey[$i] = $encryptedKey[$i] -bxor 0x64 }
$box = [byte[]](0..255)
$j = 0
for ($i=0; $i -lt 256; $i++) {
    $j = ($j + $box[$i] + $streamKey[$i % $streamKey.Length]) % 256
    $temp = $box[$i]; $box[$i] = $box[$j]; $box[$j] = $temp
}
$pad = New-Object byte[] 8192
$cover = [IO.File]::ReadAllBytes((Join-Path $root 'cover.png'))
$frame = New-Object byte[] 417
$frame[0] = 0xff; $frame[1] = 0xfb; $frame[2] = 0x90; $frame[3] = 0x40
$mp3 = [byte[]]($frame * 128)
[IO.File]::WriteAllBytes((Join-Path $root 'ncm-plain.mp3'), $mp3)
foreach ($format in @('mp3', 'flac')) {
    $plain = if ($format -eq 'mp3') { $mp3 } else { [IO.File]::ReadAllBytes((Join-Path $root 'sine-3s.flac')) }
    $metadata = [ordered]@{musicName='NCM synthetic'; artist=@(,@('Synthetic artist',1)); album='Synthetic album';
        format=$format; lyric="[00:00.00]Synthetic lyric`n"} | ConvertTo-Json -Compress -Depth 5
    $meta = Bytes ("163 key(Don't modify):" + [Convert]::ToBase64String((Encrypt (Bytes ('music:' + $metadata)) '#14ljk_!\]&0U<''(')))
    for ($i=0; $i -lt $meta.Length; $i++) { $meta[$i] = $meta[$i] -bxor 0x63 }
    $audio = New-Object byte[] $plain.Length
    for ($i=0; $i -lt $audio.Length; $i++) {
        $k = ($i + 1) % 256
        $audio[$i] = $plain[$i] -bxor $box[($box[$k] + $box[($box[$k] + $k) % 256]) % 256]
    }
    $file = [IO.File]::Create((Join-Path $root "synthetic-$format.ncm"))
    $writer = [IO.BinaryWriter]::new($file)
    try {
        $writer.Write([byte[]](Bytes 'CTENFDAM')); $writer.Write([uint16]0)
        $writer.Write([uint32]$encryptedKey.Length); $writer.Write([byte[]]$encryptedKey)
        $writer.Write([uint32]$meta.Length); $writer.Write([byte[]]$meta)
        $writer.Write([uint32]0); $writer.Write([byte]0)
        $padding = if ($format -eq 'flac') { $pad } else { [byte[]]@() }
        $writer.Write([uint32]($cover.Length + $padding.Length)); $writer.Write([uint32]$cover.Length)
        $writer.Write($cover); if ($padding) { $writer.Write([byte[]]$padding) }; $writer.Write($audio)
    } finally { $writer.Dispose(); $file.Dispose() }
}
