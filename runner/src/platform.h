// SPDX-License-Identifier: GPL-2.0-or-later
// Small differences between POSIX and Windows (MinGW).
#pragma once
#include <ctime>

namespace fg {

inline void local_time(std::time_t t, std::tm& out) {
#ifdef _WIN32
    localtime_s(&out, &t);
#else
    localtime_r(&t, &out);
#endif
}

}  // namespace fg
