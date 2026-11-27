// SPDX-License-Identifier: GPL-2.0-or-later
#include "log.h"

#include <SDL3/SDL.h>

#include <cstdarg>
#include <filesystem>
#include <mutex>

namespace fg {

namespace {

std::mutex g_log_mu;
FILE* g_log_file = nullptr;
std::string g_log_path;
std::string g_error;

}  // namespace

void log_open(const std::string& path) {
    std::lock_guard lock(g_log_mu);
#ifdef _WIN32
    g_log_file = _wfopen(std::filesystem::path(path).c_str(), L"w");
#else
    g_log_file = std::fopen(path.c_str(), "w");
#endif
    if (g_log_file) g_log_path = path;
}

const std::string& log_path() { return g_log_path; }

void log_line(const char* prefix, const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard lock(g_log_mu);  // whole lines from several threads
    std::fprintf(stderr, "%s%s\n", prefix, buf);
    if (g_log_file) {
        std::fprintf(g_log_file, "%s%s\n", prefix, buf);
        std::fflush(g_log_file);  // complete even if the process dies
    }
}

void set_error(const std::string& text) {
    std::lock_guard lock(g_log_mu);
    if (g_error.empty()) g_error = text;  // the first error is the cause
}

void report_errors(bool quiet) {
    std::string text;
    {
        std::lock_guard lock(g_log_mu);
        text = g_error;
    }
    if (text.empty() || quiet) return;
    if (!g_log_path.empty()) text += "\n\nDetails are in the log file:\n" + g_log_path;
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Fantasy General", text.c_str(), nullptr);
}

}  // namespace fg
