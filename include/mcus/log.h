#pragma once
#include <string>
#include <cstdio>

namespace mcus {

enum class LogLevel : int {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Off = 5,
};

void log_set_level(LogLevel level);
LogLevel log_get_level();
void log_set_output(const std::string& path); // "stderr" 或文件路径
void log_write(LogLevel level, const char* file, int line,
               const std::string& msg);

} // namespace mcus

#define MCUS_LOG(level, msg) \
    ::mcus::log_write(level, __FILE__, __LINE__, (msg))

#define MCUS_TRACE(msg) MCUS_LOG(::mcus::LogLevel::Trace, msg)
#define MCUS_DEBUG(msg) MCUS_LOG(::mcus::LogLevel::Debug, msg)
#define MCUS_INFO(msg)  MCUS_LOG(::mcus::LogLevel::Info,  msg)
#define MCUS_WARN(msg)  MCUS_LOG(::mcus::LogLevel::Warn,  msg)
#define MCUS_ERROR(msg) MCUS_LOG(::mcus::LogLevel::Error, msg)