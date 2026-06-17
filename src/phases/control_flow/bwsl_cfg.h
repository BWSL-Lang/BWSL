#pragma once
#include "core/bwsl_defs.h"
#include "core/bwsl_arena.h"
#include "phases/ir_generation/bwsl_ir_gen.h"

namespace BWSL {

struct CFG {
    // Hot data - separate arrays for cache efficiency
    alignas(64) u32* firstInst;      // [blockCount]
    alignas(64) u32* lastInst;       // [blockCount]
    alignas(64) u32* successors;     // [blockCount * 2] for non-switch blocks
    alignas(64) u8*  successorCount; // [blockCount] - 0, 1, 2, or 0xFF for switch
    
    // Switch successors (variable-length, similar to predecessors)
    alignas(64) u32* switchSuccessorOffsets;  // [blockCount + 1]
    alignas(64) u32* switchSuccessorData;     // Flattened switch successor lists
    u32 switchSuccessorDataCount;
    
    // Predecessor lists (variable-length per block)
    alignas(64) u32* predecessorOffsets;  // [blockCount + 1]
    alignas(64) u32* predecessorData;     // Flattened predecessor lists
    u32 predecessorDataCount;
    
    // Cold - structured control flow info for SPIR-V
    alignas(64) u32* mergeBlocks;    // [blockCount]
    alignas(64) u32* continueBlocks; // [blockCount]
    
    // Dominance (lazily computed)
    alignas(64) u32* immediateDominators;  // [blockCount]
    alignas(64) u32* postorderIndex;       // [blockCount] - for fast Intersect
    alignas(64) u32* postorder;            // [blockCount] - reverse postorder traversal
    bool dominatorsComputed;
    
    // Dominance Frontiers (CSR format, lazily computed)
    alignas(64) u32* dfOffsets;            // [blockCount + 1]
    alignas(64) u32* dfData;               // Flattened frontier sets
    alignas(64) u32* dfActualCounts;       // [blockCount] actual counts after dedup
    u32 dfDataCount;
    bool frontiersComputed;
    
    // Dominator tree children (for SSA rename traversal)
    alignas(64) u32* domChildOffsets;      // [blockCount + 1]
    alignas(64) u32* domChildData;         // Children in dominator tree
    u32 domChildCount;
    bool domChildrenComputed;
    
    u32 blockCount;
    u32 blockCapacity;
    u32 entryBlock;
    u32 exitBlock;
    
    // Instruction -> block mapping
    alignas(64) u32* instToBlock;  // [instructionCount]
    
    BWSL_Arena* arena;
    
    // Helpers
    inline u32 GetSuccessor(u32 block, u32 idx) const {
        return successors[block * 2 + idx];
    }
    
    inline bool IsSwitchBlock(u32 block) const {
        return successorCount[block] == 0xFF;
    }
    
    inline u32 SwitchSuccessorCount(u32 block) const {
        return switchSuccessorOffsets[block + 1] - switchSuccessorOffsets[block];
    }
    
    inline u32 GetSwitchSuccessor(u32 block, u32 idx) const {
        return switchSuccessorData[switchSuccessorOffsets[block] + idx];
    }
    
    // Get total successor count for any block type
    inline u32 TotalSuccessorCount(u32 block) const {
        if (successorCount[block] == 0xFF) {
            return SwitchSuccessorCount(block);
        }
        return successorCount[block];
    }
    
    // Get any successor by index
    inline u32 GetAnySuccessor(u32 block, u32 idx) const {
        if (successorCount[block] == 0xFF) {
            return GetSwitchSuccessor(block, idx);
        }
        return GetSuccessor(block, idx);
    }
    
    inline u32 PredecessorCount(u32 block) const {
        return predecessorOffsets[block + 1] - predecessorOffsets[block];
    }
    
    inline u32 GetPredecessor(u32 block, u32 idx) const {
        return predecessorData[predecessorOffsets[block] + idx];
    }
    
    // Dominance frontier helpers
    inline u32 DFSize(u32 block) const {
        // Use actual counts after dedup (dfActualCounts is populated during dedup)
        return dfActualCounts ? dfActualCounts[block] : (dfOffsets[block + 1] - dfOffsets[block]);
    }
    
    inline u32 GetDFMember(u32 block, u32 idx) const {
        return dfData[dfOffsets[block] + idx];
    }
    
    // Dominator tree children helpers
    inline u32 DomChildCount(u32 block) const {
        return domChildOffsets[block + 1] - domChildOffsets[block];
    }
    
    inline u32 GetDomChild(u32 block, u32 idx) const {
        return domChildData[domChildOffsets[block] + idx];
    }
};

// NO_BLOCK is defined in bwsl_ir_gen.h

struct CFGBuilder {
    IR::IRProgram* ir;
    BWSL_Arena* arena;
    CFG cfg;
    
    // Temporary workspace
    bool* isLeader;
    u32* predecessorCounts;  // Temp during build
    
    void Init(IR::IRProgram* program, BWSL_Arena* alloc);
    void Build();
    
    // Analysis passes (lazy/on-demand)
    void ComputeDominators();
    void ComputeDominanceFrontiers();
    void ComputeDominatorTreeChildren();
    void FindNaturalLoops();
    
    // Structured control flow recovery (for SPIR-V)
    void RecoverStructure();
    
private:
    void AllocateCFGArrays(u32 maxBlocks, u32 instructionCount);
    void FindLeaders();
    void AssignInstructionsToBlocks();
    void ComputeBlockBoundaries();
    void ComputeSuccessors();
    void ComputePredecessors();
    void ComputePostorder();
    
    u32 Intersect(u32 b1, u32 b2);
    void PostorderDFS(u32 block, bool* visited, u32* postorderOut, u32* indexOut);
};

} // namespace BWSL