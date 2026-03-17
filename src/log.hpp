#pragma once
#include <string>

namespace logger {

/// Log levels (matching Go version)
enum class LogLevel {
    Debug = 0,
    Info  = 1,
    Warning = 2,
    Error = 3,
    Fatal = 4,
    Success = 5,
};

/// Initialize logging. output = "shell" → stdout with color;
/// any other value is treated as a file path.
bool init(const std::string& output);

/// Set minimum log level to display
void set_level(LogLevel level);

/// Get current log level
LogLevel get_level();

void debug  (const char* fmt, ...);
void info   (const char* fmt, ...);
void error  (const char* fmt, ...);
void success(const char* fmt, ...);
void warning(const char* fmt, ...);
void fatal  (const char* fmt, ...); ///< logs then exit(1)

} // namespace logger
