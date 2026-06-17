#include "bwsl_ir_gen.h"
#include "phases/control_flow/bwsl_cfg.h"
#include <cstring>

namespace BWSL {
namespace IR {

void OptimizationPass::EliminateDeadBlocks(IRProgram* prog, BWSL::CFG* cfg) {
    if (cfg->blockCount == 0) return;
    
    // Allocate reachability array
    bool* reachable = (bool*)alloca(cfg->blockCount * sizeof(bool));
    memset(reachable, 0, cfg->blockCount);
    
    // BFS from entry block to find all reachable blocks
    u32* queue = (u32*)alloca(cfg->blockCount * sizeof(u32));
    u32 head = 0, tail = 0;
    
    queue[tail++] = cfg->entryBlock;
    reachable[cfg->entryBlock] = true;
    
    while (head < tail) {
        u32 b = queue[head++];
        // Use TotalSuccessorCount/GetAnySuccessor to handle both regular and switch blocks
        u32 succCount = cfg->TotalSuccessorCount(b);
        for (u32 s = 0; s < succCount; s++) {
            u32 succ = cfg->GetAnySuccessor(b, s);
            if (succ != NO_BLOCK && !reachable[succ]) {
                reachable[succ] = true;
                queue[tail++] = succ;
            }
        }
    }
    
    // Mark all instructions in unreachable blocks as NOP
    // They will be removed by the standard DCE pass
    for (u32 b = 0; b < cfg->blockCount; b++) {
        if (!reachable[b]) {
            for (u32 i = cfg->firstInst[b]; i <= cfg->lastInst[b]; i++) {
                prog->opcodes[i] = OP_NOP;
            }
        }
    }
    
    // Now run standard instruction-level DCE to remove the NOPs
    // and any other dead code
    EliminateDeadCode(prog);
}

} // namespace IR
} // namespace BWSL

