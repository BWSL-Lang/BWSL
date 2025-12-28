#include "bwsl_cfg.h"
#include <cstring>

namespace BWSL {

void CFGBuilder::Init(IR::IRProgram* program, BWSL_Arena* alloc) {
    ir = program;
    arena = alloc;
    
    memset(&cfg, 0, sizeof(CFG));
    cfg.arena = arena;
    cfg.dominatorsComputed = false;
}

void CFGBuilder::AllocateCFGArrays(u32 maxBlocks, u32 instructionCount) {
    cfg.blockCapacity = maxBlocks;
    
    // Hot arrays
    cfg.firstInst      = (u32*)arena->Allocate(maxBlocks * sizeof(u32), 64);
    cfg.lastInst       = (u32*)arena->Allocate(maxBlocks * sizeof(u32), 64);
    cfg.successors     = (u32*)arena->Allocate(maxBlocks * 2 * sizeof(u32), 64);
    cfg.successorCount = (u8*)arena->Allocate(maxBlocks * sizeof(u8), 64);
    
    // Switch successor offsets (data allocated after we know total count)
    cfg.switchSuccessorOffsets = (u32*)arena->Allocate((maxBlocks + 1) * sizeof(u32), 64);
    memset(cfg.switchSuccessorOffsets, 0, (maxBlocks + 1) * sizeof(u32));
    cfg.switchSuccessorDataCount = 0;
    
    // Predecessor offsets (data allocated after we know total count)
    cfg.predecessorOffsets = (u32*)arena->Allocate((maxBlocks + 1) * sizeof(u32), 64);
    
    // Cold arrays
    cfg.mergeBlocks    = (u32*)arena->Allocate(maxBlocks * sizeof(u32), 64);
    cfg.continueBlocks = (u32*)arena->Allocate(maxBlocks * sizeof(u32), 64);
    
    // Dominance (allocated but not computed yet)
    cfg.immediateDominators = (u32*)arena->Allocate(maxBlocks * sizeof(u32), 64);
    cfg.postorderIndex      = (u32*)arena->Allocate(maxBlocks * sizeof(u32), 64);
    cfg.postorder           = (u32*)arena->Allocate(maxBlocks * sizeof(u32), 64);
    
    // Instruction mapping
    cfg.instToBlock = (u32*)arena->Allocate(instructionCount * sizeof(u32), 64);
    
    // Initialize to sentinel values
    memset(cfg.successors, 0xFF, maxBlocks * 2 * sizeof(u32));
    memset(cfg.mergeBlocks, 0xFF, maxBlocks * sizeof(u32));
    memset(cfg.continueBlocks, 0xFF, maxBlocks * sizeof(u32));
    memset(cfg.immediateDominators, 0xFF, maxBlocks * sizeof(u32));
}

void CFGBuilder::Build() {
    if (ir->instructionCount == 0) {
        cfg.blockCount = 0;
        return;
    }
    
    // Workspace allocation
    isLeader = (bool*)arena->Allocate(ir->instructionCount * sizeof(bool), 64);
    memset(isLeader, 0, ir->instructionCount * sizeof(bool));
    
    // Pass 1: Find leaders
    FindLeaders();
    
    u32 blockCount = 0;
    for (u32 i = 0; i < ir->instructionCount; i++) {
        if (isLeader[i]) blockCount++;
    }
    
    // Allocate CFG arrays now that we know block count
    AllocateCFGArrays(blockCount, ir->instructionCount);
    cfg.blockCount = blockCount;
    cfg.entryBlock = 0;
    
    // Pass 2: Assign instructions to blocks
    AssignInstructionsToBlocks();
    
    // Pass 3: Compute block boundaries
    ComputeBlockBoundaries();
    
    // Pass 4: Compute successors
    ComputeSuccessors();
    
    // Pass 5: Compute predecessors
    ComputePredecessors();
    
    // Find exit block (block containing return, or last block)
    cfg.exitBlock = cfg.blockCount - 1;
    for (u32 b = 0; b < cfg.blockCount; b++) {
        u32 lastInst = cfg.lastInst[b];
        if (ir->opcodes[lastInst] == IR::OP_RET) {
            cfg.exitBlock = b;
            break;
        }
    }
}

void CFGBuilder::FindLeaders() {
    isLeader[0] = true;  // First instruction always a leader
    
    for (u32 i = 0; i < ir->instructionCount; i++) {
        u16 op = ir->opcodes[i];
        
        // Only terminators end basic blocks
        if (IR::IsTerminator(static_cast<IR::OpCode>(op))) {
            if (op == IR::OP_JUMP) {
                // Unconditional jump: target in metadata
                u32 target = ir->metadata[i];
                if (target < ir->instructionCount) {
                    isLeader[target] = true;
                }
            }
            else if (op == IR::OP_BRANCH) {
                // Conditional branch: metadata = (falseTarget << 16) | trueTarget
                u32 packed = ir->metadata[i];
                u32 trueTarget = packed & 0xFFFF;
                u32 falseTarget = packed >> 16;
                
                if (trueTarget < ir->instructionCount) {
                    isLeader[trueTarget] = true;
                }
                if (falseTarget < ir->instructionCount) {
                    isLeader[falseTarget] = true;
                }
            }
            else if (op == IR::OP_SWITCH) {
                // Switch: metadata = switch data index, targets in separate arrays
                u32 switchId = ir->metadata[i];
                if (switchId < ir->switchCount) {
                    // Mark all case targets as leaders
                    u32 caseStart = ir->switchCaseOffsets[switchId];
                    u32 caseEnd = ir->switchCaseOffsets[switchId + 1];
                    for (u32 c = caseStart; c < caseEnd; c++) {
                        u32 target = ir->switchCaseTargets[c];
                        if (target < ir->instructionCount) {
                            isLeader[target] = true;
                        }
                    }
                    // Mark default target as leader
                    u32 defaultTarget = ir->switchDefaultTargets[switchId];
                    if (defaultTarget < ir->instructionCount) {
                        isLeader[defaultTarget] = true;
                    }
                }
            }
            
            // Instruction after any terminator (except RET/DISCARD) could be a target
            // Mark as leader to be safe - blocks will merge if not actually targeted
            if (i + 1 < ir->instructionCount && op != IR::OP_RET && op != IR::OP_DISCARD) {
                isLeader[i + 1] = true;
            }
        }
    }

    // Mark continue targets as leaders - SPIR-V requires continue blocks to be
    // separate basic blocks for structured control flow
    if (ir->continueInfo) {
        for (u32 i = 0; i < ir->instructionCount; i++) {
            u32 continueTarget = ir->continueInfo[i];
            if (continueTarget != 0xFFFFFFFF && continueTarget < ir->instructionCount) {
                isLeader[continueTarget] = true;
            }
        }
    }
}

void CFGBuilder::AssignInstructionsToBlocks() {
    u32 currentBlock = 0;
    
    for (u32 i = 0; i < ir->instructionCount; i++) {
        if (isLeader[i] && i > 0) {
            currentBlock++;
        }
        cfg.instToBlock[i] = currentBlock;
    }
}

void CFGBuilder::ComputeBlockBoundaries() {
    u32 currentBlock = 0;
    cfg.firstInst[0] = 0;
    
    for (u32 i = 0; i < ir->instructionCount; i++) {
        if (isLeader[i] && i > 0) {
            // End previous block
            cfg.lastInst[currentBlock] = i - 1;
            currentBlock++;
            // Start new block
            cfg.firstInst[currentBlock] = i;
        }
    }
    
    // Close final block
    cfg.lastInst[currentBlock] = ir->instructionCount - 1;
}

void CFGBuilder::ComputeSuccessors() {
    // First pass: count switch successors to allocate storage
    u32 totalSwitchSuccessors = 0;
    for (u32 b = 0; b < cfg.blockCount; b++) {
        u32 lastInst = cfg.lastInst[b];
        u16 op = ir->opcodes[lastInst];
        
        if (op == IR::OP_SWITCH) {
            u32 switchId = ir->metadata[lastInst];
            if (switchId < ir->switchCount) {
                u32 caseCount = ir->switchCaseOffsets[switchId + 1] - ir->switchCaseOffsets[switchId];
                totalSwitchSuccessors += caseCount + 1;  // cases + default
            }
        }
    }
    
    // Allocate switch successor data if needed
    if (totalSwitchSuccessors > 0) {
        cfg.switchSuccessorData = (u32*)arena->Allocate(totalSwitchSuccessors * sizeof(u32), 64);
        cfg.switchSuccessorDataCount = totalSwitchSuccessors;
    }
    
    // Second pass: fill in successors
    u32 switchDataIdx = 0;
    for (u32 b = 0; b < cfg.blockCount; b++) {
        u32 lastInst = cfg.lastInst[b];
        u16 op = ir->opcodes[lastInst];
        
        cfg.successorCount[b] = 0;
        cfg.successors[b * 2] = NO_BLOCK;
        cfg.successors[b * 2 + 1] = NO_BLOCK;
        cfg.switchSuccessorOffsets[b] = switchDataIdx;
        
        if (op == IR::OP_RET || op == IR::OP_DISCARD) {
            // No successors - these instructions terminate execution
            cfg.successorCount[b] = 0;
        }
        else if (op == IR::OP_JUMP) {
            // Unconditional jump: target in metadata
            u32 target = ir->metadata[lastInst];
            if (target < ir->instructionCount) {
                cfg.successors[b * 2] = cfg.instToBlock[target];
                cfg.successorCount[b] = 1;
            }
        }
        else if (op == IR::OP_BRANCH) {
            // Conditional: metadata = (falseTarget << 16) | trueTarget
            u32 packed = ir->metadata[lastInst];
            u32 trueTarget = packed & 0xFFFF;
            u32 falseTarget = packed >> 16;
            
            // Store true branch first, then false branch
            if (trueTarget < ir->instructionCount) {
                cfg.successors[b * 2] = cfg.instToBlock[trueTarget];
                cfg.successorCount[b] = 1;
            }
            if (falseTarget < ir->instructionCount) {
                cfg.successors[b * 2 + 1] = cfg.instToBlock[falseTarget];
                cfg.successorCount[b] = 2;
            }
        }
        else if (op == IR::OP_SWITCH) {
            // Switch: use special 0xFF marker, store all targets in switch arrays
            cfg.successorCount[b] = 0xFF;  // Special marker for switch block
            
            u32 switchId = ir->metadata[lastInst];
            if (switchId < ir->switchCount) {
                u32 caseStart = ir->switchCaseOffsets[switchId];
                u32 caseEnd = ir->switchCaseOffsets[switchId + 1];
                
                // Store all case targets
                for (u32 c = caseStart; c < caseEnd; c++) {
                    u32 target = ir->switchCaseTargets[c];
                    if (target < ir->instructionCount) {
                        cfg.switchSuccessorData[switchDataIdx++] = cfg.instToBlock[target];
                    }
                }
                
                // Store default target
                u32 defaultTarget = ir->switchDefaultTargets[switchId];
                if (defaultTarget < ir->instructionCount) {
                    cfg.switchSuccessorData[switchDataIdx++] = cfg.instToBlock[defaultTarget];
                }
            }
        }
        else {
            // Non-terminator at end of block = fallthrough
            if (b + 1 < cfg.blockCount) {
                cfg.successors[b * 2] = b + 1;
                cfg.successorCount[b] = 1;
            }
        }
    }
    
    // Final offset for last block
    cfg.switchSuccessorOffsets[cfg.blockCount] = switchDataIdx;
}

void CFGBuilder::ComputePredecessors() {
    // Temporary: count predecessors per block
    predecessorCounts = (u32*)arena->Allocate(cfg.blockCount * sizeof(u32), 64);
    memset(predecessorCounts, 0, cfg.blockCount * sizeof(u32));
    
    // Count predecessors (use TotalSuccessorCount to handle both regular and switch blocks)
    for (u32 b = 0; b < cfg.blockCount; b++) {
        u32 succCount = cfg.TotalSuccessorCount(b);
        for (u32 s = 0; s < succCount; s++) {
            u32 succ = cfg.GetAnySuccessor(b, s);
            if (succ != NO_BLOCK) {
                predecessorCounts[succ]++;
            }
        }
    }
    
    // Build offset array (prefix sum)
    cfg.predecessorOffsets[0] = 0;
    for (u32 b = 0; b < cfg.blockCount; b++) {
        cfg.predecessorOffsets[b + 1] = cfg.predecessorOffsets[b] + predecessorCounts[b];
    }
    
    // Allocate predecessor data
    u32 totalPredecessors = cfg.predecessorOffsets[cfg.blockCount];
    cfg.predecessorData = (u32*)arena->Allocate(totalPredecessors * sizeof(u32), 64);
    cfg.predecessorDataCount = totalPredecessors;
    
    // Reset counts for fill pass
    memset(predecessorCounts, 0, cfg.blockCount * sizeof(u32));
    
    // Fill predecessor data (use TotalSuccessorCount/GetAnySuccessor for all block types)
    for (u32 b = 0; b < cfg.blockCount; b++) {
        u32 succCount = cfg.TotalSuccessorCount(b);
        for (u32 s = 0; s < succCount; s++) {
            u32 succ = cfg.GetAnySuccessor(b, s);
            if (succ != NO_BLOCK) {
                u32 offset = cfg.predecessorOffsets[succ] + predecessorCounts[succ];
                cfg.predecessorData[offset] = b;
                predecessorCounts[succ]++;
            }
        }
    }
}

//==========================================================================
// Dominator Computation (Cooper-Harvey-Kennedy)
//==========================================================================

void CFGBuilder::PostorderDFS(u32 block, bool* visited, u32* postorderOut, u32* indexOut) {
    visited[block] = true;
    
    // Visit successors first (use helpers to handle both regular and switch blocks)
    u32 succCount = cfg.TotalSuccessorCount(block);
    for (u32 s = 0; s < succCount; s++) {
        u32 succ = cfg.GetAnySuccessor(block, s);
        if (succ != NO_BLOCK && !visited[succ]) {
            PostorderDFS(succ, visited, postorderOut, indexOut);
        }
    }
    
    // Add to postorder after children
    cfg.postorder[*indexOut] = block;
    cfg.postorderIndex[block] = *indexOut;
    (*indexOut)++;
}

void CFGBuilder::ComputePostorder() {
    bool* visited = (bool*)arena->Allocate(cfg.blockCount * sizeof(bool), 64);
    memset(visited, 0, cfg.blockCount * sizeof(bool));
    
    u32 index = 0;
    PostorderDFS(cfg.entryBlock, visited, cfg.postorder, &index);
    
    // Handle unreachable blocks (shouldn't exist in well-formed IR)
    for (u32 b = 0; b < cfg.blockCount; b++) {
        if (!visited[b]) {
            cfg.postorder[index] = b;
            cfg.postorderIndex[b] = index;
            index++;
        }
    }
}
    
u32 CFGBuilder::Intersect(u32 b1, u32 b2) {
    // Walk up dominator tree until we find common ancestor
    while (b1 != b2) {
        // The node with smaller postorder number is "higher" in the tree
        while (cfg.postorderIndex[b1] < cfg.postorderIndex[b2]) {
            b1 = cfg.immediateDominators[b1];
        }
        while (cfg.postorderIndex[b2] < cfg.postorderIndex[b1]) {
            b2 = cfg.immediateDominators[b2];
        }
    }
    return b1;
}
void CFGBuilder::ComputeDominators() {
    if (cfg.dominatorsComputed) return;
    
    // First compute reverse postorder
    ComputePostorder();
    
    // Initialize
    for (u32 b = 0; b < cfg.blockCount; b++) {
        cfg.immediateDominators[b] = NO_BLOCK;
    }
    cfg.immediateDominators[cfg.entryBlock] = cfg.entryBlock;
    
    bool changed = true;
    while (changed) {
        changed = false;
        
        // Process in reverse postorder (skip entry)
        for (s32 i = cfg.blockCount - 1; i >= 0; i--) {
            u32 b = cfg.postorder[i];
            if (b == cfg.entryBlock) continue;
            
            u32 newIdom = NO_BLOCK;
            
            // Find first processed predecessor
            u32 predStart = cfg.predecessorOffsets[b];
            u32 predEnd = cfg.predecessorOffsets[b + 1];
            
            for (u32 pi = predStart; pi < predEnd; pi++) {
                u32 pred = cfg.predecessorData[pi];
                
                if (cfg.immediateDominators[pred] != NO_BLOCK) {
                    if (newIdom == NO_BLOCK) {
                        newIdom = pred;
                    } else {
                        newIdom = Intersect(newIdom, pred);
                    }
                }
            }
            
            if (newIdom != cfg.immediateDominators[b]) {
                cfg.immediateDominators[b] = newIdom;
                changed = true;
            }
        }
    }
    
    cfg.dominatorsComputed = true;
}

void CFGBuilder::ComputeDominanceFrontiers() {
    if (cfg.frontiersComputed) return;
    if (!cfg.dominatorsComputed) ComputeDominators();
    
    // Step 1: Count frontier sizes (first pass)
    u32* frontierCounts = (u32*)arena->Allocate(cfg.blockCount * sizeof(u32), 64);
    memset(frontierCounts, 0, cfg.blockCount * sizeof(u32));
    
    // For each join point (block with >= 2 predecessors), walk up to idom
    for (u32 b = 0; b < cfg.blockCount; b++) {
        u32 predCount = cfg.PredecessorCount(b);
        if (predCount < 2) continue;  // Not a join point
        
        for (u32 pi = 0; pi < predCount; pi++) {
            u32 pred = cfg.GetPredecessor(b, pi);
            u32 runner = pred;
            
            // Walk up dominator tree from pred until we reach idom(b)
            while (runner != NO_BLOCK && runner != cfg.immediateDominators[b]) {
                frontierCounts[runner]++;
                runner = cfg.immediateDominators[runner];
            }
        }
    }
    
    // Step 2: Build offset array (prefix sum)
    cfg.dfOffsets = (u32*)arena->Allocate((cfg.blockCount + 1) * sizeof(u32), 64);
    cfg.dfOffsets[0] = 0;
    for (u32 b = 0; b < cfg.blockCount; b++) {
        cfg.dfOffsets[b + 1] = cfg.dfOffsets[b] + frontierCounts[b];
    }
    cfg.dfDataCount = cfg.dfOffsets[cfg.blockCount];
    
    // Step 3: Allocate and fill frontier data
    if (cfg.dfDataCount > 0) {
        cfg.dfData = (u32*)arena->Allocate(cfg.dfDataCount * sizeof(u32), 64);
    } else {
        cfg.dfData = nullptr;
    }
    memset(frontierCounts, 0, cfg.blockCount * sizeof(u32));  // Reset for fill pass
    
    for (u32 b = 0; b < cfg.blockCount; b++) {
        u32 predCount = cfg.PredecessorCount(b);
        if (predCount < 2) continue;
        
        for (u32 pi = 0; pi < predCount; pi++) {
            u32 pred = cfg.GetPredecessor(b, pi);
            u32 runner = pred;
            
            while (runner != NO_BLOCK && runner != cfg.immediateDominators[b]) {
                u32 offset = cfg.dfOffsets[runner] + frontierCounts[runner];
                cfg.dfData[offset] = b;
                frontierCounts[runner]++;
                runner = cfg.immediateDominators[runner];
            }
        }
    }
    
    // Allocate actual counts array for tracking dedup results
    cfg.dfActualCounts = (u32*)arena->Allocate(cfg.blockCount * sizeof(u32), 64);

    // Step 4: Remove duplicates from each frontier set and track actual counts
    // (A block can appear multiple times via different predecessors)
    for (u32 b = 0; b < cfg.blockCount; b++) {
        u32 start = cfg.dfOffsets[b];
        u32 end = cfg.dfOffsets[b + 1];

        if (end - start <= 1) {
            // No dedup needed, actual count = original count
            cfg.dfActualCounts[b] = end - start;
            continue;
        }

        // Simple O(n²) dedup for small sets; fine for shader CFGs
        u32 writeIdx = start;
        for (u32 i = start; i < end; i++) {
            bool duplicate = false;
            for (u32 j = start; j < writeIdx; j++) {
                if (cfg.dfData[j] == cfg.dfData[i]) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                cfg.dfData[writeIdx++] = cfg.dfData[i];
            }
        }

        // Store the actual count after dedup
        cfg.dfActualCounts[b] = writeIdx - start;
    }

    cfg.frontiersComputed = true;
}

void CFGBuilder::ComputeDominatorTreeChildren() {
    if (cfg.domChildrenComputed) return;
    if (!cfg.dominatorsComputed) ComputeDominators();
    
    // Build dominator tree children lists for top-down traversal
    u32* childCounts = (u32*)arena->Allocate(cfg.blockCount * sizeof(u32), 64);
    memset(childCounts, 0, cfg.blockCount * sizeof(u32));
    
    // Count children - each block's idom is its parent
    for (u32 b = 0; b < cfg.blockCount; b++) {
        u32 parent = cfg.immediateDominators[b];
        // Exclude entry block (idom = self or NO_BLOCK)
        if (parent != NO_BLOCK && parent != b) {
            childCounts[parent]++;
        }
    }
    
    // Build offsets (prefix sum)
    cfg.domChildOffsets = (u32*)arena->Allocate((cfg.blockCount + 1) * sizeof(u32), 64);
    cfg.domChildOffsets[0] = 0;
    for (u32 b = 0; b < cfg.blockCount; b++) {
        cfg.domChildOffsets[b + 1] = cfg.domChildOffsets[b] + childCounts[b];
    }
    cfg.domChildCount = cfg.domChildOffsets[cfg.blockCount];
    
    // Allocate and fill children data
    if (cfg.domChildCount > 0) {
        cfg.domChildData = (u32*)arena->Allocate(cfg.domChildCount * sizeof(u32), 64);
    } else {
        cfg.domChildData = nullptr;
    }
    memset(childCounts, 0, cfg.blockCount * sizeof(u32));  // Reset for fill pass
    
    for (u32 b = 0; b < cfg.blockCount; b++) {
        u32 parent = cfg.immediateDominators[b];
        if (parent != NO_BLOCK && parent != b) {
            u32 offset = cfg.domChildOffsets[parent] + childCounts[parent]++;
            cfg.domChildData[offset] = b;
        }
    }
    
    cfg.domChildrenComputed = true;
}

void CFGBuilder::FindNaturalLoops() {
    // TODO: Implement when needed for loop optimization
}

void CFGBuilder::RecoverStructure() {
    // Populate mergeBlocks/continueBlocks from IR structure annotations
    // This reads from ir->structureInfo and ir->continueInfo which were
    // populated during IR lowering
    
    for (u32 b = 0; b < cfg.blockCount; b++) {
        u32 lastInst = cfg.lastInst[b];
        u32 info = ir->structureInfo[lastInst];
        
        if (info != 0) {
            u32 type = info & IR::IRProgram::STRUCT_TYPE_MASK;
            u32 mergeInst = info & IR::IRProgram::STRUCT_TARGET_MASK;
            
            // Convert instruction index to block index
            if (mergeInst < ir->instructionCount) {
                cfg.mergeBlocks[b] = cfg.instToBlock[mergeInst];
            }
            
            // For loop headers, also recover the continue target
            if (type == IR::IRProgram::STRUCT_LOOP_HEADER) {
                u32 contInst = ir->continueInfo[lastInst];
                if (contInst != 0xFFFFFFFF && contInst < ir->instructionCount) {
                    cfg.continueBlocks[b] = cfg.instToBlock[contInst];
                }
            }
        }
    }
}

} // namespace BWSL