// BWSL Arena Allocator
// Standalone memory arena for BWSL compiler
// No external dependencies

#pragma once

#include "bwsl_defs.h"
#include <cstddef>

namespace Memory {

struct BWEMemoryArena {
    void* memory;
    size_t capacity;
    size_t allocated;

    BWEMemoryArena() : memory(nullptr), capacity(0), allocated(0) {}

    void Initialize(void* mem, size_t cap) {
        memory = static_cast<uint8_t*>(mem);
        capacity = cap;
        allocated = 0;
    }

    ~BWEMemoryArena() {}

    BWEResult Reset() {
        this->allocated = 0;
        return SUCCESS;
    }

    void* Allocate(size_t sizeOfAllocation, size_t alignment = alignof(std::max_align_t)) {
        if (memory == nullptr) {
            return nullptr;
        }

        char* memAsChar = static_cast<char*>(memory);

        size_t currentAddress = reinterpret_cast<size_t>(memAsChar + allocated);
        size_t alignAdjust = AlignAdjustment(currentAddress, alignment);
        size_t totalSize = sizeOfAllocation + alignAdjust;

        if (allocated + totalSize > capacity) {
            printf("Allocation failed due to insufficient capacity\n");
            return nullptr;
        }

        void* alignedAddress = static_cast<void*>(memAsChar + allocated + alignAdjust);
        allocated += totalSize;
        return alignedAddress;
    }

private:
    static size_t AlignAdjustment(size_t address, size_t alignment) {
        size_t adjustment = alignment - (address & (alignment - 1));
        if (adjustment == alignment) return 0;
        return adjustment;
    }
};

} // namespace Memory
