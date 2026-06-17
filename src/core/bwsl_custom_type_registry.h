
#pragma once
#include "bwsl_defs.h"
#include "bwsl_utils.h"
#include <string>
#include "bwsl_types.h"
namespace BWSL{

    struct StructData;

struct CustomTypeRegistry {
    static constexpr u32 MAX_CUSTOM_TYPES = 1024;
    static constexpr u32 HASH_TABLE_SIZE = 2048;  // Power of 2

    struct Entry {
        u32 nameHash;
        ArenaString name;  // For debugging/code generation
        StructData* structData;  // Points to symbol table data
        u32 nextIndex;  // Collision chain
    };

    Entry entries[MAX_CUSTOM_TYPES];
    u32 hashTable[HASH_TABLE_SIZE];  // Indices into entries
    u32 entryCount;

    // Constructor to ensure proper initialization
    CustomTypeRegistry() {
        Init();
    }

    void Init() {
        memset(hashTable, 0xFF, sizeof(hashTable));
        entryCount = 0;
    }
    
    // Register or find a custom type
    u32 RegisterType(const ArenaString& name, StructData* data);

    StructData* LookupType(u32 hash);
    
    const char* GetTypeName(u32 hash);
};

// Free function for resolving type info from a type string (core/custom)
TypeInfo GetTypeInfo(BWSL_Arena* arena, const std::string& typeStr);



// Global instance
static CustomTypeRegistry g_customTypes;



}
