#include "bwsl_cast_registry.h"

namespace BWSL {
void CastRegistry::Init() {
    count = 0;
    memset(hashTable, 0xFF, sizeof(hashTable));

    enum class ScalarTag : u8 {
        NONE,
        BOOL,
        INT,
        UINT,
        FLOAT
    };

    enum class Category : u8 {
        SCALAR,
        VECTOR,
        MATRIX
    };

    struct TypeMeta {
        CoreType type;
        ScalarTag scalar;
        Category category;
        u8 components;
        bool allowVectorPromotion;
    };

    static constexpr TypeMeta TYPES[] = {
        {CoreType::BOOL,   ScalarTag::BOOL,  Category::SCALAR, 1, false},
        {CoreType::INT,    ScalarTag::INT,   Category::SCALAR, 1, true},
        {CoreType::UINT,   ScalarTag::UINT,  Category::SCALAR, 1, true},
        {CoreType::FLOAT,  ScalarTag::FLOAT, Category::SCALAR, 1, true},

        {CoreType::INT2,   ScalarTag::INT,   Category::VECTOR, 2, false},
        {CoreType::INT3,   ScalarTag::INT,   Category::VECTOR, 3, false},
        {CoreType::INT4,   ScalarTag::INT,   Category::VECTOR, 4, false},
        {CoreType::UINT2,  ScalarTag::UINT,  Category::VECTOR, 2, false},
        {CoreType::UINT3,  ScalarTag::UINT,  Category::VECTOR, 3, false},
        {CoreType::UINT4,  ScalarTag::UINT,  Category::VECTOR, 4, false},
        {CoreType::FLOAT2, ScalarTag::FLOAT, Category::VECTOR, 2, false},
        {CoreType::FLOAT3, ScalarTag::FLOAT, Category::VECTOR, 3, false},
        {CoreType::FLOAT4, ScalarTag::FLOAT, Category::VECTOR, 4, false},

        {CoreType::MAT2,   ScalarTag::FLOAT, Category::MATRIX, 2, false},
        {CoreType::MAT3,   ScalarTag::FLOAT, Category::MATRIX, 3, false},
        {CoreType::MAT4,   ScalarTag::FLOAT, Category::MATRIX, 4, false}
    };

    constexpr size_t TYPE_COUNT = sizeof(TYPES) / sizeof(TYPES[0]);

    for (size_t i = 0; i < TYPE_COUNT; ++i) {
        RegisterBaseCast(TYPES[i].type, TYPES[i].type, 0, false);
    }

    auto registerVectorConversions = [&](ScalarTag scalar) {
        for (size_t i = 0; i < TYPE_COUNT; ++i) {
            const auto& src = TYPES[i];
            if (src.scalar != scalar || src.category != Category::VECTOR) continue;

            for (size_t j = i + 1; j < TYPE_COUNT; ++j) {
                const auto& dst = TYPES[j];
                if (dst.scalar != scalar || dst.category != Category::VECTOR) continue;

                RegisterBaseCast(src.type, dst.type, 1, false);
                RegisterBaseCast(dst.type, src.type, 3, true);
            }
        }
    };

    registerVectorConversions(ScalarTag::FLOAT);
    registerVectorConversions(ScalarTag::INT);
    registerVectorConversions(ScalarTag::UINT);

    for (size_t scalarIdx = 0; scalarIdx < TYPE_COUNT; ++scalarIdx) {
        const auto& scalarType = TYPES[scalarIdx];
        if (scalarType.category != Category::SCALAR || !scalarType.allowVectorPromotion) continue;

        for (size_t vecIdx = 0; vecIdx < TYPE_COUNT; ++vecIdx) {
            const auto& vectorType = TYPES[vecIdx];
            if (vectorType.category != Category::VECTOR || vectorType.scalar != scalarType.scalar) continue;

            RegisterBaseCast(scalarType.type, vectorType.type, 1, false);
            RegisterBaseCast(vectorType.type, scalarType.type, 3, true);
        }
    }

    for (size_t i = 0; i < TYPE_COUNT; ++i) {
        const auto& src = TYPES[i];
        if (src.category != Category::MATRIX) continue;

        for (size_t j = i + 1; j < TYPE_COUNT; ++j) {
            const auto& dst = TYPES[j];
            if (dst.category != Category::MATRIX) continue;

            RegisterBaseCast(src.type, dst.type, 2, false);
            RegisterBaseCast(dst.type, src.type, 6, true);
        }
    }

    auto computeNumericCast = [](ScalarTag src, ScalarTag dst, u8& cost, bool& lossy) -> bool {
        switch (src) {
            case ScalarTag::INT:
                switch (dst) {
                    case ScalarTag::FLOAT: cost = 1; lossy = false; return true;
                    case ScalarTag::UINT:  cost = 2; lossy = true;  return true;
                    case ScalarTag::BOOL:  cost = 5; lossy = true;  return true;
                    default: return false;
                }
            case ScalarTag::UINT:
                switch (dst) {
                    case ScalarTag::FLOAT: cost = 1; lossy = false; return true;
                    case ScalarTag::INT:   cost = 2; lossy = true;  return true;
                    case ScalarTag::BOOL:  cost = 5; lossy = true;  return true;
                    default: return false;
                }
            case ScalarTag::FLOAT:
                switch (dst) {
                    case ScalarTag::INT:
                    case ScalarTag::UINT:
                    case ScalarTag::BOOL: cost = 5; lossy = true; return true;
                    default: return false;
                }
            case ScalarTag::BOOL:
                switch (dst) {
                    case ScalarTag::INT:
                    case ScalarTag::UINT:
                    case ScalarTag::FLOAT: cost = 1; lossy = false; return true;
                    default: return false;
                }
            default:
                return false;
        }
    };

    for (size_t i = 0; i < TYPE_COUNT; ++i) {
        const auto& src = TYPES[i];
        if (src.scalar == ScalarTag::NONE) continue;

        for (size_t j = 0; j < TYPE_COUNT; ++j) {
            if (i == j) continue;

            const auto& dst = TYPES[j];
            if (src.scalar == dst.scalar) continue;
            if (src.category != dst.category) continue;
            if (src.category == Category::MATRIX) continue;
            if (src.category != Category::SCALAR && src.components != dst.components) continue;

            u8 cost = 0;
            bool lossy = false;
            if (computeNumericCast(src.scalar, dst.scalar, cost, lossy)) {
                RegisterBaseCast(src.type, dst.type, cost, lossy);
            }
        }
    }
}

void CastRegistry::RegisterBaseCast(CoreType src, CoreType dst, u8 cost, bool lossy) {
    u64 key = MakeKey(src, dst, 0);
    u32 slot = HashKey(key) & (HASH_SIZE - 1);
    
    casts[count].sourceType = src;
    casts[count].targetType = dst;
    casts[count].nameHash = 0;
    casts[count].flags = CastOperation::IS_BASE_CAST;
    if (lossy) casts[count].flags |= CastOperation::LOSSY_CONVERSION;
    casts[count].cost = cost;
    casts[count].functionIndex = 0xFFFF;
    
    castKeys[count] = key;
    hashTable[slot] = count;
    count++;
}
}