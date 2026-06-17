#pragma once

#include "bwsl_module_cache.h"
#include "bwsl_symbol_table.h"
#include "bwsl_parser_soa.h"  // Use SoA parser
#include "bwsl_ast_soa.h"     // For NodeRef and AST types
#include "bwsl_compiler_service.h"

namespace BWSL {

// Integration layer between module cache and existing BWSL components
class ModuleIntegration {
public:
    // ========== Parser Integration ==========
    
    // Called when parser encounters an import statement
    static bool HandleImport(ParserSoA* parser, SymbolTableData* symbolTable, const char* moduleName) {
        // Check if module exists in cache
        u32 moduleIndex = g_moduleCache.FindModule(moduleName);
        
        if (moduleIndex == 0xFFFFFFFF) {
            // Module not in cache, try to load from file
            std::string filePath = std::string("shaders/") + moduleName + ".bwsl";
            if (!LoadModuleFromFile(filePath.c_str(), moduleName)) {
                parser->ErrorAtCurrent("Module not found");
                return false;
            }
            moduleIndex = g_moduleCache.FindModule(moduleName);
        }
        
        // Check if module needs recompilation
        if (g_moduleCache.NeedsRecompilation(moduleIndex)) {
            if (!RecompileModule(moduleIndex, parser, symbolTable)) {
                parser->Error("Failed to compile imported module");
                return false;
            }
        }
        
        // Import module exports into symbol table
        ImportModuleSymbols(moduleIndex, symbolTable);
        
        // Track module dependency in cache
        if (parser->currentModuleIndex != 0xFFFFFFFF) {
            g_moduleCache.AddDependency(parser->currentModuleIndex, moduleIndex);
        }
        
        // Add to symbol table's imported modules list
        symbolTable->importedModules.Push(symbolTable->arena, moduleIndex);
        
        return true;
    }
    
    // Called when parser starts parsing a module definition
    static u32 BeginModuleParsing(ParserSoA* parser, SymbolTableData* symbolTable,
                                  const char* moduleName, const char* source, u32 sourceLen) {
        // Add or update module in cache
        u32 moduleIndex = g_moduleCache.AddModule(moduleName, source, sourceLen);

        if (moduleIndex == 0xFFFFFFFF) {
            parser->Error("Failed to add module to cache");
            return 0xFFFFFFFF;
        }

        // Update parser state
        parser->currentModuleIndex = moduleIndex;

        // Update symbol table state
        symbolTable->currentModuleIndex = moduleIndex;
        symbolTable->inModuleScope = true;

        // Enter module scope in symbol table
        SymbolTable::EnterScope(symbolTable);

        return moduleIndex;
    }
    
    // Called when parser finishes parsing a module
    static void EndModuleParsing(ParserSoA* parser, SymbolTableData* symbolTable,
                                 u32 moduleIndex, NodeRef moduleAST) {
        if (moduleIndex == 0xFFFFFFFF) return;

        // Store AST reference in cache (NodeRef is already packed u32)
        StoreModuleAST(moduleIndex, moduleAST);

        // Extract and register module exports
        ExtractModuleExports(moduleIndex, moduleAST, parser->ast, symbolTable);

        // Exit module scope
        SymbolTable::ExitScope(symbolTable);
        symbolTable->inModuleScope = false;
        symbolTable->currentModuleIndex = INVALID_INDEX;

        parser->currentModuleIndex = 0xFFFFFFFF;
    }
    
    // ========== Symbol Table Integration ==========
    
    // Import all symbols from a cached module into current symbol table
    // NOTE: For SoA AST, we need access to the AST storage to interpret NodeRefs.
    // The module cache stores NodeRef packed values, but we need the AST to access data.
    // For now, this uses a simplified approach - module exports are registered by hash
    // and resolved at evaluation time via the symbol table.
    static void ImportModuleSymbols(u32 moduleIndex, SymbolTableData* symbolTable) {
        ModuleCache::ExportInfo exports[256];
        u32 exportCount = 0;
        g_moduleCache.GetModuleExports(moduleIndex, exports, &exportCount);

        for (u32 i = 0; i < exportCount; i++) {
            const auto& exp = exports[i];

            // Get module name for qualified symbol
            const char* moduleName = g_moduleCache.moduleNames[exp.moduleIndex];

            // For SoA AST, we register exports by their qualified name hash.
            // The NodeRef (exp.astNodeIndex) points to the node in the module's AST.
            // Create a symbol entry that references the module and AST node.
            ImportExportAsSymbol(symbolTable, exp, moduleName);
        }
    }
    
    // Check if a symbol exists in any imported module by hash
    // Note: For SoA AST with hash-only ArenaStrings, we look up by nameHash.
    // The evaluator already handles module-qualified lookups via qualifiedNameHash.
    static Symbol* LookupInModulesByHash(SymbolTableData* symbolTable, u32 nameHash) {
        // First check local symbols by hash
        Symbol* sym = SymbolTable::LookupByHash(symbolTable, nameHash);
        if (sym) return sym;

        // Check imported modules' export hash tables
        for (u32 i = 0; i < symbolTable->importedModules.count; i++) {
            u32 moduleIndex = symbolTable->importedModules[i];

            // Scan exports for matching hash in this module
            for (u32 j = 0; j < g_moduleCache.exportCount; j++) {
                if (g_moduleCache.exportNameHashes[j] == nameHash &&
                    g_moduleCache.exports[j].moduleIndex == moduleIndex) {
                    // Found export - could create symbol here if needed
                    // For now, returning nullptr since evaluator handles this differently
                    return nullptr;
                }
            }
        }

        return nullptr;
    }
    
    // ========== Compiler Service Integration ==========
    
    // Get compiled Metal code for a module
    static const char* GetModuleMetalCode(u32 moduleIndex) {
        if (!g_moduleCache.IsModuleValid(moduleIndex)) {
            return nullptr;
        }
        return g_moduleCache.GetCompiledCode(moduleIndex);
    }
    
    // Called by compiler service when generating shader variants
    static void InjectModuleCode(std::stringstream& output, const char* moduleName) {
        u32 moduleIndex = g_moduleCache.FindModule(moduleName);
        if (moduleIndex == 0xFFFFFFFF) return;
        
        const char* metalCode = g_moduleCache.GetCompiledCode(moduleIndex);
        if (metalCode) {
            output << "// ========== Module: " << moduleName << " ==========\n";
            output << metalCode << "\n";
            output << "// ========== End Module ==========\n\n";
        }
    }
    
    // Register compiled module code
    static void RegisterCompiledModule(u32 moduleIndex, const char* metalCode, u32 codeSize) {
        g_moduleCache.MarkModuleCompiled(moduleIndex, metalCode, codeSize);
    }
    
    // ========== Cache Management ==========
    
    // Initialize module cache with appropriate memory
    static void Initialize() {
        // Allocate memory for module cache
        void* sourceArena = Memory::BWEMemoryManager::Allocate(
            4 * Memory::MEGABYTE, Memory::MEM_PERSISTENT);
        void* compiledArena = Memory::BWEMemoryManager::Allocate(
            8 * Memory::MEGABYTE, Memory::MEM_PERSISTENT);
        void* astArena = Memory::BWEMemoryManager::Allocate(
            2 * Memory::MEGABYTE, Memory::MEM_PERSISTENT);
        
        g_moduleCache.Initialize(sourceArena, 4 * Memory::MEGABYTE,
                                compiledArena, 8 * Memory::MEGABYTE,
                                astArena, 2 * Memory::MEGABYTE);
        
        // Load cached modules from disk if available
        LoadCacheFromDisk();
    }
    
    // Save module cache to disk for faster startup
    static void SaveCacheToDisk() {
        void* buffer = Memory::BWEMemoryManager::Allocate(
            16 * Memory::MEGABYTE, Memory::MEM_TEMPORARY);
        u32 size = 16 * Memory::MEGABYTE;
        
        g_moduleCache.SerializeCache(buffer, &size);
        
        // Write to file
        FILE* file = fopen("data/bwsl_module_cache.bin", "wb");
        if (file) {
            fwrite(buffer, 1, size, file);
            fclose(file);
        }
        
        Memory::BWEMemoryManager::Free(buffer);
    }
    
    // Load cached modules from disk
    static void LoadCacheFromDisk() {
        FILE* file = fopen("data/bwsl_module_cache.bin", "rb");
        if (!file) return;
        
        fseek(file, 0, SEEK_END);
        long fileSize = ftell(file);
        fseek(file, 0, SEEK_SET);
        
        void* buffer = Memory::BWEMemoryManager::Allocate(
            fileSize, Memory::MEM_TEMPORARY);
        
        fread(buffer, 1, fileSize, file);
        fclose(file);
        
        g_moduleCache.DeserializeCache(buffer, fileSize);
        
        Memory::BWEMemoryManager::Free(buffer);
    }
    
    // Invalidate all cached modules (for development)
    static void InvalidateAllModules() {
        g_moduleCache.InvalidateAll();
    }
    
    // Get cache statistics for debugging
    static ModuleCache::CacheStats GetCacheStatistics() {
        return g_moduleCache.GetStatistics();
    }
    
private:
    // Export type constants
    static constexpr u8 EXPORT_FUNCTION = 0;
    static constexpr u8 EXPORT_STRUCT = 1;
    static constexpr u8 EXPORT_CONST = 2;

    // Helper functions - SoA AST compatible
    static bool LoadModuleFromFile(const char* filePath, const char* moduleName);
    static bool RecompileModule(u32 moduleIndex, ParserSoA* parser, SymbolTableData* symbolTable);
    static void StoreModuleAST(u32 moduleIndex, NodeRef astRef);
    static void ExtractModuleExports(u32 moduleIndex, NodeRef astRef, AST* ast, SymbolTableData* symbolTable);
    static void ImportExportAsSymbol(SymbolTableData* symbolTable, const ModuleCache::ExportInfo& exp, const char* moduleName);
    static Symbol* CreateImportedSymbol(SymbolTableData* symbolTable, const ModuleCache::ExportInfo& exp, const ArenaString& name);
};



} // namespace BWSL