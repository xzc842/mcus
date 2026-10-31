#include "mcus/log.h"
#include <mutex>
#include <fstream>
#include <memory>
#include <cstdarg>

namespace mcus {

namespace {
std::mutex g_log_mutex;
LogLevel g_level = LogLevel::Info;
std::unique_ptr<std::ofstream> g_file;
FILE* g_out = stderr;

const char* level_name(LogLevel l) {
    switch (l) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        default: return "OFF";
    }
}
} // namespace

void log_set_level(LogLevel level) { g_level = level; }
LogLevel log_get_level() { return g_level; }

void log_set_output(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_log_mutex);
    if (path == "stderr" || path.empty()) {
        g_file.reset();
        g_out = stderr;
        return;
    }
    g_file = std::make_unique<std::ofstream>(path, std::ios::app);
    if (!g_file->is_open()) {
        g_file.reset();
        g_out = stderr;
        return;
    }
    g_out = nullptr; // 用 ofstream 时走 g_file
}

void log_write(LogLevel level, const char* file, int line,
               const std::string& msg) {
    if (static_cast<int>(level) < static_cast<int>(g_level) ||
        g_level == LogLevel::Off) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_log_mutex);
    if (g_file) {
        *g_file << "[" << level_name(level) << "] " << file << ":" << line
                << " " << msg << "\n";
        g_file->flush();
    } else if (g_out) {
        std::fprintf(g_out, "[%s] %s:%d %s\n", level_name(level), file, line,
                     msg.c_str());
        std::fflush(g_out);
    }
}

} // namespace mcus