// SPDX-License-Identifier: GPL-2.0-or-later
#include "png.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "video.h"

namespace fg {

namespace {

uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t v = i;
            for (int k = 0; k < 8; ++k) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            table[i] = v;
        }
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back(static_cast<uint8_t>(x >> s));
}

void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    put32(out, static_cast<uint32_t>(data.size()));
    std::vector<uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    put32(out, crc32(body.data(), body.size()) ^ 0xFFFFFFFFu);
}

}  // namespace

bool write_png(const std::string& path, const Frame& f) {
    std::vector<uint8_t> raw;
    raw.reserve(size_t(f.height) * (1 + 3 * f.width));
    for (int y = 0; y < f.height; ++y) {
        raw.push_back(0);  // filter: none
        for (int x = 0; x < f.width; ++x) {
            uint32_t p = f.pixels[size_t(y) * f.width + x];
            raw.push_back(static_cast<uint8_t>(p >> 16));
            raw.push_back(static_cast<uint8_t>(p >> 8));
            raw.push_back(static_cast<uint8_t>(p));
        }
    }
    // zlib stream of stored (uncompressed) deflate blocks.
    std::vector<uint8_t> z = {0x78, 0x01};
    for (size_t off = 0; off < raw.size() || off == 0;) {
        size_t n = std::min<size_t>(65535, raw.size() - off);
        bool last = off + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n)); z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n)); z.push_back(static_cast<uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
        off += n;
        if (last) break;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    put32(z, (b << 16) | a);

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr;
    put32(ihdr, f.width);
    put32(ihdr, f.height);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit RGB
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;
    bool ok = std::fwrite(out.data(), 1, out.size(), fp) == out.size();
    return std::fclose(fp) == 0 && ok;
}

}  // namespace fg
