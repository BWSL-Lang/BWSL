// BWSL Memory Pool
// Standalone chunked memory pool for BWSL IR
// No external dependencies

#pragma once

#include "bwsl_defs.h"
#include <vector>
#include <cstring>
#include <algorithm>
#include <cstdint>
#include <new>

struct IRMemoryPool {
    static constexpr size_t CHUNK_SIZE = 64 * 1024;
    static constexpr size_t MAX_ALIGNMENT = 64;

    struct Allocation {
        void* ptr;
        size_t size;
        size_t reservedSize;
    };

    struct Chunk {
        u8* data;
    };

    u8* currentChunk = nullptr;
    size_t chunkUsed = 0;
    size_t chunkCapacity = 0;
    std::vector<Chunk> chunks;
    std::vector<Allocation> allocations;

    static size_t AlignUp(size_t value, size_t align) {
        return (value + align - 1) & ~(align - 1);
    }

    void AllocateChunk(size_t requestedSize = CHUNK_SIZE) {
        size_t actualSize = std::max(requestedSize, CHUNK_SIZE);
        void* raw = ::operator new(actualSize, std::align_val_t(MAX_ALIGNMENT));
        currentChunk = static_cast<u8*>(raw);
        chunks.push_back({currentChunk});
        chunkUsed = 0;
        chunkCapacity = actualSize;
    }

    void* Allocate(size_t size, size_t align = 16) {
        if (align == 0) {
            align = 1;
        }
        if (align > MAX_ALIGNMENT) {
            align = MAX_ALIGNMENT;
        }

        if (!currentChunk) {
            AllocateChunk();
        }

        size_t alignedOffset = AlignUp(reinterpret_cast<size_t>(currentChunk + chunkUsed), align) -
                               reinterpret_cast<size_t>(currentChunk);
        if (alignedOffset + size > chunkCapacity) {
            // Request a chunk large enough for this allocation. Without
            // this, allocations larger than CHUNK_SIZE (e.g. IR opcodes
            // array after several doublings — 32k u16 = 64k, next grow is
            // 128k) wrote past the newly-allocated chunk -> heap overflow.
            size_t needed = size + (align > 1 ? align : 0);
            AllocateChunk(needed);
            alignedOffset = AlignUp(reinterpret_cast<size_t>(currentChunk), align) -
                            reinterpret_cast<size_t>(currentChunk);
        }

        void* ptr = currentChunk + alignedOffset;
        chunkUsed = alignedOffset + size;

        allocations.push_back({ptr, size, chunkUsed - alignedOffset});
        return ptr;
    }

    void* Reallocate(void* oldPtr, size_t newSize, size_t align = 16) {
        void* newPtr = Allocate(newSize, align);

        for (auto& alloc : allocations) {
            if (alloc.ptr == oldPtr) {
                memcpy(newPtr, oldPtr, std::min(alloc.size, newSize));
                break;
            }
        }

        return newPtr;
    }

    ~IRMemoryPool() {
        for (const auto& chunk : chunks) {
            ::operator delete(chunk.data, std::align_val_t(MAX_ALIGNMENT));
        }
    }
};
