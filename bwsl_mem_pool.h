// BWSL Memory Pool
// Standalone chunked memory pool for BWSL IR
// No external dependencies

#pragma once

#include "bwsl_defs.h"
#include <vector>
#include <cstring>
#include <algorithm>

struct IRMemoryPool {
    static constexpr size_t CHUNK_SIZE = 64 * 1024;

    struct Allocation {
        void* ptr;
        size_t size;
        size_t capacity;
    };

    u8* currentChunk = nullptr;
    size_t chunkUsed = 0;
    size_t chunkCapacity = 0;
    std::vector<u8*> chunks;
    std::vector<Allocation> allocations;

    void* Allocate(size_t size, size_t align = 16) {
        size_t alignedSize = (size + align - 1) & ~(align - 1);

        if (chunkUsed + alignedSize > chunkCapacity) {
            currentChunk = new u8[CHUNK_SIZE];
            chunks.push_back(currentChunk);
            chunkUsed = 0;
            chunkCapacity = CHUNK_SIZE;
        }

        void* ptr = currentChunk + chunkUsed;
        chunkUsed += alignedSize;

        allocations.push_back({ptr, size, alignedSize});
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
        for (auto* chunk : chunks) {
            delete[] chunk;
        }
    }
};
