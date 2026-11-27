// SPDX-License-Identifier: GPL-2.0-or-later
// Loader for the DOS/4GW-bound LE executable (BARENA.EXE).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace fg {

class AddressSpace;

struct LeObject {
    uint32_t preferred_base;  // base in the file (= Ghidra address of offset 0)
    uint32_t load_base;       // game linear address it was loaded at
    uint32_t virtual_size;
    uint32_t flags;
};

struct LeImage {
    std::vector<LeObject> objects;
    uint32_t entry_object = 0, entry_offset = 0;  // object index 1-based
    uint32_t stack_object = 0, stack_offset = 0;
    uint32_t fixups_applied = 0;

    uint32_t entry() const { return objects[entry_object - 1].load_base + entry_offset; }
    uint32_t stack_top() const { return objects[stack_object - 1].load_base + stack_offset; }
    // Map a game linear address back to the Ghidra address (preferred base); ~0u if outside.
    uint32_t to_ghidra(uint32_t lin) const;
};

// Reads `path`, maps every object at `preferred_base + bias` in `as`
// and applies all fixups. Returns false (with a message) on failure.
bool load_le(const std::string& path, AddressSpace& as, uint32_t bias, LeImage& out, std::string& err);

}  // namespace fg
