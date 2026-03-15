#pragma once
#include <string>

namespace logger {

/// Initialize logging. output = "shell" → stdout with color;
/// any other value is treated as a file path.
bool init(const std::string& output);

void info   (const char* fmt, ...);
void error  (const char* fmt, ...);
void success(const char* fmt, ...);
void warning(const char* fmt, ...);
void fatal  (const char* fmt, ...); ///< logs then exit(1)

} // namespace logger
