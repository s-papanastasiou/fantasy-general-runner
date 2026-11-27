// SPDX-License-Identifier: GPL-2.0-or-later
// Local descriptor table: the game's segments.
//
// The master copy of the game's descriptors; bases are game-linear (as DPMI
// reports them). The CPU backend installs each change (cpu_set_descriptor).
#pragma once
#include <array>
#include <cstdint>

namespace fg {

struct Descriptor {
    bool used = false;
    uint32_t base = 0;   // game linear
    uint32_t limit = 0;  // byte granular, inclusive
    uint8_t access = 0;  // DPMI access-rights byte (P DPL S type)
    bool big = true;     // D/B bit
};

class Ldt {
public:
    static constexpr int kEntries = 8192;

    static uint16_t selector_of(int index) { return static_cast<uint16_t>((index << 3) | 7); }
    static int index_of(uint16_t sel) { return sel >> 3; }
    // True if sel refers to an allocated LDT descriptor (TI=1, RPL=3).
    bool valid(uint16_t sel) const;

    // Allocate `count` consecutive descriptors (data, base 0, limit 0).
    // Returns the first selector, or 0.
    uint16_t alloc(int count = 1);
    bool free(uint16_t sel);

    const Descriptor& get(uint16_t sel) const { return table_[index_of(sel)]; }
    uint32_t base(uint16_t sel) const { return table_[index_of(sel)].base; }

    bool set_base(uint16_t sel, uint32_t base);
    bool set_limit(uint16_t sel, uint32_t limit);
    // DPMI 0009h: CL = access byte, CH bit 6 = D/B (granularity is derived from the limit).
    bool set_rights(uint16_t sel, uint8_t access, uint8_t ext);
    // Convenience for the runner's own descriptors.
    uint16_t make(uint32_t base, uint32_t limit, bool code);

private:
    bool install(int index);

    std::array<Descriptor, kEntries> table_{};
};

}  // namespace fg
