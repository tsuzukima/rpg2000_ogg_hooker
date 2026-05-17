#pragma once

#include <string>

namespace roh {

// Small shared logger for the launcher and the injected DLL.
//
// Normal startup does not initialize this logger. The launcher enables it only
// for diagnostic command-line options, and the DLL follows the diagnostic
// environment flag passed by the launcher.
void InitLog(const std::wstring& log_path);
void Log(const std::wstring& message);
void LogLastError(const std::wstring& prefix);

// Convert a Windows GetLastError() value into readable text.
std::wstring LastErrorMessage(unsigned long error_code);

}  // namespace roh
