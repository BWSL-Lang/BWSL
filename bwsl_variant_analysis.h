#pragma once
#include "bwsl_types.h"
#include "bwsl_ast_soa.h"
#include "bwsl_symbol_table.h"
#include "bwsl_compiler_types.h"  // For VertexAttributeType

namespace BWSL {

enum class VariantPointType : u8 {
    OPTIONAL_ATTRIBUTE,
    BRANCH_CONDITION,
    BUFFER_GROUP_CHECK,
    RESOURCE_AVAILABILITY
};

struct VariantPoint {
    VariantPointType type;
    u32 nodeIndex;  // Index in some node array
    u32 dependencyMask;  // Bit mask of dependencies
    
    union {
        struct {
            u32 attributeIndex;
        } optionalAttr;
        
        struct {
            u32 conditionNodeIndex;
            bool canBeConstant;
        } branch;
        
        struct {
            BufferGroupLayout::GroupType requiredGroup;
        } bufferGroup;
    };
};

struct VariantAnalysisData {
    Arena* arena;
    ArenaArray<VariantPoint> variantPoints;
    ArenaArray<u32> branchNodes;  // Indices of branch nodes in AST
    u32 maxVariants;
    
    // Bit masks for quick lookups
    u32 optionalAttributeMask;
    u32 bufferGroupMask;
};

// Analysis functions
inline void InitVariantAnalysis(VariantAnalysisData* data, Arena* arena) {
    data->arena = arena;
    data->variantPoints.Init(arena, 32);
    data->branchNodes.Init(arena, 16);
    data->maxVariants = 0;
    data->optionalAttributeMask = 0;
    data->bufferGroupMask = 0;
}

// Analyze AST and populate variant data
void AnalyzeVariants(VariantAnalysisData* data, ASTNode* pipeline, const SymbolTableData* symbols);

// Calculate variant configurations needed
u32 CalculateRequiredVariants(const VariantAnalysisData* data, const BufferGroupLayout& layout);
}
