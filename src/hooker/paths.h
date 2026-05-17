#pragma once

#include <string>

namespace roh {

// path と文字 code 変換の公開 API です。
// hook_dll.cpp からも launcher.cpp からも使うため、hook 中に再帰しない関数は
// NoHook suffix を付けて区別しています。
//
// RPG2000.exe は古い Win32 API と ANSI path を使うため、Windows API との変換も
// ここに集めています。
std::wstring ExecutablePath();
std::wstring ExecutableDirectory();
std::wstring JoinPath(const std::wstring& lhs, const std::wstring& rhs);

// 一時 WAV cache の root (%TEMP%\rpg2000_ogg_hooker)。
// editor 側は単一の game folder を持たない (project は file menu から開く) ため、
// rpg_rt_container 流の game-local cache 経路は採用せず、session_temp 一本にします。
std::wstring TempRoot();
std::wstring SessionTempRoot(unsigned long pid);

bool EnsureDirectoryTree(const std::wstring& path);
bool RemoveDirectoryTree(const std::wstring& path);

// keep_pid 以外の session directory を削除します。
// keep_pid=0 のときは全削除。launcher の cold start 時の掃除に使います。
void CleanupStaleTempRoots(unsigned long keep_pid);

std::wstring GetFullPath(const std::wstring& path);

// RPG2000.exe の CreateFileA などで来る ANSI/CP932 文字列を wide 文字列にします。
std::wstring ToWideAnsiPath(const char* value);

// wide 文字列を ANSI/CP932 に戻します。redirect 後の temp path を ANSI で返す時に使います。
std::string ToAnsiPath(const std::wstring& value);

// hook 中から存在確認したい時用です。ここでは hook された API を避け、素の Windows API を使います。
bool FileExistsNoHook(const std::wstring& path);

}  // namespace roh
