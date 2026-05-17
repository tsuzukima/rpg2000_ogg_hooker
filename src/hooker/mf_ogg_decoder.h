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
// まず Windows Media Foundation を試し、OS codec が無い場合は ffmpeg.exe へ fallback します。
DecodeResult DecodeOggToWavMediaFoundation(const std::wstring& source_ogg,
                                           const std::wstring& temp_wav);

}  // namespace roh
