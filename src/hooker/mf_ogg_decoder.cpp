#include "hooker/mf_ogg_decoder.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "hooker/paths.h"

// stb_vorbis (public-domain Vorbis decoder by Sean Barrett) を pull/streaming で使います。
// 実装本体は third_party/stb_vorbis.c に分離 compile されており、ここでは declarations だけ。
// push-data API は不要なので、binary を縮めるため build 全体で STB_VORBIS_NO_PUSHDATA_API を有効にしています。
extern "C" {
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_HEADER_ONLY
#include "hooker/third_party/stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY
}

namespace roh {

// OGG を editor が読める一時 WAV に変換する実装です。
// stb_vorbis -> Media Foundation -> ffmpeg の順で試し、どこかで成功したら return します。
// 3 段とも fail の時だけ ok=false で戻り、editor は元の探索経路 (= MIDI 等) へ戻ります。

namespace {

// decoder chain の有効 stage を選びます。
// launcher が test flag (--stb-only 等) を env var に変換して下ろします。
// env var 未設定 = 全 stage 有効 (production default)。
struct DecoderStageSelection {
  bool use_stb = true;
  bool use_mf = true;
  bool use_ffmpeg = true;
};

DecoderStageSelection ReadDecoderStageSelection() {
  DecoderStageSelection selection;
  std::vector<wchar_t> buffer(256);
  DWORD len = GetEnvironmentVariableW(L"RPG2000_OGG_HOOKER_DECODE_CHAIN", buffer.data(),
                                       static_cast<DWORD>(buffer.size()));
  if (len == 0 || len >= buffer.size()) {
    return selection;
  }
  // 明示的に指定された場合は、いったん全 off にしてから token を on に立てます。
  selection = {false, false, false};
  std::wstring value(buffer.data(), len);
  auto absorb = [&](std::wstring& token) {
    while (!token.empty() && (token.front() == L' ' || token.front() == L'\t')) {
      token.erase(0, 1);
    }
    while (!token.empty() && (token.back() == L' ' || token.back() == L'\t')) {
      token.pop_back();
    }
    std::wstring lower;
    lower.reserve(token.size());
    for (wchar_t ch : token) {
      lower += (ch >= L'A' && ch <= L'Z') ? static_cast<wchar_t>(ch - L'A' + L'a') : ch;
    }
    if (lower == L"stb") {
      selection.use_stb = true;
    } else if (lower == L"mf") {
      selection.use_mf = true;
    } else if (lower == L"ffmpeg") {
      selection.use_ffmpeg = true;
    }
    token.clear();
  };
  std::wstring token;
  for (wchar_t ch : value) {
    if (ch == L',') {
      absorb(token);
    } else {
      token += ch;
    }
  }
  absorb(token);
  return selection;
}

// magic static で process 起動 1 回だけ env var を読みます。
const DecoderStageSelection& Selection() {
  static const DecoderStageSelection s = ReadDecoderStageSelection();
  return s;
}

// Media Foundation の COM object は Release() が必要です。
// T** を受け取る形にして、Release 後に nullptr へ戻すことで二重解放を避けます。
template <typename T>
void ReleaseIf(T** value) {
  if (value && *value) {
    (*value)->Release();
    *value = nullptr;
  }
}

void WriteLe16(std::ofstream& out, std::uint16_t value) {
  char bytes[2] = {static_cast<char>(value & 0xFF), static_cast<char>((value >> 8) & 0xFF)};
  out.write(bytes, 2);
}

void WriteLe32(std::ofstream& out, std::uint32_t value) {
  char bytes[4] = {
      static_cast<char>(value & 0xFF),
      static_cast<char>((value >> 8) & 0xFF),
      static_cast<char>((value >> 16) & 0xFF),
      static_cast<char>((value >> 24) & 0xFF),
  };
  out.write(bytes, 4);
}

void WriteWavHeader(std::ofstream& out, std::uint16_t channels, std::uint32_t sample_rate,
                    std::uint16_t bits_per_sample, std::uint32_t data_size) {
  // editor へ読ませるのは普通の RIFF/WAVE PCM です。
  // data_size は最後まで decode しないと分からないため、最初は 0 で書き、
  // decode 完了後に file 先頭へ戻って正しい値で上書きします。
  const std::uint16_t block_align = static_cast<std::uint16_t>((channels * bits_per_sample) / 8);
  const std::uint32_t byte_rate = sample_rate * block_align;
  out.write("RIFF", 4);
  WriteLe32(out, 36 + data_size);
  out.write("WAVE", 4);
  out.write("fmt ", 4);
  WriteLe32(out, 16);
  WriteLe16(out, 1);
  WriteLe16(out, channels);
  WriteLe32(out, sample_rate);
  WriteLe32(out, byte_rate);
  WriteLe16(out, block_align);
  WriteLe16(out, bits_per_sample);
  out.write("data", 4);
  WriteLe32(out, data_size);
}

DecodeResult Fail(HRESULT hr, const std::wstring& message, DWORD start_tick) {
  DecodeResult result;
  result.ok = false;
  result.error = static_cast<unsigned long>(hr);
  result.decode_ms = GetTickCount() - start_tick;
  result.message = message;
  return result;
}

std::wstring QuoteArg(const std::wstring& arg) {
  std::wstring out = L"\"";
  for (wchar_t ch : arg) {
    if (ch == L'"') {
      out += L"\\\"";
    } else {
      out += ch;
    }
  }
  out += L"\"";
  return out;
}

// この hook DLL 自身が常駐している directory を返します。
// 通常配置では launcher exe / host exe と同じ folder ですが、
// 配置ずれでも DLL 横を見るのが本来の正解なので、host exe ではなく
// この関数の address を起点に GetModuleHandleExW で自分の HMODULE を得て解決します。
std::wstring OwnModuleDirectory() {
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(&OwnModuleDirectory), &module)) {
    return L"";
  }
  std::vector<wchar_t> buffer(MAX_PATH);
  for (;;) {
    DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      return L"";
    }
    if (length + 1 < buffer.size()) {
      std::filesystem::path path(std::wstring(buffer.data(), length));
      return path.parent_path().wstring();
    }
    buffer.resize(buffer.size() * 2);
  }
}

// ffmpeg.exe の location を決めます。
// 1) この hook DLL 横、2) launcher exe 横、の順で実在 check し、なければ bare "ffmpeg.exe"
// (= Windows の PATH/cwd 検索) を返します。
// bare 検索だけだと意図しない別 binary を引きうるという指摘への対応です。
std::wstring ResolveFfmpegCommand() {
  for (const std::wstring& base : {OwnModuleDirectory(), roh::ExecutableDirectory()}) {
    if (base.empty()) {
      continue;
    }
    std::filesystem::path candidate = std::filesystem::path(base) / L"ffmpeg.exe";
    DWORD attrs = GetFileAttributesW(candidate.wstring().c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      return candidate.wstring();
    }
  }
  return L"ffmpeg.exe";
}

DecodeResult PublishWav(const std::wstring& partial, const std::wstring& temp_wav,
                        DWORD started, std::uint64_t data_bytes,
                        const std::wstring& success_message,
                        const std::wstring& previous_error) {
  DeleteFileW(temp_wav.c_str());
  if (!MoveFileExW(partial.c_str(), temp_wav.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    DWORD err = GetLastError();
    DeleteFileW(partial.c_str());
    return Fail(HRESULT_FROM_WIN32(err),
                previous_error + L"; " + success_message + L" could not publish temp wav",
                started);
  }
  WIN32_FILE_ATTRIBUTE_DATA data{};
  unsigned long long bytes = data_bytes;
  if (GetFileAttributesExW(temp_wav.c_str(), GetFileExInfoStandard, &data)) {
    bytes = (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
  }
  DecodeResult result;
  result.ok = true;
  result.decode_ms = GetTickCount() - started;
  result.bytes = bytes;
  result.message = success_message;
  return result;
}

// stb_vorbis を pull API で使い、source_ogg を 16-bit PCM の一時 WAV へ stream decode します。
// 大 BGM でもメモリ全展開せず、固定 buffer (4096 sample * channel) を回します。
// > 2 channel の Vorbis は stereo へ downmix します (RM2K は基本 mono/stereo)。
DecodeResult DecodeWithStbVorbis(const std::wstring& source_ogg, const std::wstring& temp_wav,
                                 DWORD started) {
  std::filesystem::path out_path(temp_wav);
  EnsureDirectoryTree(out_path.parent_path().wstring());
  const std::wstring partial = temp_wav + L".partial";
  DeleteFileW(partial.c_str());

  FILE* fp = nullptr;
  errno_t open_err = _wfopen_s(&fp, source_ogg.c_str(), L"rb");
  if (open_err != 0 || !fp) {
    return Fail(HRESULT_FROM_WIN32(ERROR_OPEN_FAILED),
                L"stb_vorbis _wfopen_s failed", started);
  }

  int vorbis_err = 0;
  // close_handle_on_close=1: 成功/失敗どちらでも stb_vorbis_close() 経由で fp は閉じられます。
  stb_vorbis* vorbis = stb_vorbis_open_file(fp, 1, &vorbis_err, nullptr);
  if (!vorbis) {
    fclose(fp);
    return Fail(HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
                L"stb_vorbis_open_file failed err=" + std::to_wstring(vorbis_err), started);
  }

  stb_vorbis_info info = stb_vorbis_get_info(vorbis);
  if (info.sample_rate == 0 || info.channels <= 0) {
    stb_vorbis_close(vorbis);
    return Fail(HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
                L"stb_vorbis_get_info returned invalid format", started);
  }
  // multichannel downmix policy: source が 2ch 以上なら出力は stereo に固定。
  const int target_channels = info.channels >= 2 ? 2 : 1;

  std::ofstream out(std::filesystem::path(partial), std::ios::binary | std::ios::trunc);
  if (!out) {
    stb_vorbis_close(vorbis);
    return Fail(HRESULT_FROM_WIN32(ERROR_OPEN_FAILED),
                L"stb_vorbis could not create temp wav", started);
  }
  WriteWavHeader(out, static_cast<std::uint16_t>(target_channels),
                 static_cast<std::uint32_t>(info.sample_rate), 16, 0);

  constexpr int kSamplesPerPull = 4096;
  std::vector<short> buffer(static_cast<size_t>(kSamplesPerPull) * static_cast<size_t>(target_channels));
  std::uint64_t data_bytes = 0;
  for (;;) {
    int samples_per_channel = stb_vorbis_get_samples_short_interleaved(
        vorbis, target_channels, buffer.data(), static_cast<int>(buffer.size()));
    if (samples_per_channel <= 0) {
      break;
    }
    const size_t shorts = static_cast<size_t>(samples_per_channel) * static_cast<size_t>(target_channels);
    out.write(reinterpret_cast<const char*>(buffer.data()),
              static_cast<std::streamsize>(shorts * sizeof(short)));
    data_bytes += static_cast<std::uint64_t>(shorts * sizeof(short));
  }

  stb_vorbis_close(vorbis);

  if (data_bytes == 0) {
    out.close();
    DeleteFileW(partial.c_str());
    return Fail(HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
                L"stb_vorbis produced no samples", started);
  }

  out.seekp(0, std::ios::beg);
  const std::uint32_t capped_data =
      data_bytes > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(data_bytes);
  WriteWavHeader(out, static_cast<std::uint16_t>(target_channels),
                 static_cast<std::uint32_t>(info.sample_rate), 16, capped_data);
  out.close();

  return PublishWav(partial, temp_wav, started, data_bytes + 44, L"decoded by stb_vorbis", L"");
}

DecodeResult DecodeWithMediaFoundation(const std::wstring& source_ogg,
                                       const std::wstring& temp_wav, DWORD started,
                                       const std::wstring& previous_error) {
  // Media Foundation は Windows 標準の音声/動画 API です。
  // OGG/Vorbis decoder は Win10 1809 以降 Web Media Extensions に分離されたので、
  // 環境 (N edition / LTSC / Store 無効 / 古い Windows) によっては存在しません。
  std::filesystem::path out_path(temp_wav);
  EnsureDirectoryTree(out_path.parent_path().wstring());
  const std::wstring partial = temp_wav + L".partial";
  DeleteFileW(partial.c_str());

  HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
  if (FAILED(hr)) {
    return Fail(hr, previous_error + L"; MFStartup failed", started);
  }

  IMFSourceReader* reader = nullptr;
  hr = MFCreateSourceReaderFromURL(source_ogg.c_str(), nullptr, &reader);
  if (FAILED(hr)) {
    MFShutdown();
    return Fail(hr, previous_error +
                        L"; MFCreateSourceReaderFromURL failed; OGG/Vorbis codec may be unavailable",
                    started);
  }

  IMFMediaType* desired_type = nullptr;
  hr = MFCreateMediaType(&desired_type);
  if (FAILED(hr)) {
    ReleaseIf(&reader);
    MFShutdown();
    return Fail(hr, previous_error + L"; MFCreateMediaType failed", started);
  }
  desired_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  desired_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
  hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, desired_type);
  ReleaseIf(&desired_type);
  if (FAILED(hr)) {
    ReleaseIf(&reader);
    MFShutdown();
    return Fail(hr, previous_error + L"; SetCurrentMediaType(PCM) failed", started);
  }

  IMFMediaType* current_type = nullptr;
  hr = reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &current_type);
  if (FAILED(hr)) {
    ReleaseIf(&reader);
    MFShutdown();
    return Fail(hr, previous_error + L"; GetCurrentMediaType failed", started);
  }

  UINT32 channels = 0;
  UINT32 sample_rate = 0;
  UINT32 bits_per_sample = 16;
  current_type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
  current_type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sample_rate);
  current_type->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits_per_sample);
  ReleaseIf(&current_type);
  if (channels == 0 || sample_rate == 0 || bits_per_sample == 0 || bits_per_sample > 32) {
    ReleaseIf(&reader);
    MFShutdown();
    return Fail(E_UNEXPECTED, previous_error + L"; decoded PCM format is invalid", started);
  }

  std::ofstream out(std::filesystem::path(partial), std::ios::binary | std::ios::trunc);
  if (!out) {
    ReleaseIf(&reader);
    MFShutdown();
    return Fail(HRESULT_FROM_WIN32(ERROR_OPEN_FAILED),
                previous_error + L"; could not create temp wav", started);
  }
  WriteWavHeader(out, static_cast<std::uint16_t>(channels), sample_rate,
                 static_cast<std::uint16_t>(bits_per_sample), 0);

  std::uint64_t data_bytes = 0;
  for (;;) {
    DWORD stream_index = 0;
    DWORD flags = 0;
    LONGLONG timestamp = 0;
    IMFSample* sample = nullptr;
    hr = reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &stream_index, &flags,
                            &timestamp, &sample);
    if (FAILED(hr)) {
      ReleaseIf(&sample);
      out.close();
      DeleteFileW(partial.c_str());
      ReleaseIf(&reader);
      MFShutdown();
      return Fail(hr, previous_error + L"; ReadSample failed", started);
    }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
      ReleaseIf(&sample);
      break;
    }
    if (!sample) {
      continue;
    }

    IMFMediaBuffer* buffer = nullptr;
    hr = sample->ConvertToContiguousBuffer(&buffer);
    ReleaseIf(&sample);
    if (FAILED(hr)) {
      out.close();
      DeleteFileW(partial.c_str());
      ReleaseIf(&reader);
      MFShutdown();
      return Fail(hr, previous_error + L"; ConvertToContiguousBuffer failed", started);
    }

    BYTE* data = nullptr;
    DWORD max_length = 0;
    DWORD current_length = 0;
    hr = buffer->Lock(&data, &max_length, &current_length);
    if (FAILED(hr)) {
      ReleaseIf(&buffer);
      out.close();
      DeleteFileW(partial.c_str());
      ReleaseIf(&reader);
      MFShutdown();
      return Fail(hr, previous_error + L"; MediaBuffer::Lock failed", started);
    }
    out.write(reinterpret_cast<const char*>(data), current_length);
    data_bytes += current_length;
    buffer->Unlock();
    ReleaseIf(&buffer);
  }

  ReleaseIf(&reader);
  MFShutdown();

  out.seekp(0, std::ios::beg);
  const std::uint32_t capped_data =
      data_bytes > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(data_bytes);
  WriteWavHeader(out, static_cast<std::uint16_t>(channels), sample_rate,
                 static_cast<std::uint16_t>(bits_per_sample), capped_data);
  out.close();

  return PublishWav(partial, temp_wav, started, data_bytes + 44,
                    L"decoded by Media Foundation", previous_error);
}

DecodeResult DecodeWithFfmpeg(const std::wstring& source_ogg, const std::wstring& temp_wav,
                              DWORD started, const std::wstring& previous_error) {
  // 最後の escape hatch。stb_vorbis が拒否する chained stream / 破損 / Vorbis 以外を inside-OGG container
  // で来た場合などに使います。tool 同梱 directory の ffmpeg.exe を優先し、無ければ PATH 検索 fallback。
  std::filesystem::path out_path(temp_wav);
  EnsureDirectoryTree(out_path.parent_path().wstring());
  const std::wstring partial = temp_wav + L".partial";
  DeleteFileW(partial.c_str());

  const std::wstring ffmpeg = ResolveFfmpegCommand();
  std::wstring command = QuoteArg(ffmpeg) +
                         L" -hide_banner -loglevel error -y -i " + QuoteArg(source_ogg) +
                         L" -acodec pcm_s16le -f wav " + QuoteArg(partial);
  std::vector<wchar_t> mutable_command(command.begin(), command.end());
  mutable_command.push_back(L'\0');

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    return Fail(HRESULT_FROM_WIN32(GetLastError()),
                previous_error + L"; ffmpeg fallback could not be started (resolved=\"" + ffmpeg +
                    L"\")",
                started);
  }

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exit_code = 1;
  GetExitCodeProcess(pi.hProcess, &exit_code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (exit_code != 0) {
    DeleteFileW(partial.c_str());
    return Fail(HRESULT_FROM_WIN32(ERROR_GEN_FAILURE),
                previous_error + L"; ffmpeg fallback failed exit_code=" +
                    std::to_wstring(exit_code) + L" resolved=\"" + ffmpeg + L"\"",
                started);
  }

  return PublishWav(partial, temp_wav, started, 0,
                    L"decoded by ffmpeg fallback (resolved=\"" + ffmpeg + L"\")",
                    previous_error);
}

}  // namespace

DecodeResult DecodeOggToWav(const std::wstring& source_ogg, const std::wstring& temp_wav) {
  const DWORD started = GetTickCount();
  const DecoderStageSelection& sel = Selection();

  if (!sel.use_stb && !sel.use_mf && !sel.use_ffmpeg) {
    return Fail(E_UNEXPECTED,
                L"all decoder stages disabled via RPG2000_OGG_HOOKER_DECODE_CHAIN env var",
                started);
  }

  DecodeResult last;
  last.ok = false;
  std::wstring previous_error;

  if (sel.use_stb) {
    DecodeResult r = DecodeWithStbVorbis(source_ogg, temp_wav, started);
    if (r.ok) {
      return r;
    }
    last = r;
    previous_error = r.message;
  }
  if (sel.use_mf) {
    DecodeResult r = DecodeWithMediaFoundation(source_ogg, temp_wav, started, previous_error);
    if (r.ok) {
      return r;
    }
    last = r;
    previous_error = r.message;
  }
  if (sel.use_ffmpeg) {
    DecodeResult r = DecodeWithFfmpeg(source_ogg, temp_wav, started, previous_error);
    if (r.ok) {
      return r;
    }
    last = r;
  }
  return last;
}

}  // namespace roh
