#include "bwsl_ssa.h"
#include <cstring>

namespace BWSL {
namespace SSA {

//==============================================================================
// Initialization
//==============================================================================

void SSAConstructor::Init(IR::IRProgram* program, CFG* controlFlow, CFGBuilder* builder, BWSL_Arena* alloc) {
    ir = program;
    cfg = controlFlow;
    cfgBuilder = builder;
    arena = alloc;
    
    // Ensure dominance frontiers and dominator tree children are computed
    cfgBuilder->ComputeDominanceFrontiers();
    cfgBuilder->ComputeDominatorTreeChildren();
    
    // Initialize variable tracking
    variableCapacity = 256;
    variables = (VariableInfo*)arena->Allocate(variableCapacity * sizeof(VariableInfo), 64);
    variableCount = 0;
    
    regToVariable = (u16*)arena->Allocate(ir->registerCount * sizeof(u16), 64);
    memset(regToVariable, 0xFF, ir->registerCount * sizeof(u16));  // 0xFFFF = not a variable
    
    // Initialize PHI placement storage
    phiCapacity = cfg->blockCount * 4;  // Estimate
    phiBlocks = (u32*)arena->Allocate(phiCapacity * sizeof(u32), 64);
    phiVariables = (u16*)arena->Allocate(phiCapacity * sizeof(u16), 64);
    phiCount = 0;
    
    // Initialize worklist
    worklist = (u32*)arena->Allocate(cfg->blockCount * sizeof(u32), 64);
    inWorklist = (bool*)arena->Allocate(cfg->blockCount * sizeof(bool), 64);
}

//==============================================================================
// Full SSA Conversion
//==============================================================================

void SSAConstructor::ConvertToSSA() {
    IdentifyVariables();
    PlacePhis();
    Rename();
}

//==============================================================================
// Phase 1: Identify Variables
//==============================================================================

void SSAConstructor::IdentifyVariables() {
    // Track definition sites per register
    struct RegDefs {
        u32* blocks;
        u32 blockCount;       // Number of unique blocks with definitions
        u32 definitionCount;  // Total number of definition instructions
        u32 capacity;
    };

    RegDefs* regDefs = (RegDefs*)arena->Allocate(ir->registerCount * sizeof(RegDefs), 64);
    memset(regDefs, 0, ir->registerCount * sizeof(RegDefs));
    
    // Track which registers have uses in blocks OTHER than ALL their definition blocks
    // This helps us avoid creating PHIs for pure temporaries
    bool* hasExternalUse = (bool*)arena->Allocate(ir->registerCount * sizeof(bool), 64);
    memset(hasExternalUse, 0, ir->registerCount * sizeof(bool));

    // First pass: Scan instructions for definitions
    for (u32 i = 0; i < ir->instructionCount; i++) {
        u16 op = ir->opcodes[i];

        // Skip instructions that don't define registers. OP_LOCAL_STORE /
        // OP_ARRAY_STORE / OP_STORE_OUTPUT / OP_STORE_BUFFER emit with
        // `dest` set to a *use* register (array base, value, or a dummy 0)
        // — not a new definition. Without skipping them, SSA treats the
        // dest slot as a fresh definition, which corrupts later uses of
        // that register (confirmed: OP_LOCAL_STORE with dest=0 caused
        // r0 = idx to be renamed away at resources.output[idx] writes).
        if (op == IR::OP_NOP || op == IR::OP_JUMP || op == IR::OP_RET ||
            op == IR::OP_BRANCH || op == IR::OP_SWITCH ||
            op == IR::OP_LOCAL_STORE || op == IR::OP_ARRAY_STORE ||
            op == IR::OP_STORE_OUTPUT || op == IR::OP_STORE_BUFFER) {
            continue;
        }

        u16 dest = ir->destinations[i];

        // Skip constant references (high bits set)
        if ((dest & 0x8000) != 0) continue;  // 0x8000 = float constant reference
        if ((dest & 0x4000) != 0) continue;  // 0x4000 = int constant reference
        if ((dest & 0x2000) != 0) continue;  // 0x2000 = uint constant reference
        if (dest >= ir->registerCount) continue;

        u32 block = cfg->instToBlock[i];

        // Count every definition instruction (for intra-block redefinition detection)
        regDefs[dest].definitionCount++;

        // Grow per-register definition block list if needed
        if (regDefs[dest].blockCount >= regDefs[dest].capacity) {
            u32 newCap = regDefs[dest].capacity == 0 ? 4 : regDefs[dest].capacity * 2;
            u32* newBlocks = (u32*)arena->Allocate(newCap * sizeof(u32), 64);
            if (regDefs[dest].blocks) {
                memcpy(newBlocks, regDefs[dest].blocks, regDefs[dest].blockCount * sizeof(u32));
            }
            regDefs[dest].blocks = newBlocks;
            regDefs[dest].capacity = newCap;
        }

        // Add block if not already recorded for this register
        bool found = false;
        for (u32 j = 0; j < regDefs[dest].blockCount; j++) {
            if (regDefs[dest].blocks[j] == block) {
                found = true;
                break;
            }
        }
        if (!found) {
            regDefs[dest].blocks[regDefs[dest].blockCount++] = block;
        }
    }

    // Second pass: Find registers that are used in a block that is NOT any of their
    // definition blocks. This catches the case where a variable is defined in multiple
    // blocks and used elsewhere - we need to check against ALL definition sites, not
    // just the last one encountered.
    for (u32 i = 0; i < ir->instructionCount; i++) {
        u16 op = ir->opcodes[i];
        if (op == IR::OP_NOP) continue;

        u32 useBlock = cfg->instToBlock[i];

        // Determine which operands are actual register references for this opcode
        // Some opcodes use operands as indices/slots rather than registers
        u32 firstRegOperand = 0;  // First operand that's a register
        switch (op) {
            case IR::OP_LOAD_ATTR:
            case IR::OP_LOAD_INPUT:
            case IR::OP_LOAD_CONST:
            case IR::OP_LOAD_OUTPUT:
                // operand[0] is an index/slot, not a register - no register operands
                continue;  // Skip this instruction entirely for use tracking
            case IR::OP_STORE_OUTPUT:
                // operand[0] is output slot, not a register - skip operand 0
                firstRegOperand = 1;  // But STORE_OUTPUT uses dest as the value reg, not operand
                continue;  // Actually, STORE_OUTPUT has slot in operand[0], no reg operands
            case IR::OP_BRANCH:
            case IR::OP_JUMP:
                // Branch/jump operands are labels, not registers
                // But BRANCH uses operand[0] as condition register
                firstRegOperand = 0;  // Condition is operand 0
                break;
            default:
                // Most instructions have register operands starting at index 0
                firstRegOperand = 0;
                break;
        }

        // Check operands for uses
        for (u32 opIdx = firstRegOperand; opIdx < 4; opIdx++) {
            if ((op == IR::OP_VEC_INSERT || op == IR::OP_VEC_EXTRACT || op == IR::OP_ENUM_FIELD ||
                 op == IR::OP_STRUCT_INSERT || op == IR::OP_STRUCT_EXTRACT) && opIdx == 1) {
                continue;
            }
            u16 operand = ir->GetOperand(i, opIdx);
            if (operand == 0 || operand == 0xFFFF) continue;
            if (operand & 0xE000) continue;  // Skip constants (0x8000=float, 0x4000=int, 0x2000=uint)
            if (operand >= ir->registerCount) continue;

            // Check if this use is in a block different from ALL definition blocks
            if (regDefs[operand].blockCount > 0) {
                bool usedInDefBlock = false;
                for (u32 j = 0; j < regDefs[operand].blockCount; j++) {
                    if (regDefs[operand].blocks[j] == useBlock) {
                        usedInDefBlock = true;
                        break;
                    }
                }
                if (!usedInDefBlock) {
                    hasExternalUse[operand] = true;
                }
            }
        }
    }
    
    // Create variables from registers with definitions
    for (u16 reg = 0; reg < ir->registerCount; reg++) {
        if (regDefs[reg].blockCount == 0) continue;
        if (ir->registerStorageInfo &&
            (ir->registerStorageInfo[reg] & IR::IRProgram::STORAGE_IS_PTR)) {
            continue;
        }
        // Skip address-taken variables - they live in OpVariables and don't need SSA phi nodes
        if (ir->registerStorageInfo &&
            (ir->registerStorageInfo[reg] & IR::IRProgram::STORAGE_IS_ADDRESS_TAKEN)) {
            continue;
        }

        // A register needs SSA treatment if:
        // 1. Defined more than once ANYWHERE (intra-block redefinitions violate SSA)
        // 2. Defined in multiple blocks (variable is assigned in different paths)
        // 3. Defined in a single block with non-empty DF AND has uses outside that block
        //    (the "external use" check prevents creating PHIs for pure temporaries)

        bool needsSSA = regDefs[reg].definitionCount > 1;  // Catches intra-block redefinitions

        if (!needsSSA && regDefs[reg].blockCount > 1) {
            // Multiple definition blocks (shouldn't happen if definitionCount > 1, but be safe)
            needsSSA = true;
        }

        if (!needsSSA) {
            // Check if single definition block has non-empty DF AND register is used externally
            u32 defBlock = regDefs[reg].blocks[0];
            needsSSA = (cfg->DFSize(defBlock) > 0) && hasExternalUse[reg];
        }

        if (needsSSA) {
            // Grow variable array if needed
            if (variableCount >= variableCapacity) {
                variableCapacity *= 2;
                VariableInfo* newVars = (VariableInfo*)arena->Allocate(variableCapacity * sizeof(VariableInfo), 64);
                memcpy(newVars, variables, variableCount * sizeof(VariableInfo));
                variables = newVars;
            }

            // Check if this variable has a definition in the entry block
            // If not, it means some paths may not have a definition, so we need
            // to insert an implicit undef at entry
            bool hasEntryDef = false;
            for (u32 j = 0; j < regDefs[reg].blockCount; j++) {
                if (regDefs[reg].blocks[j] == cfg->entryBlock) {
                    hasEntryDef = true;
                    break;
                }
            }

            variables[variableCount].originalReg = reg;
            variables[variableCount].type = ir->registerTypes[reg];
            variables[variableCount].defBlocks = regDefs[reg].blocks;
            variables[variableCount].defBlockCount = regDefs[reg].blockCount;
            variables[variableCount].definitionCount = regDefs[reg].definitionCount;
            variables[variableCount].hasEntryDef = hasEntryDef;

            regToVariable[reg] = static_cast<u16>(variableCount);
            variableCount++;
        }
    }
    
    // Allocate hasPhiFor bitmap: blockCount × variableCount
    if (variableCount > 0) {
        hasPhiFor = (bool*)arena->Allocate(cfg->blockCount * variableCount * sizeof(bool), 64);
        memset(hasPhiFor, 0, cfg->blockCount * variableCount * sizeof(bool));
    } else {
        hasPhiFor = nullptr;
    }
}

//==============================================================================
// Phase 2: PHI Placement
//==============================================================================

void SSAConstructor::PlacePhis() {
    // For each variable, compute IDF and insert PHIs
    for (u32 v = 0; v < variableCount; v++) {
        ComputeIDF(v);
    }
}

void SSAConstructor::ComputeIDF(u32 varIdx) {
    VariableInfo& var = variables[varIdx];
    
    // Initialize worklist with definition blocks
    u32 workHead = 0, workTail = 0;
    memset(inWorklist, 0, cfg->blockCount * sizeof(bool));
    
    for (u32 i = 0; i < var.defBlockCount; i++) {
        u32 block = var.defBlocks[i];
        worklist[workTail++] = block;
        inWorklist[block] = true;
    }
    
    // Compute iterated dominance frontier
    while (workHead < workTail) {
        u32 block = worklist[workHead++];
        
        // For each block in DF(block)
        u32 dfSize = cfg->DFSize(block);
        for (u32 di = 0; di < dfSize; di++) {
            u32 dfBlock = cfg->GetDFMember(block, di);
            
            // Insert PHI if not already present for this variable
            if (!hasPhiFor[dfBlock * variableCount + varIdx]) {
                hasPhiFor[dfBlock * variableCount + varIdx] = true;
                InsertPhi(dfBlock, varIdx);
                
                // Add to worklist (PHI is a new definition)
                if (!inWorklist[dfBlock]) {
                    worklist[workTail++] = dfBlock;
                    inWorklist[dfBlock] = true;
                }
            }
        }
    }
}

void SSAConstructor::InsertPhi(u32 block, u32 varIdx) {
    // Grow PHI arrays if needed
    if (phiCount >= phiCapacity) {
        phiCapacity *= 2;
        u32* newBlocks = (u32*)arena->Allocate(phiCapacity * sizeof(u32), 64);
        u16* newVars = (u16*)arena->Allocate(phiCapacity * sizeof(u16), 64);
        memcpy(newBlocks, phiBlocks, phiCount * sizeof(u32));
        memcpy(newVars, phiVariables, phiCount * sizeof(u16));
        phiBlocks = newBlocks;
        phiVariables = newVars;
    }
    
    phiBlocks[phiCount] = block;
    phiVariables[phiCount] = static_cast<u16>(varIdx);
    phiCount++;
}

//==============================================================================
// Phase 3: Variable Renaming
//==============================================================================

void SSAConstructor::Rename() {
    if (variableCount == 0 && phiCount == 0) return;

    RenameState state;
    u32* stackCaps = nullptr;
    if (variableCount > 0) {
        u32* phiCounts = (u32*)arena->Allocate(variableCount * sizeof(u32), 64);
        memset(phiCounts, 0, variableCount * sizeof(u32));
        for (u32 p = 0; p < phiCount; p++) {
            u16 varIdx = phiVariables[p];
            if (varIdx < variableCount) {
                phiCounts[varIdx]++;
            }
        }

        stackCaps = (u32*)arena->Allocate(variableCount * sizeof(u32), 64);
        for (u32 v = 0; v < variableCount; v++) {
            u32 cap = variables[v].definitionCount + phiCounts[v] + 1;
            if (cap < 2) {
                cap = 2;
            }
            stackCaps[v] = cap;
        }
    }

    // Start new register numbers after the existing ones
    state.Init(variableCount, arena, stackCaps, static_cast<u16>(ir->registerCount));

    // Expand registerTypes array to accommodate new SSA registers.
    // Each variable definition becomes a fresh SSA register in Rename,
    // each phi has a result register, and the post-Rename unfilled-
    // phi-operand fix-up can allocate up to one undef per phi
    // operand. The `*4` heuristic undercounted functions with many
    // inline calls to the same helper (each call creates its own
    // {returnReg, flagReg} variable with N defs), letting
    // AllocateNewRegister return indices past the capacity — those
    // writes then fall OOB and the backend reads the default
    // CoreType::FLOAT for a bool/int slot.
    u32 totalDefs = 0;
    for (u32 v = 0; v < variableCount; v++) {
        totalDefs += variables[v].definitionCount;
    }
    u32 estimatedPhiOperands = 0;
    for (u32 p = 0; p < phiCount; p++) {
        estimatedPhiOperands += cfg->PredecessorCount(phiBlocks[p]);
    }
    u32 estimatedNewRegs = totalDefs + phiCount + estimatedPhiOperands + 64;
    u32 newCapacity = ir->registerCount + estimatedNewRegs;
    u32 oldRegisterCount = ir->registerCount;
    if (ir->registerTypes) {
        u16* newTypes = (u16*)arena->Allocate(newCapacity * sizeof(u16), 64);
        memcpy(newTypes, ir->registerTypes, ir->registerCount * sizeof(u16));
        // Initialize new slots to VOID
        memset(newTypes + ir->registerCount, 0, estimatedNewRegs * sizeof(u16));
        ir->registerTypes = newTypes;
    }
    // Also expand registerStructTypes for CUSTOM types (enums, structs)
    if (ir->registerStructTypes) {
        u32* newStructTypes = (u32*)arena->Allocate(newCapacity * sizeof(u32), 64);
        memcpy(newStructTypes, ir->registerStructTypes, oldRegisterCount * sizeof(u32));
        memset(newStructTypes + oldRegisterCount, 0, estimatedNewRegs * sizeof(u32));
        ir->registerStructTypes = newStructTypes;
    }
    ir->registerCount = static_cast<u16>(newCapacity);

    // For variables that don't have a definition in the entry block, we need to
    // push an "undef" register onto their stack. This ensures that if a Phi node
    // references this variable from a path without a definition, it gets a valid
    // (but undefined) value rather than referencing a non-existent register.
    //
    // We mark these registers with a special value that the SPIR-V backend will
    // recognize and emit OpUndef for.
    for (u32 v = 0; v < variableCount; v++) {
        if (!variables[v].hasEntryDef) {
            // Allocate an "undef" register for this variable
            // We'll use the IR's undef tracking to mark this
            u16 undefReg = state.AllocateNewRegister();

            // Copy type info to the new register
            if (ir->registerTypes && undefReg < ir->registerCount) {
                ir->registerTypes[undefReg] = variables[v].type;
            }
            // Copy struct type hash for CUSTOM types (enums, structs)
            if (ir->registerStructTypes && undefReg < ir->registerCount) {
                u16 origReg = variables[v].originalReg;
                if (origReg < oldRegisterCount) {
                    ir->registerStructTypes[undefReg] = ir->registerStructTypes[origReg];
                }
            }

            // Mark this register as undef in the IR
            // We store type info so SPIR-V backend knows what type of undef to emit
            if (ir->undefRegCount < ir->undefRegCapacity) {
                ir->undefRegs[ir->undefRegCount] = undefReg;
                ir->undefRegTypes[ir->undefRegCount] = variables[v].type;
                ir->undefRegCount++;
            }

            // Push this undef register as the initial value for this variable
            state.PushRegister(v, undefReg);
        }
    }

    // Count total PHI operands to allocate IR storage
    u32 totalPhiOperands = 0;
    for (u32 p = 0; p < phiCount; p++) {
        totalPhiOperands += cfg->PredecessorCount(phiBlocks[p]);
    }
    
    // Allocate PHI storage in IR program
    ir->phiCount = phiCount;
    ir->phiCapacity = phiCount;
    ir->phiOperandCapacity = totalPhiOperands;
    
    if (phiCount > 0) {
        ir->phiBlockIndices = (u32*)arena->Allocate(phiCount * sizeof(u32), 64);
        ir->phiResultRegs = (u16*)arena->Allocate(phiCount * sizeof(u16), 64);
        ir->phiTypes = (u16*)arena->Allocate(phiCount * sizeof(u16), 64);
        ir->phiOperandOffsets = (u32*)arena->Allocate((phiCount + 1) * sizeof(u32), 64);
        
        if (totalPhiOperands > 0) {
            ir->phiOperandValues = (u16*)arena->Allocate(totalPhiOperands * sizeof(u16), 64);
            ir->phiOperandBlocks = (u32*)arena->Allocate(totalPhiOperands * sizeof(u32), 64);
        }
    }
    
    // Build block -> PHI mapping for fast lookup during rename
    u32* blockFirstPhi = (u32*)arena->Allocate(cfg->blockCount * sizeof(u32), 64);
    u32* blockPhiCount = (u32*)arena->Allocate(cfg->blockCount * sizeof(u32), 64);
    memset(blockFirstPhi, 0xFF, cfg->blockCount * sizeof(u32));
    memset(blockPhiCount, 0, cfg->blockCount * sizeof(u32));
    
    // First pass: count PHIs per block
    for (u32 p = 0; p < phiCount; p++) {
        blockPhiCount[phiBlocks[p]]++;
    }
    
    // Compute first PHI index per block
    u32 currentPhi = 0;
    for (u32 b = 0; b < cfg->blockCount; b++) {
        if (blockPhiCount[b] > 0) {
            blockFirstPhi[b] = currentPhi;
            currentPhi += blockPhiCount[b];
        }
    }
    
    // Initialize PHI metadata in IR
    u32 phiOpIdx = 0;
    for (u32 p = 0; p < phiCount; p++) {
        ir->phiBlockIndices[p] = phiBlocks[p];
        ir->phiTypes[p] = variables[phiVariables[p]].type;
        ir->phiOperandOffsets[p] = phiOpIdx;
        
        u32 predCount = cfg->PredecessorCount(phiBlocks[p]);
        
        // Initialize operands with placeholder values
        for (u32 i = 0; i < predCount; i++) {
            ir->phiOperandValues[phiOpIdx + i] = 0xFFFF;  // Will be filled during rename
            ir->phiOperandBlocks[phiOpIdx + i] = cfg->GetPredecessor(phiBlocks[p], i);
        }
        
        phiOpIdx += predCount;
    }
    if (phiCount > 0) {
        ir->phiOperandOffsets[phiCount] = phiOpIdx;
    }
    
    // Reset blockPhiCount for use as "next PHI index" during second pass
    memset(blockPhiCount, 0, cfg->blockCount * sizeof(u32));
    
    // Track which blocks got visited by the dominator-tree rename.
    // Blocks missed here are unreachable (e.g. the latch of a loop
    // whose body unconditionally `break`s) and need two kinds of
    // cleanup below:
    //   - phi operand slots from these blocks into reachable phis
    //     stay at the 0xFFFF placeholder, which has the float-const
    //     marker bit set and breaks the backend. We substitute an
    //     undef register of the variable's type.
    //   - Instructions inside the unreachable block reference raw
    //     pre-SSA operands (never renamed). The SPIR-V backend still
    //     emits these, producing "ID not defined" validator errors.
    //     NOP them out so the block is vacuous.
    bool* blockVisited = (bool*)arena->Allocate(cfg->blockCount * sizeof(bool), 64);
    memset(blockVisited, 0, cfg->blockCount * sizeof(bool));

    renameVisited = blockVisited;
    renameVisitedCapacity = cfg->blockCount;

    // Traverse dominator tree starting from entry, renaming variables
    RenameBlock(cfg->entryBlock, state, blockFirstPhi, blockPhiCount);

    renameVisited = nullptr;
    renameVisitedCapacity = 0;

    for (u32 p = 0; p < phiCount; p++) {
        u32 varIdx = phiVariables[p];
        u16 varType = variables[varIdx].type;
        u32 opStart = ir->phiOperandOffsets[p];
        u32 opEnd = ir->phiOperandOffsets[p + 1];
        for (u32 opIdx = opStart; opIdx < opEnd; opIdx++) {
            if (ir->phiOperandValues[opIdx] != 0xFFFF) continue;

            u16 undefReg = state.AllocateNewRegister();
            if (ir->registerTypes && undefReg < ir->registerCount) {
                ir->registerTypes[undefReg] = varType;
            }
            if (ir->registerStructTypes && undefReg < ir->registerCount) {
                u16 origReg = variables[varIdx].originalReg;
                if (origReg < ir->registerCount) {
                    ir->registerStructTypes[undefReg] = ir->registerStructTypes[origReg];
                }
            }
            if (ir->undefRegCount < ir->undefRegCapacity) {
                ir->undefRegs[ir->undefRegCount] = undefReg;
                ir->undefRegTypes[ir->undefRegCount] = varType;
                ir->undefRegCount++;
            }
            ir->phiOperandValues[opIdx] = undefReg;
        }
    }

    for (u32 b = 0; b < cfg->blockCount; b++) {
        if (blockVisited[b]) continue;
        u32 firstInst = cfg->firstInst[b];
        u32 lastInst = cfg->lastInst[b];
        for (u32 i = firstInst; i <= lastInst && i < ir->instructionCount; i++) {
            u16 op = ir->opcodes[i];
            // Keep block terminators — the backend still needs a valid
            // branch to wire up the unreachable-block label.
            if (op == IR::OP_JUMP || op == IR::OP_BRANCH ||
                op == IR::OP_RET || op == IR::OP_SWITCH) {
                continue;
            }
            ir->opcodes[i] = IR::OP_NOP;
            ir->destinations[i] = 0;
            for (u32 j = 0; j < 4; j++) ir->SetOperand(i, j, 0xFFFF);
        }
    }

    // Update IR register count with new SSA registers
    // Note: We're keeping original registers and adding new ones for PHI results
    // A more complete implementation would remap all registers
}

void SSAConstructor::RenameBlock(u32 block, RenameState& state,
                                  u32* blockFirstPhi, u32* blockPhiCount) {
    if (renameVisited && block < renameVisitedCapacity) {
        renameVisited[block] = true;
    }
    // Track which variables we push registers for, so we can pop them on exit
    u32 pushedCount = 0;
    u16* pushedVars = nullptr;

    u32 firstInst = cfg->firstInst[block];
    u32 lastInst = cfg->lastInst[block];
    u32 instCount = (lastInst >= firstInst) ? (lastInst - firstInst + 1) : 0;
    u32 pushedCapacity = instCount + variableCount;
    if (pushedCapacity == 0) {
        pushedCapacity = 1;
    }
    pushedVars = (u16*)arena->Allocate(pushedCapacity * sizeof(u16), 64);  // Temp storage
    
    // 1. Process PHIs at this block (they define new registers)
    if (blockFirstPhi[block] != 0xFFFFFFFF) {
        // Process each PHI that belongs to this block
        for (u32 p = 0; p < phiCount; p++) {
            if (phiBlocks[p] != block) continue;
            
            u32 varIdx = phiVariables[p];
            
            // Allocate new register for PHI result
            u16 newReg = state.AllocateNewRegister();
            ir->phiResultRegs[p] = newReg;

            // Set the type for the new PHI result register
            // Type is inherited from the variable's type
            if (newReg < ir->registerCount && ir->registerTypes) {
                ir->registerTypes[newReg] = variables[varIdx].type;
            }
            // Copy struct type hash for CUSTOM types (enums, structs)
            if (newReg < ir->registerCount && ir->registerStructTypes) {
                u16 origReg = variables[varIdx].originalReg;
                if (origReg < ir->registerCount) {
                    ir->registerStructTypes[newReg] = ir->registerStructTypes[origReg];
                }
            }

            // Push this register onto the variable's stack
            state.PushRegister(varIdx, newReg);
            if (pushedCount < pushedCapacity) {
                pushedVars[pushedCount++] = static_cast<u16>(varIdx);
            }
        }
    }
    
    // 2. Process instructions in this block
    for (u32 i = firstInst; i <= lastInst; i++) {
        u16 op = ir->opcodes[i];
        if (op == IR::OP_NOP) continue;
        
        // First: Rename USES of variables to their current version
        // This must happen BEFORE we process definitions
        
        // For STORE_OUTPUT: the value is in the destination register, not operands
        // We need to rename the destination to the current version of the variable
        if (op == IR::OP_STORE_OUTPUT) {
            u16 destReg = ir->destinations[i];
            if ((destReg & 0xE000) == 0 && destReg < ir->registerCount) {
                u16 varIdx = regToVariable[destReg];
                if (varIdx != 0xFFFF) {
                    u16 currentReg = state.GetCurrentRegister(varIdx);
                    if (currentReg != 0xFFFF && currentReg != destReg) {
                        ir->destinations[i] = currentReg;
                    }
                }
            }
            // Don't rename operands for STORE_OUTPUT (they contain slot info, not variable refs)
            continue;
        }
        
        // Skip instructions where operands are NOT variable references
        // These include memory ops with buffer/attribute indices, control flow, etc.
        bool shouldRenameOperands = false;
        if ((op >= IR::OP_FADD && op <= IR::OP_FMA) ||      // Float arithmetic
            (op >= IR::OP_IADD && op <= IR::OP_UCLAMP) ||   // Integer arithmetic
            (op >= IR::OP_AND && op <= IR::OP_PACK_SNORM4X8) || // Bitwise/packing
            (op >= IR::OP_FEQ && op <= IR::OP_UGE) ||       // Comparison
            (op >= IR::OP_SQRT && op <= IR::OP_FACEFORWARD) || // Math functions
            op == IR::OP_TANH ||                            // Non-contiguous math function
            op == IR::OP_PACK_UNORM2X16 || op == IR::OP_UNPACK_UNORM2X16 ||
            op == IR::OP_PACK_SNORM2X16 || op == IR::OP_UNPACK_SNORM2X16 ||
            op == IR::OP_UNPACK_SNORM4X8 || op == IR::OP_PACK_HALF2X16 ||
            op == IR::OP_UNPACK_HALF2X16 || op == IR::OP_ISNORMAL ||
            (op >= IR::OP_LERP && op <= IR::OP_RADIANS) ||  // Interpolation
            (op >= IR::OP_F2I && op <= IR::OP_SIGN) ||      // Type conversions
            (op >= IR::OP_ISNAN && op <= IR::OP_ISFINITE) || // Float classification
            op == IR::OP_TEX_SIZE || op == IR::OP_TEX_LEVELS || // Texture query
            op == IR::OP_VEC_CONSTRUCT ||                   // Vector construction
            op == IR::OP_VEC_EXTRACT ||                     // Vector extract
            op == IR::OP_VEC_INSERT ||                      // Vector component insert (literal index)
            op == IR::OP_VEC_INSERT_DYNAMIC ||              // Vector insert with runtime index
            op == IR::OP_VEC_SHUFFLE ||                     // Vector shuffle/swizzle
            op == IR::OP_STRUCT_EXTRACT ||                  // Struct extract
            op == IR::OP_STRUCT_INSERT ||                   // Struct insert
            op == IR::OP_SELECT ||                          // Ternary select
            op == IR::OP_BRANCH ||                          // Branch condition
            op == IR::OP_SWITCH ||                          // Switch selector
            op == IR::OP_ENUM_TAG ||                        // Enum tag extraction
            op == IR::OP_ENUM_FIELD ||                      // Enum field extraction
            op == IR::OP_ENUM_CONSTRUCT ||                  // Enum construction
            op == IR::OP_STRUCT_CONSTRUCT ||                // Struct positional constructor
            op == IR::OP_STORAGE_FIELD || op == IR::OP_STORAGE_INDEX || op == IR::OP_STORAGE_LOAD ||
            op == IR::OP_LOCAL_VAR_PTR || op == IR::OP_LOCAL_LOAD || op == IR::OP_LOCAL_STORE ||  // Pointer operations
            op == IR::OP_LOCAL_FIELD_PTR ||                                                     // Struct-field pointer
            op == IR::OP_ARRAY_LOAD || op == IR::OP_ARRAY_STORE) {  // Array operations with register indices
            shouldRenameOperands = true;
        }
        
        if (shouldRenameOperands) {
            // Rename operands for instructions that use variable values
            // IR has 4 operand slots per instruction
            for (u32 opIdx = 0; opIdx < 4; opIdx++) {
                // Special case: VEC_INSERT and VEC_EXTRACT use operand 1 as a literal
                // component index (0, 1, 2, 3), NOT a variable reference. Skip it.
                // ENUM_FIELD also uses operand 1 as a literal field index.
                if ((op == IR::OP_VEC_INSERT || op == IR::OP_VEC_EXTRACT || op == IR::OP_ENUM_FIELD ||
                     op == IR::OP_STRUCT_INSERT || op == IR::OP_STRUCT_EXTRACT ||
                     op == IR::OP_LOCAL_FIELD_PTR) && opIdx == 1) {
                    continue;
                }

                u16 opReg = ir->GetOperand(i, opIdx);
                // Skip unused operand slots (0xFFFF sentinel)
                if (opReg == 0xFFFF) continue;
                // Skip constants and invalid registers
                if (opReg & 0xE000) continue;  // Skip constants
                if (opReg >= ir->registerCount) continue;

                // Check if this operand is a tracked variable
                u16 varIdx = regToVariable[opReg];
                if (varIdx != 0xFFFF) {
                    // Get the current version of this variable
                    u16 currentReg = state.GetCurrentRegister(varIdx);
                    if (currentReg != 0xFFFF && currentReg != opReg) {
                        // Rename the operand to use the current version
                        ir->SetOperand(i, opIdx, currentReg);
                    }
                }
            }
        }
        
        // Skip instructions that don't define registers
        if (op == IR::OP_JUMP || op == IR::OP_RET || op == IR::OP_BRANCH || op == IR::OP_SWITCH) continue;

        // For definitions: allocate a new SSA register and update the instruction
        u16 dest = ir->destinations[i];
        // Skip constant references, but allow register 0
        if ((dest & 0xE000) == 0 && dest < ir->registerCount) {
            u16 varIdx = regToVariable[dest];

            // Special case: STORE_REG where dest is NOT a tracked variable
            // We still need to rename the source operand if it's a variable
            if (varIdx == 0xFFFF && op == IR::OP_STORE_REG) {
                u16 srcReg = ir->GetOperand(i, 0);
                if ((srcReg & 0xE000) == 0 && srcReg < ir->registerCount) {
                    u16 srcVarIdx = regToVariable[srcReg];
                    if (srcVarIdx != 0xFFFF) {
                        u16 currentSrc = state.GetCurrentRegister(srcVarIdx);
                        if (currentSrc != 0xFFFF && currentSrc != srcReg) {
                            ir->SetOperand(i, 0, currentSrc);
                        }
                    }
                }
            }
            if (varIdx != 0xFFFF) {
                // For STORE_REG: the VALUE is the source operand, not the destination
                // STORE_REG dst, src means "dst = src", so the value becomes the current version
                // For other instructions: allocate a NEW register for the SSA result
                u16 valueReg;


        if (op == IR::OP_STORE_REG) {
                u16 srcReg = ir->GetOperand(i, 0);
                
                // Check if source is a constant reference
                bool srcIsConstant = (srcReg & 0xE000) != 0;
                
                if (srcIsConstant) {
                    // Source is a constant - allocate a new SSA register for this definition.
                    // This is critical for phi nodes: they need a typed register reference,
                    // not a raw constant reference.
                    valueReg = state.AllocateNewRegister();
                    
                    // Update instruction destination to the new SSA register
                    ir->destinations[i] = valueReg;
                    
                    // Convert STORE_REG to LOAD_REG for proper SSA semantics.
                    // STORE_REG maps to OpStore (memory store, doesn't define a register).
                    // LOAD_REG maps to OpCopyObject (copies value, defines destination register).
                    ir->opcodes[i] = IR::OP_LOAD_REG;
                    
                    // Copy type info from the variable being assigned
                    if (valueReg < ir->registerCount && ir->registerTypes) {
                        ir->registerTypes[valueReg] = variables[varIdx].type;
                    }
                    if (valueReg < ir->registerCount && ir->registerStructTypes) {
                        u16 origReg = variables[varIdx].originalReg;
                        if (origReg < ir->registerCount) {
                            ir->registerStructTypes[valueReg] = ir->registerStructTypes[origReg];
                        }
                    }
                } else {
                    // Source is a register - rename to current SSA version if tracked
                    if (srcReg < ir->registerCount) {
                        u16 srcVarIdx = regToVariable[srcReg];
                        if (srcVarIdx != 0xFFFF) {
                            u16 currentSrc = state.GetCurrentRegister(srcVarIdx);
                            if (currentSrc != 0xFFFF) {
                                ir->SetOperand(i, 0, currentSrc);
                            }
                        }
                    }
                    valueReg = ir->GetOperand(i, 0);
                }
            }
                else {
                    // Allocate a new SSA register for this definition
                    valueReg = state.AllocateNewRegister();

                    // Update the instruction's destination to use the new register
                    ir->destinations[i] = valueReg;

                    // Copy the type from the original register
                    if (valueReg < ir->registerCount && ir->registerTypes) {
                        ir->registerTypes[valueReg] = variables[varIdx].type;
                    }
                    // Copy struct type hash for CUSTOM types (enums, structs)
                    if (valueReg < ir->registerCount && ir->registerStructTypes) {
                        u16 origReg = variables[varIdx].originalReg;
                        if (origReg < ir->registerCount) {
                            ir->registerStructTypes[valueReg] = ir->registerStructTypes[origReg];
                        }
                    }
                }

                // Push the value register onto the stack
                state.PushRegister(varIdx, valueReg);
                if (pushedCount < pushedCapacity) {
                    pushedVars[pushedCount++] = varIdx;
                }
            }
        }
    }
    
    // 3. Fill PHI operands in successors
    // For each successor, find PHIs and fill in the operand for this edge
    u32 succCount = cfg->TotalSuccessorCount(block);
    for (u32 si = 0; si < succCount; si++) {
        u32 succ = cfg->GetAnySuccessor(block, si);
        if (succ == NO_BLOCK) continue;
        
        // Find PHIs in successor block
        for (u32 p = 0; p < phiCount; p++) {
            if (phiBlocks[p] != succ) continue;
            
            u32 varIdx = phiVariables[p];
            
            // Find operand slot for edge from 'block' to 'succ'
            u32 opStart = ir->phiOperandOffsets[p];
            u32 opEnd = ir->phiOperandOffsets[p + 1];
            
            for (u32 opIdx = opStart; opIdx < opEnd; opIdx++) {
                if (ir->phiOperandBlocks[opIdx] == block) {
                    // Fill in the current register for this variable
                    u16 currentReg = state.GetCurrentRegister(varIdx);
                    if (currentReg != 0xFFFF) {
                        // Ensure we're not storing a constant reference - the PHI
                        // should use the actual register value, not constant indices
                        ir->phiOperandValues[opIdx] = currentReg;
                    } else {
                        // No value was ever defined on this path - use an undef register.
                        // This can happen when a variable is defined only in a path that
                        // doesn't flow to this block (e.g., defined in break path but
                        // we're on the continue path).
                        //
                        // Don't use originalReg here - it might only be defined in a different
                        // path and would cause a "forward reference not defined" SPIR-V error.
                        u16 undefReg = state.AllocateNewRegister();

                        // Copy type info to the new register
                        if (ir->registerTypes && undefReg < ir->registerCount) {
                            ir->registerTypes[undefReg] = variables[varIdx].type;
                        }
                        // Copy struct type hash for CUSTOM types (enums, structs)
                        if (ir->registerStructTypes && undefReg < ir->registerCount) {
                            u16 origReg = variables[varIdx].originalReg;
                            if (origReg < ir->registerCount) {
                                ir->registerStructTypes[undefReg] = ir->registerStructTypes[origReg];
                            }
                        }

                        // Mark this register as undef in the IR
                        if (ir->undefRegCount < ir->undefRegCapacity) {
                            ir->undefRegs[ir->undefRegCount] = undefReg;
                            ir->undefRegTypes[ir->undefRegCount] = variables[varIdx].type;
                            ir->undefRegCount++;
                        }

                        ir->phiOperandValues[opIdx] = undefReg;
                    }
                    break;
                }
            }
        }
    }
    
    // 4. Recurse to children in dominator tree
    u32 childCount = cfg->DomChildCount(block);
    for (u32 ci = 0; ci < childCount; ci++) {
        u32 child = cfg->GetDomChild(block, ci);
        RenameBlock(child, state, blockFirstPhi, blockPhiCount);
    }
    
    // 5. Pop registers pushed by this block
    for (u32 i = 0; i < pushedCount; i++) {
        state.PopVersion(pushedVars[i]);
    }
}

}  // namespace SSA
}  // namespace BWSL
