#include <windows.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "hooker/log.h"
#include "hooker/paths.h"

namespace {

bool g_diagnostics_enabled = false;

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

bool IsDiagnosticArg(const std::wstring& arg) {
  return arg == L"--diagnostic" || arg == L"--diag" || arg == L"/diagnostic" || arg == L"/diag";
}

void LogIfDiagnostic(const std::wstring& message) {
  if (g_diagnostics_enabled) {
    roh::Log(message);
  }
}

bool InjectDll(HANDLE process, const std::wstring& dll_path) {
  // Load the DLL in this process first so we can find the exported initializer.
  HMODULE local_hook = LoadLibraryW(dll_path.c_str());
  if (!local_hook) {
    if (g_diagnostics_enabled) {
      roh::LogLastError(L"LoadLibraryW(local rpg2k_ogg_hook.dll) failed");
    }
    return false;
  }

  FARPROC local_init = GetProcAddress(local_hook, "RohHookInitialize");
  if (!local_init) {
    local_init = GetProcAddress(local_hook, "_RohHookInitialize@4");
  }
  if (!local_init) {
    if (g_diagnostics_enabled) {
      roh::LogLastError(L"GetProcAddress(RohHookInitialize) failed");
    }
    FreeLibrary(local_hook);
    return false;
  }

  // Copy the DLL path into the suspended RPG2000.exe process.
  const size_t byte_count = (dll_path.size() + 1) * sizeof(wchar_t);
  void* remote = VirtualAllocEx(process, nullptr, byte_count, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!remote) {
    if (g_diagnostics_enabled) {
      roh::LogLastError(L"VirtualAllocEx failed");
    }
    FreeLibrary(local_hook);
    return false;
  }

  SIZE_T written = 0;
  if (!WriteProcessMemory(process, remote, dll_path.c_str(), byte_count, &written) ||
      written != byte_count) {
    if (g_diagnostics_enabled) {
      roh::LogLastError(L"WriteProcessMemory failed");
    }
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    FreeLibrary(local_hook);
    return false;
  }

  HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  auto load_library =
      reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "LoadLibraryW"));
  if (!load_library) {
    if (g_diagnostics_enabled) {
      roh::LogLastError(L"GetProcAddress(LoadLibraryW) failed");
    }
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    FreeLibrary(local_hook);
    return false;
  }

  // Run LoadLibraryW inside RPG2000.exe. The thread exit code is the remote module base.
  HANDLE thread = CreateRemoteThread(process, nullptr, 0, load_library, remote, 0, nullptr);
  if (!thread) {
    if (g_diagnostics_enabled) {
      roh::LogLastError(L"CreateRemoteThread failed");
    }
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    FreeLibrary(local_hook);
    return false;
  }

  WaitForSingleObject(thread, INFINITE);
  DWORD exit_code = 0;
  GetExitCodeThread(thread, &exit_code);
  CloseHandle(thread);
  VirtualFreeEx(process, remote, 0, MEM_RELEASE);

  if (exit_code == 0) {
    LogIfDiagnostic(L"LoadLibraryW returned NULL for rpg2k_ogg_hook.dll");
    FreeLibrary(local_hook);
    return false;
  }

  // The DLL may be mapped at a different base in the remote process. Use the
  // local export offset to compute the remote RohHookInitialize address.
  auto remote_module = reinterpret_cast<unsigned char*>(static_cast<uintptr_t>(exit_code));
  auto local_module = reinterpret_cast<unsigned char*>(local_hook);
  auto init_offset = reinterpret_cast<unsigned char*>(local_init) - local_module;
  auto remote_init = reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_module + init_offset);

  HANDLE init_thread = CreateRemoteThread(process, nullptr, 0, remote_init, nullptr, 0, nullptr);
  if (!init_thread) {
    if (g_diagnostics_enabled) {
      roh::LogLastError(L"CreateRemoteThread(RohHookInitialize) failed");
    }
    FreeLibrary(local_hook);
    return false;
  }

  WaitForSingleObject(init_thread, INFINITE);
  DWORD init_exit = 1;
  GetExitCodeThread(init_thread, &init_exit);
  CloseHandle(init_thread);
  FreeLibrary(local_hook);

  if (init_exit != 0) {
    LogIfDiagnostic(L"RohHookInitialize returned " + std::to_wstring(init_exit));
    return false;
  }
  return true;
}

int Fail(const std::wstring& message, int code) {
  LogIfDiagnostic(message);
  std::wcerr << message << L"\n";
  return code;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  const std::wstring base_dir = roh::ExecutableDirectory();
  if (base_dir.empty()) {
    std::wcerr << L"Could not resolve RPG2000_ogg_hooker.exe directory\n";
    return 1;
  }

  std::vector<std::wstring> forward_args;
  for (int i = 1; i < argc; ++i) {
    std::wstring arg = argv[i] ? argv[i] : L"";
    if (IsDiagnosticArg(arg)) {
      g_diagnostics_enabled = true;
      continue;
    }
    forward_args.push_back(arg);
  }

  if (g_diagnostics_enabled) {
    SetEnvironmentVariableW(L"RPG2000_OGG_HOOKER_DIAGNOSTIC", L"1");
  } else {
    SetEnvironmentVariableW(L"RPG2000_OGG_HOOKER_DIAGNOSTIC", nullptr);
  }

  const std::wstring preferred_log_path = roh::JoinPath(base_dir, L"rpg2000_ogg_hooker.log");
  if (g_diagnostics_enabled) {
    roh::InitLog(preferred_log_path);
    roh::Log(L"launcher start base_dir=\"" + base_dir + L"\"");

    WIN32_FILE_ATTRIBUTE_DATA log_probe{};
    const bool preferred_written =
        GetFileAttributesExW(preferred_log_path.c_str(), GetFileExInfoStandard, &log_probe) &&
        ((static_cast<unsigned long long>(log_probe.nFileSizeHigh) << 32) | log_probe.nFileSizeLow) > 0;
    if (!preferred_written) {
      std::vector<wchar_t> temp_buf(MAX_PATH);
      DWORD temp_len = GetTempPathW(static_cast<DWORD>(temp_buf.size()), temp_buf.data());
      while (temp_len >= temp_buf.size()) {
        temp_buf.resize(temp_len + 1);
        temp_len = GetTempPathW(static_cast<DWORD>(temp_buf.size()), temp_buf.data());
      }

      std::wstring fallback;
      if (temp_len > 0) {
        fallback = std::wstring(temp_buf.data(), temp_len);
        if (!fallback.empty() && fallback.back() != L'\\' && fallback.back() != L'/') {
          fallback += L'\\';
        }
        fallback += L"rpg2000_ogg_hooker.log";
      }

      const std::wstring message =
          L"rpg2000_ogg_hooker could not write the diagnostic log next to the launcher.\n\n"
          L"Launcher folder:\n" +
          base_dir + L"\n\nFallback log path:\n" + fallback +
          L"\n\nThis dialog is shown only when started with --diagnostic.";
      MessageBoxW(nullptr, message.c_str(), L"rpg2000_ogg_hooker", MB_OK | MB_ICONINFORMATION);
    }
  }

  // A previous editor crash can leave temp WAVs behind. Remove old session
  // directories before starting a new editor process.
  roh::CleanupStaleTempRoots(0);

  const std::wstring rpg2000_path = roh::JoinPath(base_dir, L"RPG2000.exe");
  const std::wstring hook_path = roh::JoinPath(base_dir, L"rpg2k_ogg_hook.dll");

  if (!std::filesystem::exists(rpg2000_path)) {
    return Fail(L"RPG2000.exe was not found next to RPG2000_ogg_hooker.exe", 2);
  }
  if (!std::filesystem::exists(hook_path)) {
    return Fail(L"rpg2k_ogg_hook.dll was not found next to RPG2000_ogg_hooker.exe", 3);
  }

  std::wstring command_line = QuoteArg(rpg2000_path);
  if (!forward_args.empty()) {
    for (const auto& arg : forward_args) {
      command_line += L" ";
      command_line += QuoteArg(arg);
    }
  }

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> mutable_cmd(command_line.begin(), command_line.end());
  mutable_cmd.push_back(L'\0');

  // Start RPG2000.exe suspended so the file hooks are installed before Delphi
  // startup code can query Music/Sound files.
  if (!CreateProcessW(rpg2000_path.c_str(), mutable_cmd.data(), nullptr, nullptr, FALSE,
                      CREATE_SUSPENDED, nullptr, base_dir.c_str(), &si, &pi)) {
    if (g_diagnostics_enabled) {
      roh::LogLastError(L"CreateProcessW(RPG2000.exe) failed");
    }
    return Fail(L"Could not start RPG2000.exe", 6);
  }

  LogIfDiagnostic(L"created RPG2000.exe pid=" + std::to_wstring(pi.dwProcessId));
  if (!InjectDll(pi.hProcess, hook_path)) {
    TerminateProcess(pi.hProcess, 10);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return Fail(L"rpg2k_ogg_hook.dll injection failed; RPG2000.exe was not resumed", 7);
  }

  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  WaitForSingleObject(pi.hProcess, INFINITE);

  DWORD exit_code = 0;
  GetExitCodeProcess(pi.hProcess, &exit_code);
  CloseHandle(pi.hProcess);

  roh::RemoveDirectoryTree(roh::SessionTempRoot(pi.dwProcessId));
  LogIfDiagnostic(L"RPG2000.exe exited code=" + std::to_wstring(exit_code));
  return static_cast<int>(exit_code);
}
