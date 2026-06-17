#pragma once

#include "phases/ir_generation/bwsl_ir_gen.h"
#include "phases/control_flow/bwsl_cfg.h"
#include "core/bwsl_arena.h"
#include "core/bwsl_defs.h"
#include <cstring>

namespace BWSL {
namespace SSA {

//==============================================================================
// Variable Information
//==============================================================================

struct VariableInfo {
    u32* defBlocks;        // Blocks where this variable is defined
    u32 defBlockCount;
    u16 originalReg;       // Original (pre-SSA) register
    u16 type;              // CoreType
    u32 definitionCount;   // Total number of definitions (pre-SSA)
    bool hasEntryDef;      // True if variable has a definition that dominates all uses
};

//==============================================================================
// Rename State (temporary during SSA construction)
//==============================================================================

struct RenameState {
    // Per-variable register stacks - stores actual IR register IDs
    u16* stacks;           // [totalStackSlots] - actual register IDs
    u32* stackOffsets;     // [variableCount] start offset per variable
    u32* stackCaps;        // [variableCount] capacity per variable
    u32* stackDepths;      // [variableCount] current stack depth
    
    // For PHI result registers
    u16 nextNewReg;        // Next available register ID for new allocations
    
    u32 variableCount;
    BWSL_Arena* arena;
    
    void Init(u32 varCount, BWSL_Arena* alloc, const u32* perVarCaps, u16 startingReg = 1000) {
        variableCount = varCount;
        arena = alloc;
        nextNewReg = startingReg;  // Start numbering new registers after existing ones

        stackOffsets = (u32*)arena->Allocate(varCount * sizeof(u32), 64);
        stackCaps = (u32*)arena->Allocate(varCount * sizeof(u32), 64);
        stackDepths = (u32*)arena->Allocate(varCount * sizeof(u32), 64);

        u32 totalSlots = 0;
        for (u32 v = 0; v < varCount; v++) {
            u32 cap = perVarCaps ? perVarCaps[v] : 1;
            if (cap == 0) {
                cap = 1;
            }
            stackOffsets[v] = totalSlots;
            stackCaps[v] = cap;
            totalSlots += cap;
        }

        stacks = (u16*)arena->Allocate(totalSlots * sizeof(u16), 64);

        memset(stackDepths, 0, varCount * sizeof(u32));
        memset(stacks, 0xFF, totalSlots * sizeof(u16));  // Init to 0xFFFF (undefined)
    }
    
    // Get the current register ID for a variable (not version number, actual register)
    u16 GetCurrentRegister(u32 var) const {
        if (stackDepths[var] == 0) return 0xFFFF;  // Undefined
        u32 idx = stackOffsets[var] + (stackDepths[var] - 1);
        return stacks[idx];
    }
    
    // Push a register ID onto the variable's stack (for when a definition is encountered)
    void PushRegister(u32 var, u16 regId) {
        u32 depth = stackDepths[var];
        if (depth < stackCaps[var]) {
            stacks[stackOffsets[var] + depth] = regId;
            stackDepths[var] = depth + 1;
        }
    }
    
    void PopVersion(u32 var) {
        if (stackDepths[var] > 0) stackDepths[var]--;
    }
    
    u16 AllocateNewRegister() {
        return nextNewReg++;
    }
    
    // Deprecated - use GetCurrentRegister instead
    u16 GetCurrentVersion(u32 var) const { return GetCurrentRegister(var); }
    u16 PushNewVersion(u32 var) { 
        u16 newReg = AllocateNewRegister();
        PushRegister(var, newReg);
        return newReg;
    }
};

//==============================================================================
// SSA Constructor
//==============================================================================

struct SSAConstructor {
    IR::IRProgram* ir;
    CFG* cfg;
    CFGBuilder* cfgBuilder;  // Need access to compute DF
    BWSL_Arena* arena;
    
    // Variable tracking
    VariableInfo* variables;
    u32 variableCount;
    u32 variableCapacity;
    
    // Original register -> variable index mapping
    u16* regToVariable;    // [registerCount], 0xFFFF if not a variable
    
    // PHI insertion results (before renaming)
    u32* phiBlocks;        // Block indices where PHIs inserted
    u16* phiVariables;     // Variable index for each PHI
    u32 phiCount;
    u32 phiCapacity;
    
    // Worklist for IDF computation
    u32* worklist;
    bool* inWorklist;
    bool* hasPhiFor;       // [blockCount * variableCount]

    // Optional blockVisited tracker used during Rename. Non-null only
    // during the dominator-tree traversal — lets the post-Rename
    // cleanup pass distinguish reachable from unreachable blocks.
    bool* renameVisited = nullptr;
    u32 renameVisitedCapacity = 0;
    
    //==========================================================================
    // Public Interface
    //==========================================================================
    
    void Init(IR::IRProgram* program, CFG* controlFlow, CFGBuilder* builder, BWSL_Arena* alloc);
    
    // Full SSA construction pipeline
    void ConvertToSSA();
    
    // Individual phases (called by ConvertToSSA)
    void IdentifyVariables();
    void PlacePhis();
    void Rename();
    
private:
    void ComputeIDF(u32 varIdx);
    void InsertPhi(u32 block, u32 varIdx);
    void RenameBlock(u32 block, RenameState& state, u32* blockFirstPhi, u32* blockPhiCount);
};

//==============================================================================
// Convenience functions
//==============================================================================

inline void ConvertToSSA(IR::IRProgram* ir, CFG* cfg, CFGBuilder* builder, BWSL_Arena* arena) {
    SSAConstructor ssa;
    ssa.Init(ir, cfg, builder, arena);
    ssa.ConvertToSSA();
}

// Verify SSA form: check that every register is defined exactly once
// Returns true if valid, prints errors to stderr if invalid
bool VerifySSA(const IR::IRProgram* ir);

}  // namespace SSA
}  // namespace BWSL
