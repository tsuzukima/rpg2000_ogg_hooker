#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "hooker/log.h"
#include "hooker/mf_ogg_decoder.h"
#include "hooker/paths.h"

namespace {

// rpg2k_ogg_hook.dll の本体です。
// RPG2000.exe は Delphi 5 製の 32-bit ANSI API アプリです。disasm で確認した
// IAT slot だけを直接差し替え、OGG を .wav 名として editor に見せます。
//
// 読む順番:
//   1. 型定義と global state
//   2. hook_audio.inc      OGG overlay と一時 WAV 作成
//   3. HookXxxxA 関数      RPG2000.exe から呼ばれる ANSI API hook
//   4. PatchKnownRpg2000Slots()
//   5. RohHookInitialize

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using GetFileAttributesWFn = DWORD(WINAPI*)(LPCWSTR);
using GetFileAttributesAFn = DWORD(WINAPI*)(LPCSTR);
using FindFirstFileWFn = HANDLE(WINAPI*)(LPCWSTR, LPWIN32_FIND_DATAW);
using FindFirstFileAFn = HANDLE(WINAPI*)(LPCSTR, LPWIN32_FIND_DATAA);
using FindNextFileWFn = BOOL(WINAPI*)(HANDLE, LPWIN32_FIND_DATAW);
using FindNextFileAFn = BOOL(WINAPI*)(HANDLE, LPWIN32_FIND_DATAA);
using FindCloseFn = BOOL(WINAPI*)(HANDLE);

CreateFileWFn g_real_create_file_w = nullptr;
CreateFileAFn g_real_create_file_a = nullptr;
GetFileAttributesWFn g_real_get_file_attributes_w = nullptr;
GetFileAttributesAFn g_real_get_file_attributes_a = nullptr;
FindFirstFileWFn g_real_find_first_file_w = nullptr;
FindFirstFileAFn g_real_find_first_file_a = nullptr;
FindNextFileWFn g_real_find_next_file_w = nullptr;
FindNextFileAFn g_real_find_next_file_a = nullptr;
FindCloseFn g_real_find_close = nullptr;

std::wstring g_session_root;
unsigned long g_process_id = 0;

std::mutex g_decode_map_mutex;
std::map<std::wstring, std::shared_ptr<std::mutex>> g_decode_locks;
std::once_flag g_init_once;
std::mutex g_audio_trace_mutex;
int g_audio_trace_count = 0;

constexpr unsigned int kFakeFindMagic = 0x524F4648;  // ROFH

struct FakeFindHandle {
  unsigned int magic = kFakeFindMagic;
  size_t index = 0;
  std::vector<WIN32_FIND_DATAW> results;
};

std::wstring Lower(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
  return value;
}

bool DiagnosticsEnabled() {
  wchar_t value[8] = {};
  DWORD length = GetEnvironmentVariableW(L"RPG2000_OGG_HOOKER_DIAGNOSTIC", value, 8);
  return length > 0 && wcscmp(value, L"1") == 0;
}

void LogIfDiagnostic(const std::wstring& message) {
  if (DiagnosticsEnabled()) {
    roh::Log(message);
  }
}

#include "hooker/hook_audio.inc"

HANDLE MakeFakeFindHandle(std::vector<WIN32_FIND_DATAW>&& results, LPWIN32_FIND_DATAW first) {
  if (results.empty()) {
    SetLastError(ERROR_FILE_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
  }
  auto* handle = new FakeFindHandle();
  handle->results = std::move(results);
  handle->index = 0;
  if (first) {
    *first = handle->results[0];
  }
  return reinterpret_cast<HANDLE>(handle);
}

FakeFindHandle* AsFakeFind(HANDLE handle) {
  auto* fake = reinterpret_cast<FakeFindHandle*>(handle);
  __try {
    if (fake && fake->magic == kFakeFindMagic) {
      return fake;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return nullptr;
}

extern "C" HANDLE WINAPI HookCreateFileA(LPCSTR file_name, DWORD desired_access, DWORD share_mode,
                                          LPSECURITY_ATTRIBUTES security_attributes,
                                          DWORD creation_disposition, DWORD flags_and_attributes,
                                          HANDLE template_file) {
  if (file_name && creation_disposition == OPEN_EXISTING) {
    TraceAudioPath(L"CreateFileA", roh::ToWideAnsiPath(file_name),
                   L"disposition=" + std::to_wstring(creation_disposition));
    std::wstring temp;
    if (TryRedirectAudioRequest(roh::ToWideAnsiPath(file_name), &temp)) {
      return g_real_create_file_w(temp.c_str(), desired_access, share_mode, security_attributes,
                                  creation_disposition, flags_and_attributes, template_file);
    }
  }
  return g_real_create_file_a(file_name, desired_access, share_mode, security_attributes,
                              creation_disposition, flags_and_attributes, template_file);
}

extern "C" DWORD WINAPI HookGetFileAttributesA(LPCSTR file_name) {
  DWORD attrs = g_real_get_file_attributes_a(file_name);
  if (file_name) {
    TraceAudioPath(L"GetFileAttributesA", roh::ToWideAnsiPath(file_name),
                   L"attrs=" + std::to_wstring(attrs) + L" error=" +
                       std::to_wstring(attrs == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0));
  }
  if (attrs != INVALID_FILE_ATTRIBUTES || !file_name) {
    return attrs;
  }

  std::wstring request = roh::ToWideAnsiPath(file_name);
  std::wstring full = roh::GetFullPath(request);
  std::wstring resolved_request;
  if (!OggPathForAudioRequestWithFallback(request, full, &resolved_request).empty()) {
    return FILE_ATTRIBUTE_ARCHIVE;
  }
  return INVALID_FILE_ATTRIBUTES;
}

extern "C" HANDLE WINAPI HookFindFirstFileA(LPCSTR file_name, LPWIN32_FIND_DATAA find_data) {
  if (file_name) {
    TraceAudioPath(L"FindFirstFileA", roh::ToWideAnsiPath(file_name), L"");
    std::vector<WIN32_FIND_DATAW> results;
    if (BuildFindResults(roh::ToWideAnsiPath(file_name), &results)) {
      WIN32_FIND_DATAW first{};
      HANDLE handle = MakeFakeFindHandle(std::move(results), &first);
      if (handle != INVALID_HANDLE_VALUE) {
        CopyFindDataWToA(first, find_data);
      }
      return handle;
    }
  }
  return g_real_find_first_file_a(file_name, find_data);
}

extern "C" BOOL WINAPI HookFindNextFileA(HANDLE handle, LPWIN32_FIND_DATAA find_data) {
  if (auto* fake = AsFakeFind(handle)) {
    if (fake->index + 1 >= fake->results.size()) {
      SetLastError(ERROR_NO_MORE_FILES);
      return FALSE;
    }
    ++fake->index;
    CopyFindDataWToA(fake->results[fake->index], find_data);
    return TRUE;
  }
  return g_real_find_next_file_a(handle, find_data);
}

extern "C" BOOL WINAPI HookFindClose(HANDLE handle) {
  if (auto* fake = AsFakeFind(handle)) {
    fake->magic = 0;
    delete fake;
    return TRUE;
  }
  return g_real_find_close(handle);
}

void ResolveRealFunctions() {
  HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  g_real_create_file_w =
      reinterpret_cast<CreateFileWFn>(GetProcAddress(kernel32, "CreateFileW"));
  g_real_create_file_a =
      reinterpret_cast<CreateFileAFn>(GetProcAddress(kernel32, "CreateFileA"));
  g_real_get_file_attributes_w =
      reinterpret_cast<GetFileAttributesWFn>(GetProcAddress(kernel32, "GetFileAttributesW"));
  g_real_get_file_attributes_a =
      reinterpret_cast<GetFileAttributesAFn>(GetProcAddress(kernel32, "GetFileAttributesA"));
  g_real_find_first_file_w =
      reinterpret_cast<FindFirstFileWFn>(GetProcAddress(kernel32, "FindFirstFileW"));
  g_real_find_first_file_a =
      reinterpret_cast<FindFirstFileAFn>(GetProcAddress(kernel32, "FindFirstFileA"));
  g_real_find_next_file_w =
      reinterpret_cast<FindNextFileWFn>(GetProcAddress(kernel32, "FindNextFileW"));
  g_real_find_next_file_a =
      reinterpret_cast<FindNextFileAFn>(GetProcAddress(kernel32, "FindNextFileA"));
  g_real_find_close = reinterpret_cast<FindCloseFn>(GetProcAddress(kernel32, "FindClose"));
}

struct KnownPatchSlot {
  const wchar_t* label;
  DWORD rva;
  void* replacement;
  void** original;
};

bool PatchSlot(DWORD rva, void* replacement, void** original) {
  HMODULE module = GetModuleHandleW(nullptr);
  if (!module || !replacement) {
    return false;
  }
  auto* base = reinterpret_cast<unsigned char*>(module);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    return false;
  }
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE || rva >= nt->OptionalHeader.SizeOfImage) {
    return false;
  }

  void** slot = reinterpret_cast<void**>(base + rva);
  DWORD old_protect = 0;
  if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect)) {
    return false;
  }
  if (original && !*original) {
    *original = *slot;
  }
  *slot = replacement;
  VirtualProtect(slot, sizeof(void*), old_protect, &old_protect);
  return true;
}

void PatchKnownRpg2000Slots() {
  // RVA は `rpg_rt/_disasm/RPG2000/analysis/imports.csv` で確認した値です。
  // この build の RPG2000.exe は OriginalFirstThunk を持たないため、関数名での
  // IAT walk ではなく、既知 slot を直接 patch します。
  const KnownPatchSlot slots[] = {
      {L"FindFirstFileA slot A", 0x001481E8, reinterpret_cast<void*>(HookFindFirstFileA),
       reinterpret_cast<void**>(&g_real_find_first_file_a)},
      {L"FindFirstFileA slot B", 0x001483B8, reinterpret_cast<void*>(HookFindFirstFileA),
       reinterpret_cast<void**>(&g_real_find_first_file_a)},
      {L"FindNextFileA slot B", 0x001483B4, reinterpret_cast<void*>(HookFindNextFileA),
       reinterpret_cast<void**>(&g_real_find_next_file_a)},
      {L"FindClose slot A", 0x001481EC, reinterpret_cast<void*>(HookFindClose),
       reinterpret_cast<void**>(&g_real_find_close)},
      {L"FindClose slot B", 0x001483BC, reinterpret_cast<void*>(HookFindClose),
       reinterpret_cast<void**>(&g_real_find_close)},
      {L"GetFileAttributesA slot B", 0x00148384, reinterpret_cast<void*>(HookGetFileAttributesA),
       reinterpret_cast<void**>(&g_real_get_file_attributes_a)},
      {L"CreateFileA slot A", 0x0014821C, reinterpret_cast<void*>(HookCreateFileA),
       reinterpret_cast<void**>(&g_real_create_file_a)},
      {L"CreateFileA slot B", 0x001483E4, reinterpret_cast<void*>(HookCreateFileA),
       reinterpret_cast<void**>(&g_real_create_file_a)},
  };

  int patched = 0;
  for (const auto& slot : slots) {
    if (PatchSlot(slot.rva, slot.replacement, slot.original)) {
      ++patched;
    } else {
      LogIfDiagnostic(L"roh_hook direct_patch_failed label=\"" + std::wstring(slot.label) + L"\"");
    }
  }
  LogIfDiagnostic(L"roh_hook direct_patch_done count=" + std::to_wstring(patched));
}

void InitializeHookOnce() {
  if (DiagnosticsEnabled()) {
    roh::InitLog(roh::JoinPath(roh::ExecutableDirectory(), L"rpg2000_ogg_hooker.log"));
  }
  ResolveRealFunctions();
  g_process_id = GetCurrentProcessId();
  g_session_root = roh::SessionTempRoot(g_process_id);
  roh::EnsureDirectoryTree(g_session_root);
  PatchKnownRpg2000Slots();
  LogIfDiagnostic(L"roh_hook installed session_temp=\"" + g_session_root + L"\"");
}

extern "C" __declspec(dllexport) DWORD WINAPI RohHookInitialize(void*) {
  std::call_once(g_init_once, InitializeHookOnce);
  return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}
