// SPDX-License-Identifier: GPL-2.0-or-later
#include "le_image.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

#include "address_space.h"

namespace fg {

namespace {

struct Reader {
    const std::vector<uint8_t>& d;
    bool ok(size_t off, size_t n) const { return off <= d.size() && n <= d.size() - off; }
    uint8_t u8(size_t o) const { return d[o]; }
    uint16_t u16(size_t o) const { uint16_t v; std::memcpy(&v, &d[o], 2); return v; }
    uint32_t u32(size_t o) const { uint32_t v; std::memcpy(&v, &d[o], 4); return v; }
};

// Walk the chain of MZ executables to the one whose e_lfanew points at "LE".
bool find_le(const Reader& r, size_t& mz, size_t& le) {
    size_t off = 0;
    while (r.ok(off, 0x40) && r.u16(off) == 0x5A4D) {
        uint32_t lfanew = r.u32(off + 0x3C);
        if (lfanew && r.ok(off + lfanew, 4) && r.u16(off + lfanew) == 0x454C) {
            mz = off;
            le = off + lfanew;
            return true;
        }
        uint16_t last = r.u16(off + 2), pages = r.u16(off + 4);
        size_t size = pages * 512u - (last ? 512u - last : 0u);
        if (size == 0) break;
        off += size;
    }
    // Fallback (BARENA.EXE): the DOS/4GW stub is followed by other data, so
    // find an "LE" signature and the MZ header whose e_lfanew points at it.
    for (size_t i = 0; r.ok(i, 4); ++i) {
        if (r.u32(i) != 0x0000454C) continue;
        for (size_t m = i >= 0x40 ? i - 0x40 : 0;; --m) {
            if (r.u16(m) == 0x5A4D && r.ok(m, 0x40) && m + r.u32(m + 0x3C) == i) {
                mz = m;
                le = i;
                return true;
            }
            if (m == 0) break;
        }
    }
    return false;
}

}  // namespace

uint32_t LeImage::to_ghidra(uint32_t lin) const {
    for (const auto& o : objects)
        if (lin >= o.load_base && lin - o.load_base < o.virtual_size)
            return o.preferred_base + (lin - o.load_base);
    return ~0u;
}

bool load_le(const std::string& path, AddressSpace& as, uint32_t bias, LeImage& out, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path; return false; }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Reader r{data};

    size_t mz, le;
    if (!find_le(r, mz, le) || !r.ok(le, 0xB0)) { err = "no LE header in " + path; return false; }

    uint32_t num_pages = r.u32(le + 0x14);
    uint32_t page_size = r.u32(le + 0x28);
    uint32_t last_page = r.u32(le + 0x2C);
    uint32_t obj_table = r.u32(le + 0x40), num_objs = r.u32(le + 0x44);
    uint32_t page_map = r.u32(le + 0x48);
    uint32_t fixup_pages = r.u32(le + 0x68), fixup_records = r.u32(le + 0x6C);
    uint32_t data_pages = r.u32(le + 0x80);
    out.entry_object = r.u32(le + 0x18);
    out.entry_offset = r.u32(le + 0x1C);
    out.stack_object = r.u32(le + 0x20);
    out.stack_offset = r.u32(le + 0x24);

    struct Obj { uint32_t first_page, page_count; };
    std::vector<Obj> pages_of;
    out.objects.clear();
    for (uint32_t i = 0; i < num_objs; ++i) {
        size_t e = le + obj_table + 24 * i;
        LeObject o{};
        o.virtual_size = r.u32(e);
        o.preferred_base = r.u32(e + 4);
        o.flags = r.u32(e + 8);
        o.load_base = o.preferred_base + bias;
        if (!as.commit(o.load_base, o.virtual_size)) { err = "cannot map object"; return false; }
        std::memset(as.host(o.load_base), 0, o.virtual_size);
        out.objects.push_back(o);
        pages_of.push_back({r.u32(e + 12), r.u32(e + 16)});
    }
    if (out.entry_object < 1 || out.entry_object > num_objs || out.stack_object < 1 || out.stack_object > num_objs) {
        err = "bad entry/stack object";
        return false;
    }

    // Copy page data. LE page map entries: 3-byte page number (high byte first) + type.
    auto object_of_page = [&](uint32_t page) -> int {  // page is 1-based
        for (size_t i = 0; i < pages_of.size(); ++i)
            if (page >= pages_of[i].first_page && page < pages_of[i].first_page + pages_of[i].page_count) return int(i);
        return -1;
    };
    for (uint32_t p = 1; p <= num_pages; ++p) {
        size_t e = le + page_map + 4 * (p - 1);
        uint32_t num = (r.u8(e) << 16) | (r.u8(e + 1) << 8) | r.u8(e + 2);
        uint8_t type = r.u8(e + 3);
        int oi = object_of_page(p);
        if (oi < 0 || type != 0 || num == 0) continue;
        uint32_t size = p == num_pages ? last_page : page_size;
        size_t src = mz + data_pages + size_t(num - 1) * page_size;
        if (!r.ok(src, size)) { err = "page data outside file"; return false; }
        LeObject& o = out.objects[oi];
        uint32_t off = (p - pages_of[oi].first_page) * page_size;
        uint32_t n = std::min<uint32_t>(size, o.virtual_size > off ? o.virtual_size - off : 0);
        std::memcpy(as.host(o.load_base + off), &data[src], n);
    }

    // Fixups. Only internal-reference records occur in this executable.
    out.fixups_applied = 0;
    for (uint32_t p = 1; p <= num_pages; ++p) {
        size_t rec = le + fixup_records + r.u32(le + fixup_pages + 4 * (p - 1));
        size_t end = le + fixup_records + r.u32(le + fixup_pages + 4 * p);
        int oi = object_of_page(p);
        while (rec < end) {
            uint8_t src_type = r.u8(rec), flags = r.u8(rec + 1);
            rec += 2;
            std::vector<int16_t> srcs;
            uint8_t count = 0;
            if (src_type & 0x20) count = r.u8(rec++);
            else { srcs.push_back(static_cast<int16_t>(r.u16(rec))); rec += 2; }
            if ((flags & 3) != 0) { err = "unsupported fixup target type"; return false; }
            uint32_t tobj;
            if (flags & 0x40) { tobj = r.u16(rec); rec += 2; } else { tobj = r.u8(rec++); }
            uint32_t toff = 0;
            if ((src_type & 0x0F) != 0x02) {
                if (flags & 0x10) { toff = r.u32(rec); rec += 4; } else { toff = r.u16(rec); rec += 2; }
            }
            if (flags & 0x04) rec += (flags & 0x20) ? 4 : 2;  // additive (not used here)
            for (uint8_t i = 0; i < count; ++i) { srcs.push_back(static_cast<int16_t>(r.u16(rec))); rec += 2; }
            if (tobj < 1 || tobj > num_objs) { err = "bad fixup target object"; return false; }
            if ((src_type & 0x0F) != 0x07) { err = "unsupported fixup source type"; return false; }
            if (oi < 0) continue;

            uint32_t value = out.objects[tobj - 1].load_base + toff;
            LeObject& o = out.objects[oi];
            for (int16_t s : srcs) {
                int64_t pos = int64_t(p - pages_of[oi].first_page) * page_size + s;
                // Records straddling a page boundary appear in both pages; write
                // only the bytes inside the object.
                for (int b = 0; b < 4; ++b)
                    if (pos + b >= 0 && pos + b < o.virtual_size)
                        as.host(o.load_base)[pos + b] = static_cast<uint8_t>(value >> (8 * b));
                if (s >= 0) ++out.fixups_applied;
            }
        }
    }
    return true;
}

}  // namespace fg
