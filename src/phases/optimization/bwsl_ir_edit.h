#pragma once

#include "phases/control_flow/bwsl_cfg.h"
#include "phases/ir_generation/bwsl_ir_gen.h"

namespace BWSL {
namespace Optimization {

// Adds instructions to a program after lowering, CFG construction and SSA.
// A pass queues them while it works, since instruction indices must stay
// put until it's done, and applies them once at the end. Each goes in
// front of an existing instruction, in the same block; several in front of
// the same one keep the order they were queued in. A jump or branch to that
// instruction then lands on the first one inserted before it.

struct InsertedInstruction {
    u32 before;       // The existing instruction it goes in front of
    u16 opcode;
    u16 dest;
    u16 operands[4];
};

struct InstructionInsertions {
    InsertedInstruction* items;
    u32 count;
    u32 capacity;
    u32 nextRegister;   // Spare registers AllocateRegister hands out: [nextRegister, registerEnd)
    u32 registerEnd;
};

// A new register of `type`, or 0xFFFF past IR::RegisterLimit. Registers are
// added in batches, like SSA's: the program's registerCount includes spare
// ones of type VOID. Like SSA, it grows the metadata the backends read
// (types, struct types, storage info, names), not registerLifetimes or
// registerSpillSlots, which nothing reads after lowering.
u16 AllocateRegister(IR::IRBuilder* builder, InstructionInsertions* insertions, CoreType type);

// Queues `opcode dest <- a, b` in front of instruction `before`. Queue in
// ascending `before` order.
void QueueInstruction(IR::IRBuilder* builder, InstructionInsertions* insertions, u32 before,
                      u16 opcode, u16 dest, u16 a, u16 b = 0xFFFF);

// Inserts the queued instructions and updates every instruction index the
// program and `cfg` (null for straight-line stages) hold: branch, jump and
// switch targets, structured control flow annotations, block bounds and
// the instruction-to-block map.
void ApplyInsertions(IR::IRBuilder* builder, CFG* cfg, InstructionInsertions* insertions);

}  // namespace Optimization
}  // namespace BWSL
