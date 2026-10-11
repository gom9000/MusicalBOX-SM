#include "core/Log.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace mbx::log {
namespace {

std::mutex g_mutex;

void write(const char* level, const char* fmt, va_list args) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::fprintf(stderr, "[%s] ", level);
    std::vfprintf(stderr, fmt, args);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

} // namespace

void info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    write("info", fmt, args);
    va_end(args);
}

void warn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    write("warn", fmt, args);
    va_end(args);
}

void error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    write("error", fmt, args);
    va_end(args);
}

} // namespace mbx::log
