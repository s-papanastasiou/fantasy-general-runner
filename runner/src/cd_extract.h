// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>

namespace fg {

// Extract the ISO 9660 data track of a raw CD image (2352-byte sectors) to
// out_dir, once: a marker file records a completed extraction.
bool extract_cd_data_track(const std::string& image_path, const std::string& out_dir);

}  // namespace fg
