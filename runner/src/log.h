// SPDX-License-Identifier: GPL-2.0-or-later
// Minimal logging. Output goes to stderr; FG_TRACE=1 enables trace().
#pragma once
#include <cstdio>
#include <cstdlib>

namespace fg {

extern bool g_trace;

#define FG_LOG(...) do { std::fprintf(stderr, "[fg] " __VA_ARGS__); std::fputc('\n', stderr); } while (0)
#define FG_TRACE(...) do { if (::fg::g_trace) { std::fprintf(stderr, "[trace] " __VA_ARGS__); std::fputc('\n', stderr); } } while (0)

}  // namespace fg
