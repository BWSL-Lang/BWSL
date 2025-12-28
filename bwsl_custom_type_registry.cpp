#include "bwsl_symbol_table.h"
#include "bwsl_custom_type_registry.h"
#include "bwsl_utils.h"
#include "bwsl_types.h"
namespace BWSL {


    // Register or find a custom type
u32 CustomTypeRegistry::RegisterType(const ArenaString& name, StructData* data) {
        u32 hash = name.nameHash;

        // Check if already exists
        u32 slot = hash & (HASH_TABLE_SIZE - 1);
        u32 idx = hashTable[slot];
        while (idx != 0xFFFFFFFF) {
            if (entries[idx].nameHash == hash) {
                return hash;  // Already registered
            }
            idx = entries[idx].nextIndex;
        }
        
        // Add new entry
        if (entryCount >= MAX_CUSTOM_TYPES) return 0;
        
        u32 newIdx = entryCount++;
        entries[newIdx].nameHash = hash;
        entries[newIdx].name = name;
        entries[newIdx].structData = data;
        entries[newIdx].nextIndex = hashTable[slot];
        hashTable[slot] = newIdx;
        
        return hash;
    }
    
StructData* CustomTypeRegistry::LookupType(u32 hash) {
        u32 slot = hash & (HASH_TABLE_SIZE - 1);
        u32 idx = hashTable[slot];
        while (idx != 0xFFFFFFFF) {
            if (entries[idx].nameHash == hash) {
                return entries[idx].structData;
            }
            idx = entries[idx].nextIndex;
        }
        return nullptr;
    }
    
const char* CustomTypeRegistry::GetTypeName(u32 hash) {
        u32 slot = hash & (HASH_TABLE_SIZE - 1);
        u32 idx = hashTable[slot];
        while (idx != 0xFFFFFFFF) {
            if (entries[idx].nameHash == hash) {
                // Return a stable string for debugging; prefer reverse lookup table
                static thread_local std::string temp;
                temp = ReverseLookup::GetString(entries[idx].name.nameHash);
                return temp.c_str();
            }
            idx = entries[idx].nextIndex;
        }
        return "unknown";
    }

 // Free function used throughout parser/codegen
 TypeInfo GetTypeInfo(BWSL_Arena* /*arena*/, const std::string& typeStr) {
    // Fast path: check pre-computed hashes
    u32 hash = Utils::HashStr(typeStr.c_str());
    
    // Check core types first (most common)
    for (u32 i = 0; i < TypeHashes::HASH_TABLE_SIZE; i++) {
        if (TypeHashes::HASH_TABLE[i].hash == hash) {
            return TypeHashes::HASH_TABLE[i].info;
        }
    }
    
    // Check custom types
    if (StructData* structData = g_customTypes.LookupType(hash)) {
        TypeInfo info{};
        info.coreType = CoreType::CUSTOM;
        info.componentCount = static_cast<u8>(structData->fields.count);
        info.arrayDimensions = 0;
        info.customTypeHash = hash;
        info.arrayLength = 0;
        info.arrayStride = 0;
        return info;
    }
    
    // Unknown type - register as custom
    TypeInfo info{};
    info.coreType = CoreType::CUSTOM;
    info.componentCount = 0;
    info.arrayDimensions = 0;
    info.customTypeHash = hash;
    info.arrayLength = 0;
    info.arrayStride = 0;
    
    // Register in global table for later resolution
    g_customTypes.RegisterType(ArenaString::MakeHashOnly(typeStr), nullptr);
    
    return info;
}
}