#include "hooker/mf_ogg_decoder.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "hooker/paths.h"

namespace roh {

// OGG を editor が読める一時 WAV に変換する実装です。
// Media Foundation が使えない環境でも落とさず、ffmpeg.exe があれば fallback します。

namespace {

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

DecodeResult DecodeWithFfmpeg(const std::wstring& source_ogg, const std::wstring& temp_wav,
                              DWORD started, const std::wstring& previous_error) {
  // Windows に OGG/Vorbis codec が無い環境用の fallback です。
  // .partial に出力してから MoveFileExW で完成品として公開することで、
  // 変換途中の壊れた WAV を editor が読む事故を避けます。
  std::filesystem::path out_path(temp_wav);
  EnsureDirectoryTree(out_path.parent_path().wstring());
  const std::wstring partial = temp_wav + L".partial";
  DeleteFileW(partial.c_str());

  std::wstring command = L"ffmpeg.exe -hide_banner -loglevel error -y -i " +
                         QuoteArg(source_ogg) + L" -acodec pcm_s16le -f wav " +
                         QuoteArg(partial);
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
                previous_error + L"; ffmpeg.exe fallback could not be started", started);
  }

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exit_code = 1;
  GetExitCodeProcess(pi.hProcess, &exit_code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (exit_code != 0) {
    DeleteFileW(partial.c_str());
    return Fail(HRESULT_FROM_WIN32(ERROR_GEN_FAILURE),
                previous_error + L"; ffmpeg.exe fallback failed exit_code=" +
                    std::to_wstring(exit_code),
                started);
  }

  DeleteFileW(temp_wav.c_str());
  if (!MoveFileExW(partial.c_str(), temp_wav.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(partial.c_str());
    return Fail(HRESULT_FROM_WIN32(GetLastError()),
                previous_error + L"; ffmpeg.exe fallback could not publish temp wav", started);
  }

  WIN32_FILE_ATTRIBUTE_DATA data{};
  unsigned long long bytes = 0;
  if (GetFileAttributesExW(temp_wav.c_str(), GetFileExInfoStandard, &data)) {
    bytes = (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
  }
  DecodeResult result;
  result.ok = true;
  result.decode_ms = GetTickCount() - started;
  result.bytes = bytes;
  result.message = L"decoded by ffmpeg.exe fallback";
  return result;
}

}  // namespace

DecodeResult DecodeOggToWavMediaFoundation(const std::wstring& source_ogg,
                                           const std::wstring& temp_wav) {
  // Media Foundation は Windows 標準の音声/動画 API です。
  // 環境によって OGG/Vorbis decoder が入っていないため、失敗したら ffmpeg に任せます。
  const DWORD started = GetTickCount();
  DecodeResult result;

  std::filesystem::path out_path(temp_wav);
  EnsureDirectoryTree(out_path.parent_path().wstring());
  const std::wstring partial = temp_wav + L".partial";
  DeleteFileW(partial.c_str());

  HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
  if (FAILED(hr)) {
    return DecodeWithFfmpeg(source_ogg, temp_wav, started, L"MFStartup failed");
  }

  IMFSourceReader* reader = nullptr;
  hr = MFCreateSourceReaderFromURL(source_ogg.c_str(), nullptr, &reader);
  if (FAILED(hr)) {
    MFShutdown();
    return DecodeWithFfmpeg(source_ogg, temp_wav, started,
                            L"MFCreateSourceReaderFromURL failed; OGG/Vorbis codec may be unavailable");
  }

  IMFMediaType* desired_type = nullptr;
  hr = MFCreateMediaType(&desired_type);
  if (FAILED(hr)) {
    ReleaseIf(&reader);
    MFShutdown();
    return DecodeWithFfmpeg(source_ogg, temp_wav, started, L"MFCreateMediaType failed");
  }
  desired_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  desired_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
  hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, desired_type);
  ReleaseIf(&desired_type);
  if (FAILED(hr)) {
    ReleaseIf(&reader);
    MFShutdown();
    return DecodeWithFfmpeg(source_ogg, temp_wav, started, L"SetCurrentMediaType(PCM) failed");
  }

  IMFMediaType* current_type = nullptr;
  hr = reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &current_type);
  if (FAILED(hr)) {
    ReleaseIf(&reader);
    MFShutdown();
    return DecodeWithFfmpeg(source_ogg, temp_wav, started, L"GetCurrentMediaType failed");
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
    return DecodeWithFfmpeg(source_ogg, temp_wav, started, L"decoded PCM format is invalid");
  }

  std::ofstream out(std::filesystem::path(partial), std::ios::binary | std::ios::trunc);
  if (!out) {
    ReleaseIf(&reader);
    MFShutdown();
    return DecodeWithFfmpeg(source_ogg, temp_wav, started, L"could not create temp wav");
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
      return DecodeWithFfmpeg(source_ogg, temp_wav, started, L"ReadSample failed");
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
      return DecodeWithFfmpeg(source_ogg, temp_wav, started, L"ConvertToContiguousBuffer failed");
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
      return DecodeWithFfmpeg(source_ogg, temp_wav, started, L"MediaBuffer::Lock failed");
    }
    out.write(reinterpret_cast<const char*>(data), current_length);
    data_bytes += current_length;
    buffer->Unlock();
    ReleaseIf(&buffer);
  }

  out.seekp(0, std::ios::beg);
  const std::uint32_t capped_data =
      data_bytes > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(data_bytes);
  WriteWavHeader(out, static_cast<std::uint16_t>(channels), sample_rate,
                 static_cast<std::uint16_t>(bits_per_sample), capped_data);
  out.close();

  DeleteFileW(temp_wav.c_str());
  if (!MoveFileExW(partial.c_str(), temp_wav.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(partial.c_str());
    ReleaseIf(&reader);
    MFShutdown();
    return DecodeWithFfmpeg(source_ogg, temp_wav, started, L"could not publish temp wav");
  }

  ReleaseIf(&reader);
  MFShutdown();
  result.ok = true;
  result.decode_ms = GetTickCount() - started;
  result.bytes = data_bytes + 44;
  return result;
}

}  // namespace roh
