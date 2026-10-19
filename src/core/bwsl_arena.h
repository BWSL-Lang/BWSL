// BWSL Arena Allocator
// Standalone memory arena for BWSL compiler
// No external dependencies

#pragma once

#include "bwsl_defs.h"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace Memory {

// Most bytes a growable arena may hold across all of its blocks unless the
// caller asks for another limit. Set once per process (bwslc -arena-limit).
// The largest real shaders need tens of MiB; the limit exists so that a
// compiler bug that allocates in a loop still ends in a diagnostic instead
// of exhausting the machine.
inline size_t g_arenaGrowthLimit = size_t(1024) * 1024 * 1024;

// Thrown by a growable arena when an allocation would take it past its
// limit, or when the system cannot supply another block. A growable arena
// never returns nullptr, so running out is reported rather than crashing at
// the first null dereference.
struct ArenaExhausted : std::runtime_error {
    size_t limitBytes;
    size_t requestedBytes;
    bool systemOutOfMemory;  // The limit was not reached; malloc failed

    ArenaExhausted(size_t limit, size_t requested, bool outOfMemory)
        : std::runtime_error(Describe(limit, requested, outOfMemory)),
          limitBytes(limit),
          requestedBytes(requested),
          systemOutOfMemory(outOfMemory) {}

private:
    static std::string Describe(size_t limit, size_t requested, bool systemOutOfMemory) {
        char text[192];
        if (systemOutOfMemory) {
            std::snprintf(text, sizeof(text),
                          "out of memory: the system refused another compiler arena block "
                          "(%zu-byte allocation)", requested);
        } else {
            std::snprintf(text, sizeof(text),
                          "compiler memory limit of %zu MiB reached (%zu-byte allocation)",
                          limit / (1024 * 1024), requested);
        }
        return text;
    }
};

struct BWEMemoryArena {
    // The block being filled. `allocated` counts bytes used in this block only.
    void* memory;
    size_t capacity;
    size_t allocated;

    // A fixed arena returns nullptr once its block is full. After
    // EnableGrowth() the arena instead chains another block: blocks never
    // move, so every pointer it has handed out stays valid. The caller owns
    // the block passed to Initialize(); the arena owns and frees the rest.
    struct OverflowBlock {
        OverflowBlock* previous;
        size_t size;  // Header included
    };
    OverflowBlock* overflowBlocks;
    void* firstMemory;
    size_t firstCapacity;
    size_t growthBlockSize;  // 0 = fixed
    size_t growthLimit;      // Bytes across all blocks, the first included
    size_t reservedBytes;    // Bytes across all blocks

    BWEMemoryArena()
        : memory(nullptr), capacity(0), allocated(0),
          overflowBlocks(nullptr), firstMemory(nullptr), firstCapacity(0),
          growthBlockSize(0), growthLimit(0), reservedBytes(0) {}

    BWEMemoryArena(const BWEMemoryArena&) = delete;
    BWEMemoryArena& operator=(const BWEMemoryArena&) = delete;

    void Initialize(void* mem, size_t cap) {
        FreeOverflowBlocks();
        memory = static_cast<uint8_t*>(mem);
        capacity = cap;
        allocated = 0;
        firstMemory = mem;
        firstCapacity = cap;
        reservedBytes = cap;
    }

    // Let the arena grow by blocks of at least `blockSize` bytes until all of
    // its blocks together would exceed `limit` bytes; past that, Allocate
    // throws ArenaExhausted.
    void EnableGrowth(size_t blockSize, size_t limit = g_arenaGrowthLimit) {
        growthBlockSize = blockSize > 0 ? blockSize : 1;
        growthLimit = limit;
    }

    ~BWEMemoryArena() {
        FreeOverflowBlocks();
    }

    BWEResult Reset() {
        FreeOverflowBlocks();
        memory = firstMemory;
        capacity = firstCapacity;
        reservedBytes = firstCapacity;
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

        if (totalSize < sizeOfAllocation || totalSize > capacity - allocated) {
            if (growthBlockSize != 0) {
                return AllocateInNewBlock(sizeOfAllocation, alignment);
            }
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

    void* AllocateInNewBlock(size_t sizeOfAllocation, size_t alignment) {
        const size_t header = sizeof(OverflowBlock);
        const size_t available = growthLimit > reservedBytes ? growthLimit - reservedBytes : 0;
        // Room for the header, worst-case alignment padding and the request.
        if (sizeOfAllocation > available || alignment > available - sizeOfAllocation ||
            header > available - sizeOfAllocation - alignment) {
            throw ArenaExhausted(growthLimit, sizeOfAllocation, false);
        }
        const size_t needed = header + alignment + sizeOfAllocation;
        size_t blockSize = needed > growthBlockSize ? needed : growthBlockSize;
        if (blockSize > available) {
            blockSize = available;
        }

        // Zeroed, like the fresh pages a large malloc hands the first block.
        void* raw = calloc(1, blockSize);
        if (raw == nullptr) {
            throw ArenaExhausted(growthLimit, sizeOfAllocation, true);
        }
        OverflowBlock* block = static_cast<OverflowBlock*>(raw);
        block->previous = overflowBlocks;
        block->size = blockSize;
        overflowBlocks = block;
        reservedBytes += blockSize;

        char* blockMemory = static_cast<char*>(raw) + header;
        size_t alignAdjust = AlignAdjustment(reinterpret_cast<size_t>(blockMemory), alignment);
        memory = blockMemory;
        capacity = blockSize - header;
        allocated = alignAdjust + sizeOfAllocation;
        return blockMemory + alignAdjust;
    }

    void FreeOverflowBlocks() {
        while (overflowBlocks != nullptr) {
            OverflowBlock* previous = overflowBlocks->previous;
            free(overflowBlocks);
            overflowBlocks = previous;
        }
    }
};

} // namespace Memory
