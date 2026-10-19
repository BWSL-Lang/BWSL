// Unit tests for Memory::BWEMemoryArena: fixed arenas keep returning nullptr
// when full; growable arenas chain blocks without moving earlier allocations
// and throw ArenaExhausted at their limit.
#include "core/bwsl_arena.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// Checks run in every build mode; several of them allocate.
#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            std::exit(1); \
        } \
    } while (0)

using Memory::ArenaExhausted;
using Memory::BWEMemoryArena;

static bool IsAligned(const void* pointer, size_t alignment) {
    return (reinterpret_cast<uintptr_t>(pointer) & (alignment - 1)) == 0;
}

static void FixedArenaStillReturnsNull() {
    alignas(64) static unsigned char buffer[256];
    BWEMemoryArena arena;
    arena.Initialize(buffer, sizeof(buffer));
    CHECK(arena.Allocate(200, 8) != nullptr);
    CHECK(arena.Allocate(100, 8) == nullptr);
    CHECK(arena.overflowBlocks == nullptr);
}

static void GrowthKeepsEarlierAllocations() {
    std::vector<unsigned char> first(1024);
    BWEMemoryArena arena;
    arena.Initialize(first.data(), first.size());
    arena.EnableGrowth(4096, 1024 * 1024);

    std::vector<unsigned char*> pointers;
    for (u32 i = 0; i < 400; ++i) {
        size_t alignment = (i % 3 == 0) ? 64 : 8;
        auto* pointer = static_cast<unsigned char*>(arena.Allocate(100, alignment));
        CHECK(pointer != nullptr);
        CHECK(IsAligned(pointer, alignment));
        std::memset(pointer, static_cast<int>(i & 0xFF), 100);
        pointers.push_back(pointer);
    }
    CHECK(arena.overflowBlocks != nullptr);
    CHECK(arena.reservedBytes > first.size());

    // Larger than a growth block: gets a block of its own.
    auto* large = static_cast<unsigned char*>(arena.Allocate(10000, 64));
    CHECK(large != nullptr && IsAligned(large, 64));
    for (size_t i = 0; i < 10000; ++i) {
        CHECK(large[i] == 0);  // Growth blocks start zeroed
    }
    std::memset(large, 0xAB, 10000);

    for (u32 i = 0; i < pointers.size(); ++i) {
        for (u32 byte = 0; byte < 100; ++byte) {
            CHECK(pointers[i][byte] == static_cast<unsigned char>(i & 0xFF));
        }
    }
}

static void LimitThrowsInsteadOfReturningNull() {
    std::vector<unsigned char> first(1024);
    BWEMemoryArena arena;
    arena.Initialize(first.data(), first.size());
    arena.EnableGrowth(2048, 8192);

    bool threw = false;
    try {
        for (u32 i = 0; i < 1000; ++i) {
            CHECK(arena.Allocate(64, 8) != nullptr);
        }
    } catch (const ArenaExhausted& error) {
        threw = true;
        CHECK(error.limitBytes == 8192);
        CHECK(!error.systemOutOfMemory);
        CHECK(std::strstr(error.what(), "limit") != nullptr);
    }
    CHECK(threw);
    CHECK(arena.reservedBytes <= 8192);

    // A request whose size would overflow the bookkeeping is refused too.
    threw = false;
    try {
        arena.Allocate(SIZE_MAX - 8, 64);
    } catch (const ArenaExhausted&) {
        threw = true;
    }
    CHECK(threw);
}

static void LimitAtFirstBlockMeansNoGrowth() {
    std::vector<unsigned char> first(4096);
    BWEMemoryArena arena;
    arena.Initialize(first.data(), first.size());
    arena.EnableGrowth(4096, first.size());
    CHECK(arena.Allocate(4000, 8) != nullptr);
    bool threw = false;
    try {
        arena.Allocate(200, 8);
    } catch (const ArenaExhausted&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(arena.overflowBlocks == nullptr);
}

static void ResetReturnsToFirstBlock() {
    std::vector<unsigned char> first(1024);
    BWEMemoryArena arena;
    arena.Initialize(first.data(), first.size());
    arena.EnableGrowth(4096, 1024 * 1024);
    for (u32 i = 0; i < 100; ++i) {
        arena.Allocate(100, 8);
    }
    CHECK(arena.overflowBlocks != nullptr);

    arena.Reset();
    CHECK(arena.overflowBlocks == nullptr);
    CHECK(arena.memory == first.data());
    CHECK(arena.capacity == first.size());
    CHECK(arena.reservedBytes == first.size());
    void* again = arena.Allocate(16, 8);
    CHECK(again >= static_cast<void*>(first.data()) &&
           again < static_cast<void*>(first.data() + first.size()));
}

int main() {
    FixedArenaStillReturnsNull();
    GrowthKeepsEarlierAllocations();
    LimitThrowsInsteadOfReturningNull();
    LimitAtFirstBlockMeansNoGrowth();
    ResetReturnsToFirstBlock();
    std::printf("arena tests passed\n");
    return 0;
}
