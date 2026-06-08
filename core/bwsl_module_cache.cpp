#include "bwsl_module_cache.h"
#include <cstring>
#include <algorithm>

namespace BWSL {

ModuleCache g_moduleCache;

void ModuleCache::Initialize(void* sourceArena, u32 sourceArenaSize,
                            void* compiledArena, u32 compiledArenaSize,
                            void* astArena, u32 astArenaSize) {
    // Initialize buffers
    sourceCodeBuffer = (char*)sourceArena;
    sourceCodeBufferSize = sourceArenaSize;
    sourceCodeBufferUsed = 0;

    compiledCodeBuffer = (char*)compiledArena;
    compiledCodeBufferSize = compiledArenaSize;
    compiledCodeBufferUsed = 0;

    // AST refs store NodeRef packed values (u32)
    astNodeRefs = (u32*)astArena;
    astNodeRefsSize = astArenaSize / sizeof(u32);
    astNodeRefsUsed = 0;

    // Initialize arrays with invalid values
    memset(modules, 0xFF, sizeof(modules));
    memset(moduleNameHashes, 0, sizeof(moduleNameHashes));
    memset(moduleStatus, 0, sizeof(moduleStatus));
    memset(dependencies, 0xFF, sizeof(dependencies));
    memset(reverseDependencies, 0xFF, sizeof(reverseDependencies));
    memset(exports, 0xFF, sizeof(exports));
    memset(exportNameHashes, 0, sizeof(exportNameHashes));
    memset(moduleHashTable, 0xFF, sizeof(moduleHashTable));
    memset(exportHashTable, 0xFF, sizeof(exportHashTable));
    memset(hashChainStorage, 0xFF, sizeof(hashChainStorage));

    moduleCount = 0;
    exportCount = 0;
    dependencyCount = 0;
    hashChainUsed = 0;
    cacheGeneration = 0;
}



u32 ModuleCache::HashBuffer(const void* buffer, u32 size) {
    return Utils::HashBytes(buffer, size);
}

u32 ModuleCache::AddModule(const char* name, const char* source, u32 sourceLen) {
    if (moduleCount >= MAX_MODULES) return 0xFFFFFFFF;
    if (sourceCodeBufferUsed + sourceLen > sourceCodeBufferSize) return 0xFFFFFFFF;
    
    u32 nameHash = Utils::HashStr(name);
    u32 slot = nameHash & (HASH_TABLE_SIZE - 1);
    
    // Check if already exists
    u32 existing = FindModuleByHash(nameHash);
    if (existing != 0xFFFFFFFF) {
        // Update existing module
        ModuleID& mod = modules[existing];
        
        // Check if source changed
        u32 newSourceHash = HashBuffer(source, sourceLen);
        if (mod.sourceHash != newSourceHash) {
            // Invalidate module and dependents
            InvalidateDependents(existing);
            
            // Update source
            mod.sourceHash = newSourceHash;
            mod.sourceLength = sourceLen;
            mod.lastModified = std::chrono::system_clock::now().time_since_epoch().count();
            mod.status = 1; // Parsing
            
            // Copy new source
            memcpy(sourceCodeBuffer + mod.sourceOffset, source, sourceLen);
            
            cacheGeneration++;
        }
        
        return existing;
    }

    u32 chainIndex = INVALID_INDEX;
    if (moduleHashTable[slot].moduleIndex != INVALID_INDEX) {
        chainIndex = AllocateHashChain();
        if (chainIndex == INVALID_INDEX) return INVALID_INDEX;
    }
    
    // Add new module
    u32 index = moduleCount++;
    ModuleID& mod = modules[index];
    
    mod.nameHash = nameHash;
    mod.sourceHash = HashBuffer(source, sourceLen);
    mod.astRootIndex = 0xFFFFFFFF;
    mod.dependencyStart = dependencyCount;
    mod.dependencyCount = 0;
    mod.exportCount = 0;
    mod.exportStart = exportCount;
    mod.sourceOffset = sourceCodeBufferUsed;
    mod.sourceLength = sourceLen;
    mod.lastModified = std::chrono::system_clock::now().time_since_epoch().count();
    mod.compiledDataOffset = 0;
    mod.compiledDataSize = 0;
    mod.status = 1; // Parsing
    
    // Copy source
    memcpy(sourceCodeBuffer + sourceCodeBufferUsed, source, sourceLen);
    sourceCodeBufferUsed += sourceLen;
    
    // Update hash tables
    moduleNameHashes[index] = nameHash;
    moduleStatus[index] = 1;
    
    // Add to hash table
    if (moduleHashTable[slot].moduleIndex == 0xFFFFFFFF) {
        moduleHashTable[slot].moduleIndex = index;
        moduleHashTable[slot].nextIndex = 0xFFFFFFFF;
    } else {
        // Handle collision
        hashChainStorage[chainIndex].moduleIndex = index;
        hashChainStorage[chainIndex].nextIndex = INVALID_INDEX;

        u32 currentChain = moduleHashTable[slot].nextIndex;
        if (currentChain == INVALID_INDEX) {
            moduleHashTable[slot].nextIndex = chainIndex;
        } else {
            while (hashChainStorage[currentChain].nextIndex != INVALID_INDEX) {
                currentChain = hashChainStorage[currentChain].nextIndex;
            }
            hashChainStorage[currentChain].nextIndex = chainIndex;
        }
    }
    
    // Copy name
    strncpy(moduleNames[index], name, 63);
    moduleNames[index][63] = '\0';
    
    cacheGeneration++;
    return index;
}

u32 ModuleCache::FindModule(const char* name) const {
    return FindModuleByHash(Utils::HashStr(name));
}

void ModuleCache::AddDependency(u32 moduleIndex, u32 dependsOnIndex) {
    if (moduleIndex >= moduleCount || dependsOnIndex >= moduleCount) return;
    if (dependencyCount >= MAX_DEPENDENCIES) return;
    
    // Check for circular dependency
    if (CheckCircularDependency(moduleIndex, dependsOnIndex)) return;
    
    ModuleID& mod = modules[moduleIndex];
    
    // Add to dependency list
    u32 depIndex = dependencyCount++;
    dependencies[depIndex].moduleIndex = dependsOnIndex;
    dependencies[depIndex].lastKnownHash = modules[dependsOnIndex].sourceHash;
    
    // Update module dependency range
    if (mod.dependencyCount == 0) {
        mod.dependencyStart = depIndex;
    }
    mod.dependencyCount++;
    
    // Add reverse dependency for invalidation
    reverseDependencies[depIndex] = moduleIndex;
    
    cacheGeneration++;
}

void ModuleCache::InvalidateDependents(u32 moduleIndex) {
    if (moduleIndex >= moduleCount) return;
    
    // Mark module as needing recompilation
    moduleStatus[moduleIndex] = 1; // Parsing
    modules[moduleIndex].status = 1;
    
    // Find all modules that depend on this one
    for (u32 i = 0; i < dependencyCount; i++) {
        if (dependencies[i].moduleIndex == moduleIndex) {
            u32 dependent = reverseDependencies[i];
            if (dependent != 0xFFFFFFFF && dependent != moduleIndex) {
                // Recursively invalidate
                InvalidateDependents(dependent);
            }
        }
    }
    
    cacheGeneration++;
}

bool ModuleCache::CheckCircularDependency(u32 moduleA, u32 moduleB) const {
    // Simple DFS to check for cycles
    if (moduleA == moduleB) return true;
    
    const ModuleID& modB = modules[moduleB];
    for (u32 i = 0; i < modB.dependencyCount; i++) {
        u32 depIndex = modB.dependencyStart + i;
        u32 depModule = dependencies[depIndex].moduleIndex;
        if (depModule == moduleA) return true;
        if (CheckCircularDependency(moduleA, depModule)) return true;
    }
    
    return false;
}

u32 ModuleCache::AddExport(u32 moduleIndex, const char* symbolName, u8 type, u32 astNode) {
    if (moduleIndex >= moduleCount) return 0xFFFFFFFF;
    if (exportCount >= MAX_EXPORTS) return 0xFFFFFFFF;
    
    u32 nameHash = Utils::HashStr(symbolName);
    u32 slot = nameHash & (HASH_TABLE_SIZE - 1);
    u32 chainIndex = INVALID_INDEX;
    if (exportHashTable[slot].moduleIndex != INVALID_INDEX) {
        chainIndex = AllocateHashChain();
        if (chainIndex == INVALID_INDEX) return INVALID_INDEX;
    }

    u32 index = exportCount++;
    
    ExportInfo& exp = exports[index];
    exp.nameHash = nameHash;
    exp.moduleIndex = moduleIndex;
    exp.astNodeIndex = astNode;
    exp.exportType = type;
    
    exportNameHashes[index] = nameHash;
    
    // Update module export range
    ModuleID& mod = modules[moduleIndex];
    if (mod.exportCount == 0) {
        mod.exportStart = index;
    }
    mod.exportCount++;
    
    // Add to export hash table
    if (exportHashTable[slot].moduleIndex == 0xFFFFFFFF) {
        exportHashTable[slot].moduleIndex = index;
        exportHashTable[slot].nextIndex = 0xFFFFFFFF;
    } else {
        // Handle collision
        hashChainStorage[chainIndex].moduleIndex = index;
        hashChainStorage[chainIndex].nextIndex = INVALID_INDEX;

        u32 currentChain = exportHashTable[slot].nextIndex;
        if (currentChain == INVALID_INDEX) {
            exportHashTable[slot].nextIndex = chainIndex;
        } else {
            while (hashChainStorage[currentChain].nextIndex != INVALID_INDEX) {
                currentChain = hashChainStorage[currentChain].nextIndex;
            }
            hashChainStorage[currentChain].nextIndex = chainIndex;
        }
    }
    
    cacheGeneration++;
    return index;
}

u32 ModuleCache::FindExport(const char* symbolName) const {
    u32 nameHash = Utils::HashStr(symbolName);
    u32 slot = nameHash & (HASH_TABLE_SIZE - 1);

    u32 index = exportHashTable[slot].moduleIndex;
    if (index != INVALID_INDEX && index < exportCount &&
        exportNameHashes[index] == nameHash) {
        u32 moduleIndex = exports[index].moduleIndex;
        if (moduleIndex < moduleCount && modules[moduleIndex].status != 0 &&
            modules[moduleIndex].nameHash != 0) {
            return index;
        }
    }

    u32 chainIndex = exportHashTable[slot].nextIndex;
    while (chainIndex != INVALID_INDEX && chainIndex < hashChainUsed) {
        index = hashChainStorage[chainIndex].moduleIndex;
        if (index != INVALID_INDEX && index < exportCount &&
            exportNameHashes[index] == nameHash) {
            u32 moduleIndex = exports[index].moduleIndex;
            if (moduleIndex < moduleCount && modules[moduleIndex].status != 0 &&
                modules[moduleIndex].nameHash != 0) {
                return index;
            }
        }
        chainIndex = hashChainStorage[chainIndex].nextIndex;
    }

    return INVALID_INDEX;
}

void ModuleCache::GetModuleExports(u32 moduleIndex, ExportInfo* outExports, u32* outCount) const {
    if (moduleIndex >= moduleCount) {
        *outCount = 0;
        return;
    }
    
    const ModuleID& mod = modules[moduleIndex];
    *outCount = mod.exportCount;
    
    if (mod.exportCount > 0) {
        memcpy(outExports, &exports[mod.exportStart], mod.exportCount * sizeof(ExportInfo));
    }
}

void ModuleCache::MarkModuleCompiled(u32 moduleIndex, const char* metalCode, u32 codeSize) {
    if (moduleIndex >= moduleCount) return;
    if (compiledCodeBufferUsed + codeSize > compiledCodeBufferSize) return;
    
    ModuleID& mod = modules[moduleIndex];
    
    // Store compiled code
    mod.compiledDataOffset = compiledCodeBufferUsed;
    mod.compiledDataSize = codeSize;
    memcpy(compiledCodeBuffer + compiledCodeBufferUsed, metalCode, codeSize);
    compiledCodeBufferUsed += codeSize;
    
    // Update status
    mod.status = 2; // Compiled
    moduleStatus[moduleIndex] = 2;
    
    cacheGeneration++;
}

void ModuleCache::MarkModuleError(u32 moduleIndex) {
    if (moduleIndex >= moduleCount) return;
    
    modules[moduleIndex].status = 3; // Error
    moduleStatus[moduleIndex] = 3;
    
    cacheGeneration++;
}

bool ModuleCache::NeedsRecompilation(u32 moduleIndex) const {
    if (moduleIndex >= moduleCount) return false;
    
    const ModuleID& mod = modules[moduleIndex];
    
    // Check if not compiled or has error
    if (mod.status != 2) return true;
    
    // Check if any dependencies changed
    for (u32 i = 0; i < mod.dependencyCount; i++) {
        u32 depIndex = mod.dependencyStart + i;
        const DependencyInfo& dep = dependencies[depIndex];
        
        if (dep.moduleIndex < moduleCount) {
            const ModuleID& depMod = modules[dep.moduleIndex];
            if (depMod.sourceHash != dep.lastKnownHash) {
                return true;
            }
            if (depMod.status != 2) {
                return true;
            }
        }
    }
    
    return false;
}

u32 ModuleCache::GetModuleASTRef(u32 moduleIndex) const {
    if (moduleIndex >= moduleCount) return INVALID_INDEX;

    u32 astIndex = modules[moduleIndex].astRootIndex;
    if (astIndex == INVALID_INDEX || astIndex >= astNodeRefsUsed) return INVALID_INDEX;

    return astNodeRefs[astIndex];
}

void ModuleCache::InvalidateAll() {
    for (u32 i = 0; i < moduleCount; i++) {
        modules[i].status = 1; // Parsing
        moduleStatus[i] = 1;
    }
    cacheGeneration++;
}

void ModuleCache::CompactCache() {
    // TODO: Implement defragmentation
    // This would reorganize arrays to remove gaps and improve cache locality
}

void ModuleCache::SerializeCache(void* buffer, u32* size) const {
    // Calculate required size
    u32 requiredSize = sizeof(u32) * 4; // Header
    requiredSize += sizeof(ModuleID) * moduleCount;
    requiredSize += sizeof(DependencyInfo) * dependencyCount;
    requiredSize += sizeof(ExportInfo) * exportCount;
    requiredSize += sourceCodeBufferUsed;
    requiredSize += compiledCodeBufferUsed;
    requiredSize += sizeof(u32) * astNodeRefsUsed;

    if (*size < requiredSize) {
        *size = requiredSize;
        return;
    }

    u8* ptr = (u8*)buffer;

    // Write header
    *(u32*)ptr = 0x4C535742; ptr += 4; // 'BWSL'
    *(u32*)ptr = moduleCount; ptr += 4;
    *(u32*)ptr = dependencyCount; ptr += 4;
    *(u32*)ptr = exportCount; ptr += 4;

    // Write modules
    memcpy(ptr, modules, sizeof(ModuleID) * moduleCount);
    ptr += sizeof(ModuleID) * moduleCount;

    // Write dependencies
    memcpy(ptr, dependencies, sizeof(DependencyInfo) * dependencyCount);
    ptr += sizeof(DependencyInfo) * dependencyCount;

    // Write exports
    memcpy(ptr, exports, sizeof(ExportInfo) * exportCount);
    ptr += sizeof(ExportInfo) * exportCount;

    // Write source code
    memcpy(ptr, sourceCodeBuffer, sourceCodeBufferUsed);
    ptr += sourceCodeBufferUsed;

    // Write compiled code
    memcpy(ptr, compiledCodeBuffer, compiledCodeBufferUsed);
    ptr += compiledCodeBufferUsed;

    // Write AST node refs
    memcpy(ptr, astNodeRefs, sizeof(u32) * astNodeRefsUsed);

    *size = requiredSize;
}

void ModuleCache::DeserializeCache(const void* buffer, u32 size) {
    (void)size;
    const u8* ptr = (const u8*)buffer;

    // Read header
    u32 magic = *(u32*)ptr; ptr += 4;
    if (magic != 0x4C535742) return; // Invalid magic

    u32 modCount = *(u32*)ptr; ptr += 4;
    u32 depCount = *(u32*)ptr; ptr += 4;
    u32 expCount = *(u32*)ptr; ptr += 4;

    // Read modules
    memcpy(modules, ptr, sizeof(ModuleID) * modCount);
    ptr += sizeof(ModuleID) * modCount;

    // Read dependencies
    memcpy(dependencies, ptr, sizeof(DependencyInfo) * depCount);
    ptr += sizeof(DependencyInfo) * depCount;

    // Read exports
    memcpy(exports, ptr, sizeof(ExportInfo) * expCount);
    ptr += sizeof(ExportInfo) * expCount;

    // Calculate sizes
    sourceCodeBufferUsed = 0;
    compiledCodeBufferUsed = 0;
    astNodeRefsUsed = 0;

    for (u32 i = 0; i < modCount; i++) {
        sourceCodeBufferUsed = std::max(sourceCodeBufferUsed,
            modules[i].sourceOffset + modules[i].sourceLength);
        compiledCodeBufferUsed = std::max(compiledCodeBufferUsed,
            modules[i].compiledDataOffset + modules[i].compiledDataSize);
        if (modules[i].astRootIndex != INVALID_INDEX) {
            astNodeRefsUsed = std::max(astNodeRefsUsed, modules[i].astRootIndex + 1);
        }
    }

    // Read source code
    memcpy(sourceCodeBuffer, ptr, sourceCodeBufferUsed);
    ptr += sourceCodeBufferUsed;

    // Read compiled code
    memcpy(compiledCodeBuffer, ptr, compiledCodeBufferUsed);
    ptr += compiledCodeBufferUsed;

    // Read AST node refs
    memcpy(astNodeRefs, ptr, sizeof(u32) * astNodeRefsUsed);

    // Update counts
    moduleCount = modCount;
    dependencyCount = depCount;
    exportCount = expCount;

    // Rebuild hash tables
    RebuildHashTable();

    cacheGeneration++;
}

ModuleCache::CacheStats ModuleCache::GetStatistics() const {
    CacheStats stats = {};

    stats.totalModules = moduleCount;
    stats.totalExports = exportCount;
    stats.totalDependencies = dependencyCount;
    stats.sourceMemoryUsed = sourceCodeBufferUsed;
    stats.compiledMemoryUsed = compiledCodeBufferUsed;
    stats.astMemoryUsed = astNodeRefsUsed * sizeof(u32);
    
    u32 compiledCount = 0;
    u32 errorCount = 0;
    for (u32 i = 0; i < moduleCount; i++) {
        if (moduleStatus[i] == 2) compiledCount++;
        else if (moduleStatus[i] == 3) errorCount++;
    }
    
    stats.compiledModules = compiledCount;
    stats.errorModules = errorCount;
    
    // Calculate hash table load factor
    u32 usedSlots = 0;
    for (u32 i = 0; i < HASH_TABLE_SIZE; i++) {
        if (moduleHashTable[i].moduleIndex != 0xFFFFFFFF) {
            usedSlots++;
        }
    }
    stats.hashTableLoadFactor = (f32)usedSlots / HASH_TABLE_SIZE;
    
    return stats;
}

u32 ModuleCache::AllocateHashChain() {
    if (hashChainUsed >= HASH_CHAIN_CAPACITY) {
        // TODO: Handle overflow
        return INVALID_INDEX;
    }
    return hashChainUsed++;
}

bool ModuleCache::RemoveModule(u32 moduleIndex) {
    if (moduleIndex >= moduleCount || moduleIndex == INVALID_INDEX) return false;

    // Invalidate dependents before removal
    InvalidateDependents(moduleIndex);

    ModuleID& mod = modules[moduleIndex];
    mod.status = 0;
    mod.nameHash = 0;
    mod.sourceHash = 0;
    mod.exportCount = 0;
    mod.dependencyCount = 0;
    moduleNameHashes[moduleIndex] = 0;
    moduleStatus[moduleIndex] = 0;
    moduleNames[moduleIndex][0] = '\0';

    RebuildHashTable();

    cacheGeneration++;
    return true;
}

void ModuleCache::RebuildHashTable() {
    // Clear hash tables
    memset(moduleHashTable, 0xFF, sizeof(moduleHashTable));
    memset(exportHashTable, 0xFF, sizeof(exportHashTable));
    memset(hashChainStorage, 0xFF, sizeof(hashChainStorage));
    hashChainUsed = 0;
    
    // Rebuild module hash table
    for (u32 i = 0; i < moduleCount; i++) {
        u32 nameHash = modules[i].nameHash;
        if (modules[i].status == 0 || nameHash == 0) continue;
        u32 slot = nameHash & (HASH_TABLE_SIZE - 1);
        
        if (moduleHashTable[slot].moduleIndex == 0xFFFFFFFF) {
            moduleHashTable[slot].moduleIndex = i;
            moduleHashTable[slot].nextIndex = 0xFFFFFFFF;
        } else {
            u32 chainIndex = AllocateHashChain();
            if (chainIndex == INVALID_INDEX) break;
            hashChainStorage[chainIndex].moduleIndex = i;
            hashChainStorage[chainIndex].nextIndex = INVALID_INDEX;

            u32 currentChain = moduleHashTable[slot].nextIndex;
            if (currentChain == INVALID_INDEX) {
                moduleHashTable[slot].nextIndex = chainIndex;
            } else {
                while (hashChainStorage[currentChain].nextIndex != INVALID_INDEX) {
                    currentChain = hashChainStorage[currentChain].nextIndex;
                }
                hashChainStorage[currentChain].nextIndex = chainIndex;
            }
        }
        
        moduleNameHashes[i] = nameHash;
    }
    
    // Rebuild export hash table
    for (u32 i = 0; i < exportCount; i++) {
        u32 nameHash = exports[i].nameHash;
        u32 moduleIndex = exports[i].moduleIndex;
        if (nameHash == 0 || moduleIndex >= moduleCount ||
            modules[moduleIndex].status == 0 || modules[moduleIndex].nameHash == 0) {
            continue;
        }
        u32 slot = nameHash & (HASH_TABLE_SIZE - 1);
        
        if (exportHashTable[slot].moduleIndex == 0xFFFFFFFF) {
            exportHashTable[slot].moduleIndex = i;
            exportHashTable[slot].nextIndex = 0xFFFFFFFF;
        } else {
            u32 chainIndex = AllocateHashChain();
            if (chainIndex == INVALID_INDEX) break;
            hashChainStorage[chainIndex].moduleIndex = i;
            hashChainStorage[chainIndex].nextIndex = INVALID_INDEX;

            u32 currentChain = exportHashTable[slot].nextIndex;
            if (currentChain == INVALID_INDEX) {
                exportHashTable[slot].nextIndex = chainIndex;
            } else {
                while (hashChainStorage[currentChain].nextIndex != INVALID_INDEX) {
                    currentChain = hashChainStorage[currentChain].nextIndex;
                }
                hashChainStorage[currentChain].nextIndex = chainIndex;
            }
        }
        
        exportNameHashes[i] = nameHash;
    }
}

void ModuleCache::PropagateInvalidation(u32 moduleIndex) {
    (void)moduleIndex;
    // This is called internally by InvalidateDependents
    // Could be optimized with a work queue to avoid recursion
}

} // namespace BWSL
