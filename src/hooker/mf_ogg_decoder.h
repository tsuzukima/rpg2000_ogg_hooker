#pragma once

#include <string>

namespace roh {

// audio overlay から呼ばれる OGG decoder の公開 API です。
// 変換できない時は ok=false で返し、editor の本来の file 探索へ戻れるようにします。
//
// OGG -> WAV 変換の結果です。
// ok=false の時は message に原因、error に HRESULT/Win32 error が入ります。
struct DecodeResult {
  bool ok = false;
  unsigned long error = 0;
  unsigned long decode_ms = 0;
  unsigned long long bytes = 0;
  std::wstring message;
};

// OGG/Vorbis を一時 WAV に変換します。
// 実装は 3 段:
//   1. stb_vorbis 内蔵 decoder で stream decode (任意 Windows で常に動く primary path)
//   2. Windows Media Foundation (Web Media Extensions が入っている Windows でのみ成功)
//   3. tool 同梱 directory 経由 -> PATH 経由の ffmpeg.exe fallback
// channel layout が 3ch 以上のときは stereo へ downmix します
// (PCM WAV は channel mask 無しだと 3ch 以上が曖昧なため、再生互換を優先)。
DecodeResult DecodeOggToWav(const std::wstring& source_ogg,
                            const std::wstring& temp_wav);

}  // namespace roh
