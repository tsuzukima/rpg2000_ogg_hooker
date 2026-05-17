#include "hooker/paths.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace roh {

// path helper の実装です。
// RPG2000.exe は ANSI/CP932 path しか渡してこないため、wide string と ANSI の境界を
// このファイルに寄せて、hook 側で同じ変換を再実装しないようにしています。

// 実行中の exe のフルパスを取得します。
// launcher では RPG2000_ogg_hooker.exe、hook DLL 内では RPG2000.exe の path になります。
std::wstring ExecutablePath() {
  std::vector<wchar_t> buffer(MAX_PATH);
  DWORD length = 0;
  for (;;) {
    length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      return L"";
    }
    if (length + 1 < buffer.size()) {
      return std::wstring(buffer.data(), length);
    }
    buffer.resize(buffer.size() * 2);
  }
}

std::wstring ExecutableDirectory() {
  std::filesystem::path path(ExecutablePath());
  return path.parent_path().wstring();
}

std::wstring JoinPath(const std::wstring& lhs, const std::wstring& rhs) {
  if (lhs.empty()) {
    return rhs;
  }
  return (std::filesystem::path(lhs) / rhs).wstring();
}

std::wstring TempRoot() {
  // OGG → 一時 WAV decode の保存先 root です。
  // 例: %TEMP%\rpg2000_ogg_hooker
  std::vector<wchar_t> buffer(MAX_PATH);
  DWORD length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
  while (length >= buffer.size()) {
    buffer.resize(length + 1);
    length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
  }
  std::filesystem::path root(std::wstring(buffer.data(), length));
  return (root / L"rpg2000_ogg_hooker").wstring();
}

std::wstring SessionTempRoot(unsigned long pid) {
  wchar_t pid_text[32]{};
  swprintf_s(pid_text, L"%lu", pid);
  return JoinPath(TempRoot(), pid_text);
}

bool EnsureDirectoryTree(const std::wstring& path) {
  // Python の os.makedirs(path, exist_ok=True) に近い helper です。
  if (path.empty()) {
    return false;
  }
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    return std::filesystem::is_directory(path, ec);
  }
  return std::filesystem::create_directories(path, ec) || std::filesystem::exists(path, ec);
}

bool RemoveDirectoryTree(const std::wstring& path) {
  // std::filesystem::remove_all は存在しない path でも error にしないため、cleanup 向きです。
  if (path.empty()) {
    return false;
  }
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
  return !ec;
}

void CleanupStaleTempRoots(unsigned long keep_pid) {
  // keep_pid 以外の pid ディレクトリを削除します。
  // keep_pid=0 のときは全削除です。launcher 起動時の掃除に使います。
  std::error_code ec;
  auto root = TempRoot();
  if (!std::filesystem::exists(root, ec)) {
    return;
  }
  for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_directory(ec)) {
      continue;
    }
    auto name = entry.path().filename().wstring();
    wchar_t* end = nullptr;
    unsigned long pid = wcstoul(name.c_str(), &end, 10);
    if (end != name.c_str() && *end == L'\0' && pid == keep_pid) {
      continue;
    }
    RemoveDirectoryTree(entry.path().wstring());
  }
}

std::wstring GetFullPath(const std::wstring& path) {
  // editor から相対 path が来ることがあるため、Windows API と同じ基準で絶対 path 化します。
  DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (!needed) {
    return path;
  }
  std::vector<wchar_t> buffer(needed + 1);
  DWORD written = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
  if (!written) {
    return path;
  }
  return std::wstring(buffer.data(), written);
}

std::wstring ToWideAnsiPath(const char* value) {
  if (!value) {
    return L"";
  }
  int needed = MultiByteToWideChar(CP_ACP, 0, value, -1, nullptr, 0);
  if (needed <= 0) {
    return L"";
  }
  std::wstring result(static_cast<size_t>(needed - 1), L'\0');
  MultiByteToWideChar(CP_ACP, 0, value, -1, result.data(), needed);
  return result;
}

std::string ToAnsiPath(const std::wstring& value) {
  int needed = WideCharToMultiByte(CP_ACP, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (needed <= 0) {
    return {};
  }
  std::string result(static_cast<size_t>(needed - 1), '\0');
  WideCharToMultiByte(CP_ACP, 0, value.c_str(), -1, result.data(), needed, nullptr, nullptr);
  return result;
}

bool FileExistsNoHook(const std::wstring& path) {
  DWORD attrs = GetFileAttributesW(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

}  // namespace roh
