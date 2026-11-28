// SPDX-License-Identifier: GPL-2.0-or-later
// The game's linear address space.
//
// The game runs with segment selectors whose base is the start of one large
// reserved host region, so game linear address L lives at host address
// base() + L. That gives the game a DOS-like low megabyte (IVT, BIOS data
// area, conventional memory, VGA window at 0xA0000) without mapping host
// page zero.
//
// Layout (game linear):
//   0x00000000-0x000FFFFF  DOS first megabyte (always mapped)
//   0x01000000-...         the program's LE objects (Ghidra address + kObjectBias)
//   0x02000000-0x0FFFFFFF  DPMI heap (committed on allocation)
#pragma once
#include <cstdint>
#include <cstring>
#include <map>

namespace fg {

class AddressSpace {
public:
    static constexpr uint32_t kSize = 0x10000000;
    static constexpr uint32_t kDosTop = 0x00100000;
    static constexpr uint32_t kObjectBias = 0x01000000;
    static constexpr uint32_t kHeapStart = 0x02000000;
    // Conventional memory handed out by DOS/DPMI allocations (paragraph granular).
    static constexpr uint32_t kConvStart = 0x00001000;
    static constexpr uint32_t kConvEnd = 0x000A0000;

    // Reserves the region, all of it readable and writable.
    bool init();

    uintptr_t host_base() const { return reinterpret_cast<uintptr_t>(base_); }
    uint8_t* host(uint32_t lin) const { return base_ + lin; }
    bool contains(uint32_t lin, uint32_t len) const { return lin < kSize && len <= kSize - lin; }
    // Game linear address of a host pointer inside the region (or ~0u).
    uint32_t linear_of(const void* p) const;

    // Check that [lin, lin+size) lies in the region (all of it is accessible).
    bool commit(uint32_t lin, uint32_t size);

    template <typename T> T read(uint32_t lin) const { T v; std::memcpy(&v, host(lin), sizeof v); return v; }
    template <typename T> void write(uint32_t lin, T v) { std::memcpy(host(lin), &v, sizeof v); }

    // DPMI heap: page-aligned blocks.
    uint32_t heap_alloc(uint32_t size);
    bool heap_free(uint32_t lin);
    uint32_t heap_resize(uint32_t lin, uint32_t size);  // may move; 0 on failure
    uint32_t heap_free_bytes() const;

    // Conventional memory, in paragraphs. Returns segment or 0.
    uint16_t conv_alloc(uint32_t paragraphs, uint32_t* largest_free = nullptr);
    bool conv_free(uint16_t segment);
    bool conv_resize(uint16_t segment, uint32_t paragraphs, uint32_t* largest_free = nullptr);

private:
    uint8_t* base_ = nullptr;
    std::map<uint32_t, uint32_t> heap_;  // lin -> size (page multiple)
    std::map<uint32_t, uint32_t> conv_;  // segment -> paragraphs
    uint32_t conv_largest_free() const;
    void release(uint32_t lin, uint32_t size);
};

}  // namespace fg
