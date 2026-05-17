#include "hooker/log.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace roh {

// log 出力の実装です。
// DLL 注入前後と hook 中の両方から呼ばれるため、例外を投げず、失敗しても呼び元を止めません。

namespace {

// launcher と hook DLL の両方から log を書くため、同時書き込みで行が混ざらないよう
// mutex で 1 回の書き込みを保護します。
std::mutex g_log_mutex;
std::wstring g_log_path;

std::wstring Timestamp() {
  SYSTEMTIME st{};
  GetLocalTime(&st);
  wchar_t buffer[64]{};
  swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             st.wMilliseconds);
  return buffer;
}

// 指定 path に追記 open できるかを破壊せずに確認します。
// 開ければそのまま close して true を返します。
bool CanAppendOpen(const std::wstring& path) {
  if (path.empty()) {
    return false;
  }
  FILE* probe = nullptr;
  if (_wfopen_s(&probe, path.c_str(), L"a, ccs=UTF-8") != 0 || !probe) {
    return false;
  }
  fclose(probe);
  return true;
}

// %TEMP%\rpg2000_ogg_hooker.log を返します。
// launcher 直下が Program Files など read-only な場合の最終 fallback です。
std::wstring TempFallbackLogPath() {
  std::vector<wchar_t> buffer(MAX_PATH);
  DWORD length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
  while (length >= buffer.size()) {
    buffer.resize(length + 1);
    length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
  }
  if (length == 0) {
    return L"";
  }
  std::filesystem::path root(std::wstring(buffer.data(), length));
  return (root / L"rpg2000_ogg_hooker.log").wstring();
}

}  // namespace

void InitLog(const std::wstring& log_path) {
  // 優先 path (通常は launcher exe と同じ folder の rpg2000_ogg_hooker.log) に書けるかを
  // まず試します。書けない (例: Program Files 配下に置かれて write 拒否) 時は %TEMP% に
  // 落とします。ここで決まった path を g_log_path に保持し、以後の Log() は決まった先へ追記します。
  std::lock_guard<std::mutex> lock(g_log_mutex);
  std::wstring chosen;
  if (CanAppendOpen(log_path)) {
    chosen = log_path;
  } else {
    const std::wstring fallback = TempFallbackLogPath();
    if (CanAppendOpen(fallback)) {
      chosen = fallback;
    }
  }
  if (chosen.empty()) {
    // 両方失敗。OutputDebugString だけで「log を書けなかった」事実を残します。
    // DebugView 等で観測できれば追跡可能。launcher exe の wmain からは MessageBox も併用します。
    OutputDebugStringW((L"[rpg2000_ogg_hooker] could not open any log path; preferred=\"" +
                        log_path + L"\"")
                           .c_str());
    return;
  }
  g_log_path = chosen;
  FILE* f = nullptr;
  if (_wfopen_s(&f, g_log_path.c_str(), L"a, ccs=UTF-8") == 0 && f) {
    fwprintf(f, L"\n[%s] log_start pid=%lu chosen_path=\"%s\" preferred_path=\"%s\"\n",
             Timestamp().c_str(), GetCurrentProcessId(), g_log_path.c_str(), log_path.c_str());
    fclose(f);
  }
  // launcher 起動時に「どの path に log が出るか」を Explorer 経由ですぐ追えるよう、
  // OutputDebugString にも 1 行流します。DebugView 等で観測可能。
  OutputDebugStringW((L"[rpg2000_ogg_hooker] log_start chosen=\"" + g_log_path + L"\"").c_str());
}

void Log(const std::wstring& message) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  if (g_log_path.empty()) {
    return;
  }
  FILE* f = nullptr;
  _wfopen_s(&f, g_log_path.c_str(), L"a, ccs=UTF-8");
  if (!f) {
    return;
  }
  fwprintf(f, L"[%s] %s\n", Timestamp().c_str(), message.c_str());
  fclose(f);
}

std::wstring LastErrorMessage(unsigned long error_code) {
  // GetLastError() の数値だけでは追いにくいため、Windows の説明文へ変換します。
  wchar_t* raw = nullptr;
  DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                  FORMAT_MESSAGE_IGNORE_INSERTS,
                              nullptr, error_code, 0, reinterpret_cast<wchar_t*>(&raw), 0,
                              nullptr);
  std::wstring text;
  if (size && raw) {
    text.assign(raw, size);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
      text.pop_back();
    }
  }
  if (raw) {
    LocalFree(raw);
  }
  if (text.empty()) {
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"error=%lu", error_code);
    text = buffer;
  }
  return text;
}

void LogLastError(const std::wstring& prefix) {
  // Windows API 失敗直後に呼ぶ helper です。別 API を挟むと GetLastError() が変わります。
  DWORD error = GetLastError();
  Log(prefix + L": " + LastErrorMessage(error));
}

}  // namespace roh
