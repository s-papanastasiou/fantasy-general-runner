// SPDX-License-Identifier: GPL-2.0-or-later
// Extract the data track of the GOG CD image (game.gog, raw MODE2/2352,
// ISO 9660) to a directory, which then serves as drive D:. Same as
// tools/extract_cd.py, for players without Python (Windows).
#include "cd_extract.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "log.h"

namespace fg {

namespace {

namespace fs = std::filesystem;
constexpr uint32_t kSector = 2352, kUserOff = 24;  // MODE2 form 1: 12 sync + 4 header + 8 subheader

class Image {
public:
    explicit Image(const fs::path& p) : f_(p, std::ios::binary) {}
    bool ok() const { return bool(f_); }
    std::vector<uint8_t> sectors(uint32_t lba, uint32_t count) {
        std::vector<uint8_t> out(size_t(count) * 2048);
        for (uint32_t i = 0; i < count; ++i) {
            f_.seekg(std::streamoff(lba + i) * kSector + kUserOff);
            f_.read(reinterpret_cast<char*>(out.data() + size_t(i) * 2048), 2048);
            if (!f_) return {};
        }
        return out;
    }

private:
    std::ifstream f_;
};

uint32_t u32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

bool walk(Image& img, uint32_t lba, uint32_t size, const fs::path& dir, int depth, int& count) {
    if (depth > 16) return false;
    std::vector<uint8_t> data = img.sectors(lba, (size + 2047) / 2048);
    if (data.empty()) return false;
    for (size_t i = 0; i < data.size();) {
        uint8_t n = data[i];
        if (n == 0) { i = (i / 2048 + 1) * 2048; continue; }  // records do not cross sectors
        if (i + 33 > data.size() || i + 33 + data[i + 32] > data.size()) return false;
        uint32_t elba = u32(&data[i + 2]), esize = u32(&data[i + 10]);
        uint8_t flags = data[i + 25];
        std::string name(reinterpret_cast<const char*>(&data[i + 33]), data[i + 32]);
        if (name != std::string(1, '\0') && name != std::string(1, '\1')) {
            name = name.substr(0, name.find(';'));
            if (name.empty() || name.find_first_of("/\\:") != std::string::npos || name == "." || name == "..")
                return false;  // not a plain file name
            fs::path dest = dir / name;
            if (flags & 2) {
                std::error_code ec;
                fs::create_directories(dest, ec);
                if (ec || !walk(img, elba, esize, dest, depth + 1, count)) return false;
            } else {
                std::ofstream out(dest, std::ios::binary | std::ios::trunc);
                for (uint32_t remaining = esize, cur = elba; remaining > 0; cur += 64) {
                    uint32_t want = std::min<uint32_t>(64, (remaining + 2047) / 2048);
                    std::vector<uint8_t> chunk = img.sectors(cur, want);
                    if (chunk.empty()) return false;
                    uint32_t len = std::min<uint32_t>(remaining, uint32_t(chunk.size()));
                    out.write(reinterpret_cast<const char*>(chunk.data()), len);
                    remaining -= len;
                }
                if (!out) return false;
                ++count;
            }
        }
        i += n;
    }
    return true;
}

}  // namespace

bool extract_cd_data_track(const std::string& image_path, const std::string& out_dir) {
    fs::path out = fs::path(out_dir);
    fs::path done = out / ".extracted";
    if (fs::exists(done)) return true;
    Image img{fs::path(image_path)};
    if (!img.ok()) return false;
    std::vector<uint8_t> pvd = img.sectors(16, 1);
    if (pvd.size() < 2048 || std::memcmp(&pvd[1], "CD001", 5) != 0) {
        FG_LOG("%s: not an ISO 9660 data track", image_path.c_str());
        return false;
    }
    FG_LOG("extracting the CD data track of %s to %s (first run only)", image_path.c_str(), out_dir.c_str());
    std::error_code ec;
    fs::create_directories(out, ec);
    int count = 0;
    const uint8_t* root = &pvd[156];
    if (ec || !walk(img, u32(root + 2), u32(root + 10), out, 0, count)) {
        FG_LOG("CD extraction failed");
        return false;
    }
    std::ofstream(done) << count << " files\n";
    FG_LOG("extracted %d files", count);
    return true;
}

}  // namespace fg
