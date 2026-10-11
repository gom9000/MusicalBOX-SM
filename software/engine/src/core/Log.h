#pragma once

// Minimal logging to stderr. Thread safe, but it locks and does I/O:
// never call it from the audio thread.

namespace mbx::log {

void info(const char* fmt, ...);
void warn(const char* fmt, ...);
void error(const char* fmt, ...);

} // namespace mbx::log
