// SPDX-License-Identifier: GPL-2.0-or-later
// Minimal PNG writer (RGB, uncompressed deflate) for screenshots.
#pragma once
#include <string>

namespace fg {

struct Frame;
bool write_png(const std::string& path, const Frame& f);

}  // namespace fg
