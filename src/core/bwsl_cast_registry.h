#include "bwsl_types.h"
#include "bwsl_defs.h"
#include "bwsl_ast_soa.h"

namespace BWSL {

struct CastOperation {
    CoreType sourceType;
    CoreType targetType;
    u32 nameHash;              // 0 for base casts, non-zero for named extensions
    
    enum Flags : u8 {
        IS_BASE_CAST      = 0x01,  // Built-in conversion
        IS_EXTENSION      = 0x02,  // User-defined extension
        NEEDS_FUNCTION    = 0x04,  // Requires function call vs simple reinterpret
        LOSSY_CONVERSION  = 0x08,  // May lose precision
        VALIDATED         = 0x10   // Has been type-checked
    } flags;
    
    u8 cost;                   // Conversion cost (0=free, 255=expensive)
    u16 functionIndex;         // Index into cast function table if NEEDS_FUNCTION
};

// Pre-computed cast table (sorted for binary search)
struct CastRegistry {
    static constexpr u32 MAX_CASTS = 512;
    
    // Parallel arrays for cache-friendly lookups
    alignas(64) u64 castKeys[MAX_CASTS];        // (sourceType << 48) | (targetType << 32) | nameHash
    alignas(64) CastOperation casts[MAX_CASTS];
    alignas(64) u16 functionIndices[MAX_CASTS]; // AST node indices for extension functions
    u32 count;
    
    // Hash table for O(1) lookups
    static constexpr u32 HASH_SIZE = 1024;
    u16 hashTable[HASH_SIZE];
    
    void Init();
    
    // Fast lookup: inline hash → index → cast
    inline const CastOperation* Find(CoreType src, CoreType dst, u32 nameHash = 0) const {
        u64 key = MakeKey(src, dst, nameHash);
        u32 slot = HashKey(key) & (HASH_SIZE - 1);
        u16 idx = hashTable[slot];
        
        while (idx != 0xFFFF) {
            if (castKeys[idx] == key) return &casts[idx];
            idx++; // Linear probe (casts are sorted)
            if (idx >= count || (castKeys[idx] >> 32) != (key >> 32)) break;
        }
        return nullptr;
    }
    
    // Register base cast at compile time
    void RegisterBaseCast(CoreType src, CoreType dst, u8 cost, bool lossy);
    
    // Register extension cast during parsing
    u32 RegisterExtension(BWSL_Arena arena, CoreType src, CoreType dst, 
                          const ArenaString& name, NodeRef* function);
    
private:
    static inline u64 MakeKey(CoreType src, CoreType dst, u32 nameHash) {
        return (static_cast<u64>(src) << 48) | 
               (static_cast<u64>(dst) << 32) | 
               nameHash;
    }
    
    static inline u32 HashKey(u64 key) {
        // Fast hash for cast lookup
        key ^= key >> 33;
        key *= 0xff51afd7ed558ccdULL;
        key ^= key >> 33;
        return static_cast<u32>(key);
    }
};

// Global cast registry (initialized at startup)
extern CastRegistry g_castRegistry;

} // namespace BWSL