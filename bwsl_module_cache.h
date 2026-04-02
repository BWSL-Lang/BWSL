#pragma once

#include "bwsl_defs.h"
#include "bwsl_utils.h"
#include <chrono>

namespace BWSL {
static constexpr u32 INVALID_INDEX  = 0xFFFFFFFF;
static constexpr u32 INVALID_MODULE = 0xFFFFFFFF;
// Forward declarations
struct ModuleAST;
struct CompilationContext; 

// Module cache using Data-Oriented Design principles
// All data is stored in contiguous arrays for cache efficiency
struct ModuleCache {
    static constexpr u32 MAX_MODULES = 256;
    static constexpr u32 MAX_DEPENDENCIES = 1024;
    static constexpr u32 MAX_EXPORTS = 4096;
    static constexpr u32 MAX_SOURCE_SIZE = 1024 * 1024; // 1MB max per module
    static constexpr u32 HASH_TABLE_SIZE = 512; // Power of 2 for fast modulo
    static constexpr u32 HASH_CHAIN_CAPACITY = MAX_MODULES + MAX_EXPORTS;
    
    // Module identification and metadata (hot data)
    struct ModuleID {
        u32 nameHash;           // Fast comparison
        u32 sourceHash;         // Content hash for invalidation
        u32 astRootIndex;       // Index into AST node pool
        u32 dependencyStart;    // Index into dependency arrays
        u16 dependencyCount;    
        u16 exportCount;
        u32 exportStart;        // Index into export arrays
        u32 sourceOffset;       // Offset into source buffer
        u32 sourceLength;       // Length of source code
        u64 lastModified;       // Timestamp for file watching
        u32 compiledDataOffset; // Offset into compiled Metal code buffer
        u32 compiledDataSize;   // Size of compiled Metal code
        u8  status;             // 0=empty, 1=parsing, 2=compiled, 3=error
        u8  _padding[3];
    };
    
    // Export information for symbol resolution
    struct ExportInfo {
        u32 nameHash;           // Hash of exported symbol name
        u32 moduleIndex;        // Which module exports this
        u32 astNodeIndex;       // AST node for the export
        u8  exportType;         // Function, struct, const, etc.
        u8  _padding[3];
    };
    
    // Dependency tracking for incremental compilation
    struct DependencyInfo {
        u32 moduleIndex;        // Index of dependent module
        u32 lastKnownHash;      // Hash when dependency was resolved
    };
    
    // Hash table entry for fast lookups
    struct HashEntry {
        u32 moduleIndex;        // Index into modules array
        u32 nextIndex;          // Chain for collision resolution (index into hashChain)
    };
    
    // ========== Data Storage (Structure of Arrays) ==========
    
    // Hot data - accessed frequently during compilation
    alignas(64) ModuleID modules[MAX_MODULES];
    alignas(64) u32 moduleNameHashes[MAX_MODULES];  // Separate array for fast scanning
    alignas(64) u8 moduleStatus[MAX_MODULES];       // Separate status for cache-line efficiency
    
    // Dependency graph
    alignas(64) DependencyInfo dependencies[MAX_DEPENDENCIES];
    alignas(64) u32 reverseDependencies[MAX_DEPENDENCIES]; // For invalidation propagation
    
    // Export table for symbol resolution  
    alignas(64) ExportInfo exports[MAX_EXPORTS];
    alignas(64) u32 exportNameHashes[MAX_EXPORTS];  // Separate for SIMD scanning
    
    // Hash tables for O(1) lookups
    alignas(64) HashEntry moduleHashTable[HASH_TABLE_SIZE];
    alignas(64) HashEntry exportHashTable[HASH_TABLE_SIZE];
    alignas(64) HashEntry hashChainStorage[HASH_CHAIN_CAPACITY]; // Collision chains
    
    // Cold data - accessed less frequently
    char* sourceCodeBuffer;      // Arena allocated buffer for all source code
    u32 sourceCodeBufferSize;
    u32 sourceCodeBufferUsed;
    
    char* compiledCodeBuffer;    // Arena allocated buffer for Metal code
    u32 compiledCodeBufferSize;
    u32 compiledCodeBufferUsed;

    // AST references - stores NodeRef values (indices into parser's AST)
    // In SoA design, AST lives in the parser, we just store references
    u32* astNodeRefs;            // Arena allocated NodeRef storage
    u32 astNodeRefsSize;
    u32 astNodeRefsUsed;

    // Module names stored separately (cold data)
    char moduleNames[MAX_MODULES][64];
    
    // Statistics and state
    u32 moduleCount;
    u32 exportCount;
    u32 dependencyCount;
    u32 hashChainUsed;
    u64 cacheGeneration;     // Incremented on any change
    
    // ========== Methods ==========
    
    // Initialize cache with arena allocators
    void Initialize(void* sourceArena, u32 sourceArenaSize,
                   void* compiledArena, u32 compiledArenaSize,
                   void* astArena, u32 astArenaSize);
    
    // Module management
    u32 AddModule(const char* name, const char* source, u32 sourceLen);
    bool RemoveModule(u32 moduleIndex);
    u32 FindModule(const char* name) const;
    u32 FindModuleByHash(u32 nameHash) const;
    
    // Dependency management
    void AddDependency(u32 moduleIndex, u32 dependsOnIndex);
    void InvalidateDependents(u32 moduleIndex);
    bool CheckCircularDependency(u32 moduleA, u32 moduleB) const;
    
    // Export/Import resolution
    u32 AddExport(u32 moduleIndex, const char* symbolName, u8 type, u32 astNode);
    u32 FindExport(const char* symbolName) const;
    void GetModuleExports(u32 moduleIndex, ExportInfo* outExports, u32* outCount) const;
    
    // Compilation state
    void MarkModuleCompiled(u32 moduleIndex, const char* metalCode, u32 codeSize);
    void MarkModuleError(u32 moduleIndex);
    bool IsModuleValid(u32 moduleIndex) const;
    bool NeedsRecompilation(u32 moduleIndex) const;
    
    // Cache queries
    const char* GetModuleSource(u32 moduleIndex) const;
    const char* GetCompiledCode(u32 moduleIndex) const;
    u32 GetModuleASTRef(u32 moduleIndex) const;  // Returns NodeRef packed value
    
    // Batch operations for efficiency
    void InvalidateAll();
    void CompactCache();  // Defragment arrays
    void SerializeCache(void* buffer, u32* size) const;
    void DeserializeCache(const void* buffer, u32 size);
    
    // Statistics
    struct CacheStats {
        u32 totalModules;
        u32 compiledModules;
        u32 errorModules;
        u32 totalExports;
        u32 totalDependencies;
        u32 sourceMemoryUsed;
        u32 compiledMemoryUsed;
        u32 astMemoryUsed;
        f32 hashTableLoadFactor;
        u32 cacheHits;
        u32 cacheMisses;
    };
    
    CacheStats GetStatistics() const;
    
private:
    // Hash functions
    static u32 HashBuffer(const void* buffer, u32 size);
    
    // Internal helpers
    u32 AllocateHashChain();
    void RebuildHashTable();
    void PropagateInvalidation(u32 moduleIndex);
};

// Global module cache instance
extern ModuleCache g_moduleCache;

// ========== Inline implementations for hot paths ==========

inline u32 ModuleCache::FindModuleByHash(u32 nameHash) const {
    u32 slot = nameHash & (HASH_TABLE_SIZE - 1);
    u32 index = moduleHashTable[slot].moduleIndex;

    if (index != INVALID_INDEX && moduleNameHashes[index] == nameHash) {
        return index;
    }

    u32 chainIndex = moduleHashTable[slot].nextIndex;
    while (chainIndex != INVALID_INDEX && chainIndex < hashChainUsed) {
        index = hashChainStorage[chainIndex].moduleIndex;
        if (index != INVALID_INDEX && moduleNameHashes[index] == nameHash) {
            return index;
        }
        chainIndex = hashChainStorage[chainIndex].nextIndex;
    }

    return INVALID_INDEX;
}

inline bool ModuleCache::IsModuleValid(u32 moduleIndex) const {
    return moduleIndex < moduleCount && moduleStatus[moduleIndex] == 2;
}

inline const char* ModuleCache::GetModuleSource(u32 moduleIndex) const {
    if (moduleIndex >= moduleCount) return nullptr;
    return sourceCodeBuffer + modules[moduleIndex].sourceOffset;
}

inline const char* ModuleCache::GetCompiledCode(u32 moduleIndex) const {
    if (!IsModuleValid(moduleIndex)) return nullptr;
    return compiledCodeBuffer + modules[moduleIndex].compiledDataOffset;
}

} // namespace BWSL
