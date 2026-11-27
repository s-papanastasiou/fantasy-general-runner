// SPDX-License-Identifier: GPL-2.0-or-later
#include "address_space.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include <algorithm>
#include <iterator>

#include "cpu_backend.h"
#include "log.h"

namespace fg {

namespace {
constexpr uint32_t kPage = 0x1000;
uint32_t page_up(uint32_t v) { return (v + kPage - 1) & ~(kPage - 1); }
}  // namespace

bool AddressSpace::init(bool all_accessible) {
    all_accessible_ = all_accessible;
#ifdef _WIN32
    // Only the emulator backends run on Windows: the whole region is usable.
    // Pages cost memory only once touched.
    void* p = all_accessible ? VirtualAlloc(nullptr, kSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) : nullptr;
    if (!p) {
#else
    int prot = all_accessible ? PROT_READ | PROT_WRITE : PROT_NONE;
    void* p = mmap(nullptr, kSize, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
#endif
        FG_LOG("cannot reserve %u MB for the game address space", kSize >> 20);
        return false;
    }
    base_ = static_cast<uint8_t*>(p);
    return commit(0, kDosTop);
}

uint32_t AddressSpace::linear_of(const void* p) const {
    auto a = reinterpret_cast<const uint8_t*>(p);
    if (a < base_ || a >= base_ + kSize) return ~0u;
    return static_cast<uint32_t>(a - base_);
}

bool AddressSpace::commit(uint32_t lin, uint32_t size) {
    uint32_t start = lin & ~(kPage - 1);
    uint32_t end = page_up(lin + size);
    if (!contains(start, end - start)) return false;
    if (all_accessible_) return true;
#ifdef _WIN32
    return false;
#else
    // Executable too: the game writes into its code object and may run code
    // placed in data (DOS memory callbacks, self-modifying startup code).
    return mprotect(base_ + start, end - start, PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
#endif
}

void AddressSpace::release(uint32_t lin, uint32_t size) {
#ifdef _WIN32
    // Decommit and recommit: the memory reads as zeros from now on.
    VirtualFree(host(lin), size, MEM_DECOMMIT);
    VirtualAlloc(host(lin), size, MEM_COMMIT, PAGE_READWRITE);
#else
    if (!all_accessible_) mprotect(host(lin), size, PROT_NONE);
    madvise(host(lin), size, MADV_DONTNEED);  // reads as zeros from now on
#endif
    cpu_code_written(lin, size);
}

uint32_t AddressSpace::heap_alloc(uint32_t size) {
    size = page_up(size ? size : 1);
    uint32_t cur = kHeapStart;
    for (auto& [lin, len] : heap_) {
        if (lin - cur >= size) break;
        cur = lin + len;
    }
    if (cur + size > kSize || cur + size < cur) return 0;
    if (!commit(cur, size)) return 0;
    std::memset(host(cur), 0, size);
    heap_[cur] = size;
    return cur;
}

bool AddressSpace::heap_free(uint32_t lin) {
    auto it = heap_.find(lin);
    if (it == heap_.end()) return false;
    release(lin, it->second);
    heap_.erase(it);
    return true;
}

uint32_t AddressSpace::heap_resize(uint32_t lin, uint32_t size) {
    auto it = heap_.find(lin);
    if (it == heap_.end()) return 0;
    size = page_up(size ? size : 1);
    uint32_t old = it->second;
    if (size <= old) {
        it->second = size;
        if (size < old) {
            release(lin + size, old - size);
        }
        return lin;
    }
    auto next = std::next(it);
    uint32_t limit = next == heap_.end() ? kSize : next->first;
    if (lin + size <= limit && commit(lin, size)) {
        std::memset(host(lin + old), 0, size - old);
        it->second = size;
        return lin;
    }
    uint32_t moved = heap_alloc(size);
    if (!moved) return 0;
    std::memcpy(host(moved), host(lin), old);
    heap_free(lin);
    return moved;
}

uint32_t AddressSpace::heap_free_bytes() const {
    uint32_t used = 0;
    for (auto& [lin, len] : heap_) used += len;
    return (kSize - kHeapStart) - used;
}

uint32_t AddressSpace::conv_largest_free() const {
    uint32_t best = 0, cur = kConvStart >> 4;
    for (auto& [seg, paras] : conv_) {
        if (seg > cur) best = std::max(best, static_cast<uint32_t>(seg - cur));
        cur = seg + paras;
    }
    return std::max(best, (kConvEnd >> 4) - cur);
}

uint16_t AddressSpace::conv_alloc(uint32_t paragraphs, uint32_t* largest_free) {
    uint32_t cur = kConvStart >> 4;
    for (auto& [seg, paras] : conv_) {
        if (seg - cur >= paragraphs) break;
        cur = seg + paras;
    }
    if (paragraphs == 0 || cur + paragraphs > (kConvEnd >> 4)) {
        if (largest_free) *largest_free = conv_largest_free();
        return 0;
    }
    conv_[cur] = paragraphs;
    std::memset(host(cur << 4), 0, paragraphs << 4);
    return static_cast<uint16_t>(cur);
}

bool AddressSpace::conv_free(uint16_t segment) { return conv_.erase(segment) == 1; }

bool AddressSpace::conv_resize(uint16_t segment, uint32_t paragraphs, uint32_t* largest_free) {
    auto it = conv_.find(segment);
    if (it == conv_.end()) return false;
    auto next = std::next(it);
    uint32_t limit = next == conv_.end() ? (kConvEnd >> 4) : next->first;
    if (segment + paragraphs > limit) {
        if (largest_free) *largest_free = limit - segment;
        return false;
    }
    it->second = paragraphs;
    return true;
}

}  // namespace fg
