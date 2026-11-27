// SPDX-License-Identifier: GPL-2.0-or-later
// Logging. FG_LOG lines go to stderr and to the log file (log_open);
// FG_TRACE (FG_TRACE=1) only to stderr, since it can be very large.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>

namespace fg {

extern bool g_trace;

// Start writing log lines to `path` as well (truncated each run).
void log_open(const std::string& path);
const std::string& log_path();  // "" if no log file
void log_line(const char* prefix, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// An error the player must see: logged now, and kept so the main thread can
// show it in a message box once the window is gone (report_errors).
void set_error(const std::string& text);
// Shows the kept error, if any, unless `quiet` (headless). Main thread only.
void report_errors(bool quiet);

#define FG_LOG(...) ::fg::log_line("[fg] ", __VA_ARGS__)
#define FG_TRACE(...) do { if (::fg::g_trace) { std::fprintf(stderr, "[trace] " __VA_ARGS__); std::fputc('\n', stderr); } } while (0)

}  // namespace fg
