// SPDX-License-Identifier: GPL-2.0-or-later
#include "ldt.h"

#include "cpu_backend.h"

namespace fg {

namespace {
constexpr int kFirstFree = 1;  // keep entry 0 unused so selector 0x07 is never handed out
constexpr uint8_t kDataRW = 0xF2;  // present, DPL3, data, read/write
constexpr uint8_t kCodeRX = 0xFA;  // present, DPL3, code, read/execute
}  // namespace

bool Ldt::valid(uint16_t sel) const {
    if ((sel & 7) != 7) return false;
    int i = index_of(sel);
    return i >= kFirstFree && i < kEntries && table_[i].used;
}

uint16_t Ldt::alloc(int count) {
    for (int i = kFirstFree; i + count <= kEntries; ++i) {
        int j = 0;
        while (j < count && !table_[i + j].used) ++j;
        if (j < count) { i += j; continue; }
        for (j = 0; j < count; ++j) {
            table_[i + j] = Descriptor{true, 0, 0, kDataRW, true};
            if (!install(i + j)) return 0;
        }
        return selector_of(i);
    }
    return 0;
}

bool Ldt::free(uint16_t sel) {
    if (!valid(sel)) return false;
    int i = index_of(sel);
    table_[i] = Descriptor{};
    return cpu_clear_descriptor(i);
}

bool Ldt::set_base(uint16_t sel, uint32_t base) {
    if (!valid(sel)) return false;
    table_[index_of(sel)].base = base;
    return install(index_of(sel));
}

bool Ldt::set_limit(uint16_t sel, uint32_t limit) {
    if (!valid(sel)) return false;
    table_[index_of(sel)].limit = limit;
    return install(index_of(sel));
}

bool Ldt::set_rights(uint16_t sel, uint8_t access, uint8_t ext) {
    if (!valid(sel)) return false;
    Descriptor& d = table_[index_of(sel)];
    d.access = access | 0x10;  // always a code/data (non-system) descriptor
    d.big = (ext & 0x40) != 0;
    return install(index_of(sel));
}

uint16_t Ldt::make(uint32_t base, uint32_t limit, bool code) {
    uint16_t sel = alloc(1);
    if (!sel) return 0;
    Descriptor& d = table_[index_of(sel)];
    d.base = base;
    d.limit = limit;
    d.access = code ? kCodeRX : kDataRW;
    d.big = true;
    return install(index_of(sel)) ? sel : 0;
}

bool Ldt::install(int index) { return cpu_set_descriptor(index, table_[index]); }

}  // namespace fg
